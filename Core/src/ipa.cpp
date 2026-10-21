// SPDX-License-Identifier: MIT

#include "ipashelf/ipa.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <utility>

#include "miniz.h"

namespace ipashelf {
namespace {

std::string zip_error(mz_zip_archive& archive, std::string_view context) {
  const mz_zip_error error = mz_zip_get_last_error(&archive);
  return std::string(context) + ": " + std::string(mz_zip_get_error_string(error));
}

std::vector<std::string> split_path(std::string_view path) {
  std::vector<std::string> parts;
  std::size_t start = 0;
  while (start <= path.size()) {
    const std::size_t end = path.find('/', start);
    if (end == std::string_view::npos) {
      if (start < path.size()) parts.emplace_back(path.substr(start));
      break;
    }
    if (end > start) parts.emplace_back(path.substr(start, end - start));
    start = end + 1;
  }
  return parts;
}

bool is_main_info_plist(std::string_view name) {
  const auto parts = split_path(name);
  if (parts.size() != 3 || parts[0] != "Payload") return false;
  if (!ends_with(parts[1], ".app")) return false;
  return parts[2] == "Info.plist";
}

bool matches_name(const char* entry_name, std::string_view candidate) {
  return entry_name != nullptr && candidate == entry_name;
}

Result<Bytes> extract_entry(mz_zip_archive& archive, mz_uint index,
                            std::uint64_t uncompressed_size,
                            std::string_view label) {
  if (uncompressed_size == 0 || uncompressed_size > 4 * 1024 * 1024) {
    return make_error(ErrorCode::kParse,
                      std::string(label) + " has an invalid size");
  }
  Bytes buffer(static_cast<std::size_t>(uncompressed_size));
  if (!mz_zip_reader_extract_to_mem(&archive, index, buffer.data(),
                                    buffer.size(), 0)) {
    return make_error(ErrorCode::kParse,
                      zip_error(archive, "cannot extract " +
                                             std::string(label)));
  }
  return buffer;
}

// ---------------------------------------------------------------------------
// Append-style patching
//
// The store package's own zip layout is load-bearing, because the on-device
// installer walks local file headers: entries whose local header carries
// zeroed CRC/sizes behind the data-descriptor bit are not what Apple's
// tooling ever ships, and a streaming reader can only pass them by
// decompressing blind. Patching a package the way Apple's tooling (and
// the established App Store clients) do it means appending: every original
// byte — local headers, compressed data, central directory records —
// stays exactly as the store wrote it, and only the injected files are
// new records, written with their full CRC and sizes in the local header
// and no data descriptor. miniz's in-memory writer always streams through
// a descriptor, so the appended records are serialised by hand here; miniz
// still provides the raw deflate stream and CRC.
// ---------------------------------------------------------------------------

std::uint16_t read_le16(const std::uint8_t* bytes) {
  return static_cast<std::uint16_t>(bytes[0] |
                                    (static_cast<std::uint16_t>(bytes[1])
                                     << 8));
}

std::uint32_t read_le32(const std::uint8_t* bytes) {
  return static_cast<std::uint32_t>(bytes[0]) |
         (static_cast<std::uint32_t>(bytes[1]) << 8) |
         (static_cast<std::uint32_t>(bytes[2]) << 16) |
         (static_cast<std::uint32_t>(bytes[3]) << 24);
}

std::uint64_t read_le64(const std::uint8_t* bytes) {
  return static_cast<std::uint64_t>(read_le32(bytes)) |
         (static_cast<std::uint64_t>(read_le32(bytes + 4)) << 32);
}

void write_le16(Bytes& out, std::uint16_t value) {
  out.push_back(static_cast<std::uint8_t>(value & 0xff));
  out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
}

void write_le32(Bytes& out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xff));
  }
}

void write_le64(Bytes& out, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xff));
  }
}

void patch_le64(Bytes& out, std::size_t offset, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    out[offset + static_cast<std::size_t>(shift / 8)] =
        static_cast<std::uint8_t>((value >> shift) & 0xff);
  }
}

struct DosDateTime {
  std::uint16_t time = 0;
  std::uint16_t date = 0;
};

DosDateTime current_dos_datetime() {
  const std::time_t now = std::time(nullptr);
  std::tm local{};
#if defined(_WIN32)
  localtime_s(&local, &now);
#else
  localtime_r(&now, &local);
#endif
  const int year = std::max(1980, std::min(2107, local.tm_year + 1900));
  DosDateTime value;
  value.date = static_cast<std::uint16_t>(
      ((year - 1980) << 9) | ((local.tm_mon + 1) << 5) | local.tm_mday);
  value.time = static_cast<std::uint16_t>(
      (local.tm_hour << 11) | (local.tm_min << 5) | (local.tm_sec / 2));
  return value;
}

// Raw DEFLATE (no zlib wrapper) — the form zip entries store.
Result<Bytes> deflate_raw(const Bytes& input) {
  mz_stream stream{};
  if (mz_deflateInit2(&stream, MZ_DEFAULT_COMPRESSION, MZ_DEFLATED, -15, 8,
                      MZ_DEFAULT_STRATEGY) != MZ_OK) {
    return make_error(ErrorCode::kInternal, "compression could not start");
  }
  Bytes output;
  output.resize(
      mz_deflateBound(&stream, static_cast<mz_ulong>(input.size())));
  stream.next_in = input.data();
  stream.avail_in = static_cast<unsigned int>(input.size());
  stream.next_out = output.data();
  stream.avail_out = static_cast<unsigned int>(output.size());
  const int status = mz_deflate(&stream, MZ_FINISH);
  const std::size_t produced = static_cast<std::size_t>(stream.total_out);
  mz_deflateEnd(&stream);
  if (status != MZ_STREAM_END) {
    return make_error(ErrorCode::kInternal, "compression failed");
  }
  output.resize(produced);
  return output;
}

struct ZipTailLayout {
  std::uint64_t eocd_offset = 0;
  std::uint64_t cd_offset = 0;
  std::uint64_t cd_size = 0;
  std::uint64_t total_entries = 0;
  std::string comment;
  bool zip64 = false;
  Bytes zip64_record;  // verbatim source record, patched on the way out
};

// Locates the end-of-central-directory structures. Returns nothing for
// layouts the append writer must not touch (multi-disk, detached
// structures between the central directory and the tail records, sentinel
// values without the zip64 record they point to).
std::optional<ZipTailLayout> read_zip_tail(std::ifstream& input,
                                           std::uint64_t file_size) {
  if (file_size < 22) return std::nullopt;
  const std::uint64_t tail_size =
      std::min<std::uint64_t>(file_size, 65536 + 22);
  std::vector<std::uint8_t> tail(static_cast<std::size_t>(tail_size));
  input.seekg(static_cast<std::streamoff>(file_size - tail_size));
  if (!input.read(reinterpret_cast<char*>(tail.data()),
                  static_cast<std::streamsize>(tail.size()))) {
    return std::nullopt;
  }
  const std::uint64_t tail_start = file_size - tail_size;

  std::optional<std::size_t> eocd_at;
  for (std::size_t index = tail.size() - 22;; --index) {
    if (read_le32(&tail[index]) == 0x06054b50) {
      const std::uint16_t comment_length = read_le16(&tail[index + 20]);
      if (tail_start + index + 22 + comment_length == file_size) {
        eocd_at = index;
        break;
      }
    }
    if (index == 0) break;
  }
  if (!eocd_at) return std::nullopt;
  const std::uint8_t* eocd = &tail[*eocd_at];

  ZipTailLayout layout;
  layout.eocd_offset = tail_start + *eocd_at;
  if (read_le16(eocd + 4) != 0 || read_le16(eocd + 6) != 0) {
    return std::nullopt;  // multi-disk archive
  }
  layout.comment.assign(reinterpret_cast<const char*>(eocd + 22),
                        read_le16(eocd + 20));

  // The zip64 locator, when present, sits directly before the EOCD.
  if (layout.eocd_offset >= 20) {
    const std::uint64_t locator_at = layout.eocd_offset - 20;
    std::uint8_t locator[20];
    input.seekg(static_cast<std::streamoff>(locator_at));
    if (input.read(reinterpret_cast<char*>(locator), 20) &&
        read_le32(locator) == 0x07064b50) {
      if (read_le32(locator + 16) != 1) return std::nullopt;
      const std::uint64_t record_at = read_le64(locator + 8);
      std::uint8_t record_head[12];
      input.seekg(static_cast<std::streamoff>(record_at));
      if (!input.read(reinterpret_cast<char*>(record_head), 12) ||
          read_le32(record_head) != 0x06064b50) {
        return std::nullopt;
      }
      const std::uint64_t record_size = 12 + read_le64(record_head + 4);
      if (record_size < 56 || record_at + record_size != locator_at) {
        return std::nullopt;
      }
      layout.zip64_record.assign(record_head, record_head + 12);
      layout.zip64_record.resize(static_cast<std::size_t>(record_size));
      input.seekg(static_cast<std::streamoff>(record_at + 12));
      if (!input.read(
              reinterpret_cast<char*>(layout.zip64_record.data() + 12),
              static_cast<std::streamsize>(record_size - 12))) {
        return std::nullopt;
      }
      const std::uint8_t* record = layout.zip64_record.data();
      if (read_le32(record + 16) != 0 || read_le32(record + 20) != 0) {
        return std::nullopt;  // multi-disk archive
      }
      layout.zip64 = true;
      layout.total_entries = read_le64(record + 32);
      layout.cd_size = read_le64(record + 40);
      layout.cd_offset = read_le64(record + 48);
      if (layout.cd_offset + layout.cd_size != record_at) {
        return std::nullopt;
      }
      return layout;
    }
  }

  layout.total_entries = read_le16(eocd + 10);
  layout.cd_size = read_le32(eocd + 12);
  layout.cd_offset = read_le32(eocd + 16);
  if (layout.total_entries == 0xffff || layout.cd_size == 0xffffffffu ||
      layout.cd_offset == 0xffffffffu) {
    return std::nullopt;  // zip64 sentinels without the record
  }
  if (layout.cd_offset + layout.cd_size != layout.eocd_offset) {
    return std::nullopt;
  }
  return layout;
}

// Reads the central directory whole — it is written back verbatim — and
// collects the entry names. Anything malformed falls back to the rewrite.
std::optional<std::vector<std::string>> read_central_names(
    std::ifstream& input, const ZipTailLayout& layout, Bytes& cd_bytes) {
  if (layout.cd_size > (std::uint64_t{1} << 28)) return std::nullopt;
  cd_bytes.assign(static_cast<std::size_t>(layout.cd_size), 0);
  input.seekg(static_cast<std::streamoff>(layout.cd_offset));
  if (layout.cd_size > 0 &&
      !input.read(reinterpret_cast<char*>(cd_bytes.data()),
                  static_cast<std::streamsize>(cd_bytes.size()))) {
    return std::nullopt;
  }
  std::vector<std::string> names;
  std::size_t at = 0;
  while (at < cd_bytes.size()) {
    if (cd_bytes.size() - at < 46 ||
        read_le32(&cd_bytes[at]) != 0x02014b50) {
      return std::nullopt;
    }
    const std::uint16_t name_length = read_le16(&cd_bytes[at + 28]);
    const std::size_t following =
        static_cast<std::size_t>(read_le16(&cd_bytes[at + 28])) +
        read_le16(&cd_bytes[at + 30]) + read_le16(&cd_bytes[at + 32]);
    if (cd_bytes.size() - at - 46 < following) return std::nullopt;
    names.emplace_back(reinterpret_cast<const char*>(&cd_bytes[at + 46]),
                       name_length);
    at += 46 + following;
  }
  if (names.size() != layout.total_entries) return std::nullopt;
  return names;
}

struct PreparedEntry {
  std::string name;
  Bytes compressed;
  std::uint32_t crc = 0;
  std::uint32_t uncompressed = 0;
  std::uint64_t local_offset = 0;
};

// Appends the sinf entries (and the metadata document when the source
// lacks one) to a copy of the source archive, preserving all original
// bytes. Returns nothing when the source layout needs the rewrite path
// instead (an injected name already exists, nonstandard tail, ...).
std::optional<Status> append_entries_to_ipa(
    const IpaPatchOptions& options, const Bytes& metadata_document,
    const CancellationToken& token) {
  const std::string temporary = options.destination_path + ".partial";
  struct Cleanup {
    std::string path;
    bool keep = false;
    ~Cleanup() {
      if (!keep) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
      }
    }
  } cleanup{temporary};

  std::ifstream input(options.source_path,
                      std::ios::binary | std::ios::ate);
  if (!input) return fail(ErrorCode::kParse, "cannot open source IPA");
  const std::uint64_t file_size = static_cast<std::uint64_t>(input.tellg());
  auto layout = read_zip_tail(input, file_size);
  if (!layout) return std::nullopt;
  Bytes central;
  auto names = read_central_names(input, layout.value(), central);
  if (!names) return std::nullopt;
  const auto exists = [&](const std::string& name) {
    return std::find(names->begin(), names->end(), name) != names->end();
  };
  // Appending only adds; replacing an existing entry is the rewrite
  // path's job (the domain the reference patcher refuses outright).
  for (const auto& [path, data] : options.sinf_entries) {
    (void)data;
    if (exists(path)) return std::nullopt;
  }

  std::vector<std::pair<std::string, Bytes>> additions(
      options.sinf_entries);
  if (!exists("iTunesMetadata.plist")) {
    additions.emplace_back("iTunesMetadata.plist", metadata_document);
  }

  std::vector<PreparedEntry> prepared;
  for (const auto& [name, data] : additions) {
    if (data.size() >= 0xffffffffu || name.size() > 0xffff) {
      return std::nullopt;
    }
    auto compressed = deflate_raw(data);
    if (!compressed) return compressed.error();
    if (compressed.value().size() >= 0xffffffffu) return std::nullopt;
    PreparedEntry entry;
    entry.name = name;
    entry.crc = static_cast<std::uint32_t>(
        mz_crc32(MZ_CRC32_INIT, data.data(), data.size()));
    entry.uncompressed = static_cast<std::uint32_t>(data.size());
    entry.compressed = std::move(compressed).value();
    prepared.push_back(std::move(entry));
  }

  std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
  if (!output) return fail(ErrorCode::kStorage, "cannot create output IPA");
  const auto write_all = [&](const std::uint8_t* bytes, std::size_t count) {
    output.write(reinterpret_cast<const char*>(bytes),
                 static_cast<std::streamsize>(count));
    return output.good();
  };

  // Everything before the central directory, byte for byte.
  input.seekg(0);
  std::uint64_t remaining = layout->cd_offset;
  std::vector<char> buffer(1 << 20);
  while (remaining > 0) {
    if (token.cancelled()) {
      return fail(ErrorCode::kCancelled, "IPA patch cancelled");
    }
    const std::streamsize chunk = static_cast<std::streamsize>(
        std::min<std::uint64_t>(remaining, buffer.size()));
    if (!input.read(buffer.data(), chunk)) {
      return fail(ErrorCode::kParse, "cannot read source IPA");
    }
    if (!write_all(reinterpret_cast<const std::uint8_t*>(buffer.data()),
                   static_cast<std::size_t>(chunk))) {
      return fail(ErrorCode::kStorage, "cannot write output IPA");
    }
    remaining -= static_cast<std::uint64_t>(chunk);
  }
  std::uint64_t position = layout->cd_offset;
  const DosDateTime stamp = current_dos_datetime();

  // The injected files, in the record form the store itself uses:
  // complete CRC and sizes in the local header, no data descriptor.
  for (auto& entry : prepared) {
    if (token.cancelled()) {
      return fail(ErrorCode::kCancelled, "IPA patch cancelled");
    }
    entry.local_offset = position;
    Bytes header;
    write_le32(header, 0x04034b50);
    write_le16(header, 20);      // version needed to extract
    write_le16(header, 0x0800);  // UTF-8 name; never a data descriptor
    write_le16(header, 8);       // deflate
    write_le16(header, stamp.time);
    write_le16(header, stamp.date);
    write_le32(header, entry.crc);
    write_le32(header, static_cast<std::uint32_t>(entry.compressed.size()));
    write_le32(header, entry.uncompressed);
    write_le16(header, static_cast<std::uint16_t>(entry.name.size()));
    write_le16(header, 0);  // extra field length
    header.insert(header.end(), entry.name.begin(), entry.name.end());
    if (!write_all(header.data(), header.size()) ||
        !write_all(entry.compressed.data(), entry.compressed.size())) {
      return fail(ErrorCode::kStorage, "cannot write output IPA");
    }
    position += header.size() + entry.compressed.size();
  }

  // The original central directory, verbatim, then the new records.
  const std::uint64_t new_cd_offset = position;
  if (!central.empty() && !write_all(central.data(), central.size())) {
    return fail(ErrorCode::kStorage, "cannot write output IPA");
  }
  position += central.size();
  std::uint64_t added_central_size = 0;
  for (const auto& entry : prepared) {
    const bool offset_needs_zip64 = entry.local_offset >= 0xffffffffu;
    Bytes record;
    write_le32(record, 0x02014b50);
    write_le16(record, 789);  // version made by: Unix, 2.1
    write_le16(record, 20);   // version needed to extract
    write_le16(record, 0x0800);
    write_le16(record, 8);
    write_le16(record, stamp.time);
    write_le16(record, stamp.date);
    write_le32(record, entry.crc);
    write_le32(record, static_cast<std::uint32_t>(entry.compressed.size()));
    write_le32(record, entry.uncompressed);
    write_le16(record, static_cast<std::uint16_t>(entry.name.size()));
    write_le16(record, offset_needs_zip64 ? 12 : 0);
    write_le16(record, 0);  // comment length
    write_le16(record, 0);  // disk number start
    write_le16(record, 0);  // internal attributes
    write_le32(record, 0x81a40000u);  // regular file, 0644
    write_le32(record, offset_needs_zip64
                           ? 0xffffffffu
                           : static_cast<std::uint32_t>(entry.local_offset));
    record.insert(record.end(), entry.name.begin(), entry.name.end());
    if (offset_needs_zip64) {
      write_le16(record, 0x0001);
      write_le16(record, 8);
      write_le64(record, entry.local_offset);
    }
    if (!write_all(record.data(), record.size())) {
      return fail(ErrorCode::kStorage, "cannot write output IPA");
    }
    added_central_size += record.size();
    position += record.size();
  }

  const std::uint64_t new_cd_size = layout->cd_size + added_central_size;
  const std::uint64_t new_total = layout->total_entries + prepared.size();
  const bool need_zip64 = layout->zip64 || new_total >= 0xffff ||
                          new_cd_size >= 0xffffffffull ||
                          new_cd_offset >= 0xffffffffull;
  if (need_zip64) {
    Bytes record;
    if (layout->zip64) {
      record = layout->zip64_record;
      patch_le64(record, 24, new_total);
      patch_le64(record, 32, new_total);
      patch_le64(record, 40, new_cd_size);
      patch_le64(record, 48, new_cd_offset);
    } else {
      write_le32(record, 0x06064b50);
      write_le64(record, 44);
      write_le16(record, 789);
      write_le16(record, 45);
      write_le32(record, 0);
      write_le32(record, 0);
      write_le64(record, new_total);
      write_le64(record, new_total);
      write_le64(record, new_cd_size);
      write_le64(record, new_cd_offset);
    }
    const std::uint64_t record_offset = position;
    if (!write_all(record.data(), record.size())) {
      return fail(ErrorCode::kStorage, "cannot write output IPA");
    }
    Bytes locator;
    write_le32(locator, 0x07064b50);
    write_le32(locator, 0);
    write_le64(locator, record_offset);
    write_le32(locator, 1);
    if (!write_all(locator.data(), locator.size())) {
      return fail(ErrorCode::kStorage, "cannot write output IPA");
    }
  }
  Bytes eocd;
  write_le32(eocd, 0x06054b50);
  write_le16(eocd, 0);
  write_le16(eocd, 0);
  const std::uint16_t count16 =
      new_total >= 0xffff ? 0xffff
                          : static_cast<std::uint16_t>(new_total);
  write_le16(eocd, count16);
  write_le16(eocd, count16);
  write_le32(eocd, new_cd_size >= 0xffffffffull
                       ? 0xffffffffu
                       : static_cast<std::uint32_t>(new_cd_size));
  write_le32(eocd, new_cd_offset >= 0xffffffffull
                       ? 0xffffffffu
                       : static_cast<std::uint32_t>(new_cd_offset));
  write_le16(eocd, static_cast<std::uint16_t>(layout->comment.size()));
  eocd.insert(eocd.end(), layout->comment.begin(), layout->comment.end());
  if (!write_all(eocd.data(), eocd.size())) {
    return fail(ErrorCode::kStorage, "cannot write output IPA");
  }
  output.flush();
  if (!output) return fail(ErrorCode::kStorage, "cannot write output IPA");
  output.close();

  std::error_code ignored;
  std::filesystem::rename(temporary, options.destination_path, ignored);
  if (ignored) {
    std::error_code remove_error;
    std::filesystem::remove(options.destination_path, remove_error);
    std::filesystem::rename(temporary, options.destination_path, ignored);
  }
  if (ignored) {
    return fail(ErrorCode::kStorage, "cannot move patched IPA into place");
  }
  cleanup.keep = true;
  return ok_status();
}

}  // namespace

Result<IpaInfo> inspect_ipa(const std::string& path) {
  mz_zip_archive archive{};
  if (!mz_zip_reader_init_file(&archive, path.c_str(), 0)) {
    return make_error(ErrorCode::kParse,
                      zip_error(archive, "cannot open IPA archive"));
  }
  struct Guard {
    mz_zip_archive* archive;
    ~Guard() { mz_zip_reader_end(archive); }
  } guard{&archive};

  IpaInfo info;
  std::error_code size_error;
  info.file_size = std::filesystem::file_size(path, size_error);
  const mz_uint count = mz_zip_reader_get_num_files(&archive);

  bool found_info = false;
  for (mz_uint index = 0; index < count && !found_info; ++index) {
    mz_zip_archive_file_stat stat{};
    if (!mz_zip_reader_file_stat(&archive, index, &stat)) continue;
    if (!is_main_info_plist(stat.m_filename)) continue;
    const auto parts = split_path(stat.m_filename);
    auto buffer = extract_entry(archive, index, stat.m_uncomp_size,
                                "IPA Info.plist");
    if (!buffer) return buffer.error();
    auto plist = PlistValue::parse(buffer.value());
    if (!plist) return plist.error();
    if (!plist.value().is_dictionary()) {
      return make_error(ErrorCode::kParse, "IPA Info.plist is not a dictionary");
    }
    const PlistValue& document = plist.value();
    info.bundle_id = document.string_or("CFBundleIdentifier");
    info.display_version = document.string_or("CFBundleShortVersionString");
    info.build_version = document.string_or("CFBundleVersion");
    info.executable = document.string_or("CFBundleExecutable");
    info.app_directory = "Payload/" + parts[1] + "/";
    info.supported_platforms.clear();
    const PlistValue* platforms = document.find("CFBundleSupportedPlatforms");
    info.declares_supported_platforms = platforms != nullptr;
    if (platforms != nullptr && platforms->is_array()) {
      for (const auto& entry : platforms->array_items()) {
        if (entry.is_string()) {
          info.supported_platforms.push_back(entry.string_value());
        }
      }
    }
    if (info.bundle_id.empty() || info.executable.empty()) {
      return make_error(ErrorCode::kParse,
                        "IPA Info.plist is missing the bundle identity");
    }
    found_info = true;
  }
  if (!found_info) {
    return make_error(ErrorCode::kParse, "IPA does not contain Payload/*.app");
  }

  // The SC_Info manifest, when present, authoritatively lists where each
  // FairPlay sinf blob belongs inside the bundle. Its paths are relative to
  // the .app directory.
  const std::string manifest_path =
      info.app_directory + "SC_Info/Manifest.plist";
  for (mz_uint index = 0; index < count; ++index) {
    mz_zip_archive_file_stat stat{};
    if (!mz_zip_reader_file_stat(&archive, index, &stat)) continue;
    if (!matches_name(stat.m_filename, manifest_path)) continue;
    auto buffer = extract_entry(archive, index, stat.m_uncomp_size,
                                "IPA SC_Info manifest");
    if (!buffer) return buffer.error();
    auto plist = PlistValue::parse(buffer.value());
    if (!plist) return plist.error();
    if (!plist.value().is_dictionary()) {
      return make_error(ErrorCode::kParse,
                        "IPA SC_Info manifest is not a dictionary");
    }
    const PlistValue* paths = plist.value().find("SinfPaths");
    if (paths != nullptr && paths->is_array()) {
      for (const auto& entry : paths->array_items()) {
        if (entry.is_string() && !entry.string_value().empty()) {
          info.sinf_paths.push_back(info.app_directory +
                                    entry.string_value());
        }
      }
    }
    break;
  }

  // Record the FairPlay files the archive actually carries, separate from
  // the manifest's expectations: comparing the two is how a package that
  // would die at iOS's FairPlay check is spotted before it is served.
  // Each encrypted binary keeps its sinf in its own SC_Info directory
  // (main app, frameworks, extensions), so match any of them.
  for (mz_uint index = 0; index < count; ++index) {
    mz_zip_archive_file_stat stat{};
    if (!mz_zip_reader_file_stat(&archive, index, &stat)) continue;
    const std::string_view name(stat.m_filename);
    if (name.rfind(info.app_directory, 0) == 0 &&
        name.find("/SC_Info/") != std::string_view::npos &&
        ends_with(name, ".sinf")) {
      info.sinf_files.emplace_back(std::string(name), stat.m_uncomp_size);
    }
  }

  return info;
}

std::string default_sinf_path(const IpaInfo& info) {
  if (info.executable.empty()) return {};
  return info.app_directory + "SC_Info/" + info.executable + ".sinf";
}

Result<std::vector<std::pair<std::string, Bytes>>> resolve_sinf_entries(
    const IpaInfo& info, const std::vector<Sinf>& sinfs) {
  std::vector<std::pair<std::string, Bytes>> entries;
  if (sinfs.empty()) return entries;
  if (!info.sinf_paths.empty()) {
    if (info.sinf_paths.size() != sinfs.size()) {
      return make_error(
          ErrorCode::kParse,
          "download response and package manifest disagree on the number "
          "of sinf entries");
    }
    entries.reserve(sinfs.size());
    for (std::size_t index = 0; index < sinfs.size(); ++index) {
      entries.emplace_back(info.sinf_paths[index], sinfs[index].data);
    }
    return entries;
  }
  const std::string fallback = default_sinf_path(info);
  if (fallback.empty()) {
    return make_error(ErrorCode::kParse,
                      "IPA does not declare an executable for the sinf path");
  }
  entries.emplace_back(fallback, sinfs.front().data);
  return entries;
}

Status validate_ipa_platform(const IpaInfo& info, Platform platform) {
  std::string_view expected;
  switch (platform) {
    case Platform::kIPhone:
    case Platform::kIPad:
      expected = "iPhoneOS";
      break;
    case Platform::kAppleTV:
      expected = "AppleTVOS";
      break;
    case Platform::kWatchOS:
      expected = "WatchOS";
      break;
    case Platform::kVisionOS:
      expected = "XROS";
      break;
    default:
      return ok_status();
  }
  // Legacy iOS apps may predate CFBundleSupportedPlatforms. Only a missing
  // declaration qualifies; an empty or conflicting one does not.
  if (expected == "iPhoneOS" && !info.declares_supported_platforms) {
    return ok_status();
  }
  if (std::find(info.supported_platforms.begin(),
                info.supported_platforms.end(),
                expected) != info.supported_platforms.end()) {
    return ok_status();
  }
  return fail(ErrorCode::kUnsupported,
              "downloaded package does not declare " + std::string(expected) +
                  " support");
}

Status write_patched_ipa(const IpaPatchOptions& options,
                         const CancellationToken& token) {
  if (options.source_path.empty() || options.destination_path.empty()) {
    return fail(ErrorCode::kInvalidArgument, "IPA paths are required");
  }
  for (const auto& [sinf_path, sinf_data] : options.sinf_entries) {
    if (sinf_path.empty() || sinf_data.empty()) {
      return fail(ErrorCode::kInvalidArgument,
                  "sinf entries require both a path and a payload");
    }
  }

  // A store package carries neither of the injected files yet, so the
  // normal case patches by appending and keeps the package byte for byte
  // outside the additions. Layouts that need an existing entry replaced
  // fall through to the full rewrite below.
  if (!options.artwork.has_value() || options.artwork->empty()) {
    if (auto appended = append_entries_to_ipa(
            options, options.metadata.dump_binary(), token)) {
      return appended.value();
    }
  }

  mz_zip_archive reader{};
  if (!mz_zip_reader_init_file(&reader, options.source_path.c_str(), 0)) {
    return fail(ErrorCode::kParse,
                zip_error(reader, "cannot open source IPA"));
  }

  const std::string temporary = options.destination_path + ".partial";
  std::error_code ignored;
  std::filesystem::remove(temporary, ignored);

  mz_zip_archive writer{};
  if (!mz_zip_writer_init_file(&writer, temporary.c_str(), 0)) {
    const std::string message = zip_error(writer, "cannot create output IPA");
    mz_zip_reader_end(&reader);
    return fail(ErrorCode::kStorage, message);
  }

  const auto cleanup = [&]() {
    mz_zip_writer_end(&writer);
    mz_zip_reader_end(&reader);
    std::filesystem::remove(temporary, ignored);
  };

  const auto is_sinf_target = [&](const char* entry_name) {
    for (const auto& [sinf_path, sinf_data] : options.sinf_entries) {
      (void)sinf_data;
      if (matches_name(entry_name, sinf_path)) return true;
    }
    return false;
  };

  // The finished package only gains a metadata document when the store
  // package lacks one. An existing document (and any root artwork) is
  // carried over untouched, exactly like every other entry.
  bool source_has_metadata = false;
  const mz_uint count = mz_zip_reader_get_num_files(&reader);
  for (mz_uint index = 0; index < count; ++index) {
    mz_zip_archive_file_stat stat{};
    if (mz_zip_reader_file_stat(&reader, index, &stat) &&
        matches_name(stat.m_filename, "iTunesMetadata.plist")) {
      source_has_metadata = true;
      break;
    }
  }
  const bool replace_artwork =
      options.artwork.has_value() && !options.artwork->empty();

  for (mz_uint index = 0; index < count; ++index) {
    if (token.cancelled()) {
      cleanup();
      return fail(ErrorCode::kCancelled, "IPA patch cancelled");
    }
    mz_zip_archive_file_stat stat{};
    if (!mz_zip_reader_file_stat(&reader, index, &stat)) {
      const std::string message = zip_error(reader, "cannot read IPA entry");
      cleanup();
      return fail(ErrorCode::kParse, message);
    }
    if (replace_artwork &&
        (matches_name(stat.m_filename, "iTunesArtwork") ||
         matches_name(stat.m_filename, "iTunesArtwork@2x"))) {
      continue;
    }
    if (is_sinf_target(stat.m_filename)) continue;
    if (!mz_zip_writer_add_from_zip_reader(&writer, &reader, index)) {
      const std::string message = zip_error(writer, "cannot copy IPA entry");
      cleanup();
      return fail(ErrorCode::kStorage, message);
    }
  }

  for (const auto& [sinf_path, sinf_data] : options.sinf_entries) {
    if (!mz_zip_writer_add_mem_ex(&writer, sinf_path.c_str(), sinf_data.data(),
                                  sinf_data.size(), nullptr, 0,
                                  MZ_DEFAULT_COMPRESSION, 0, 0)) {
      const std::string message = zip_error(writer, "cannot write sinf");
      cleanup();
      return fail(ErrorCode::kStorage, message);
    }
  }
  if (!source_has_metadata) {
    // Apple's own tooling writes the metadata document as a binary plist.
    const Bytes metadata = options.metadata.dump_binary();
    if (!mz_zip_writer_add_mem_ex(&writer, "iTunesMetadata.plist",
                                  metadata.data(), metadata.size(), nullptr,
                                  0, MZ_DEFAULT_COMPRESSION, 0, 0)) {
      const std::string message = zip_error(writer, "cannot write metadata");
      cleanup();
      return fail(ErrorCode::kStorage, message);
    }
  }
  if (options.artwork.has_value() && !options.artwork->empty()) {
    if (!mz_zip_writer_add_mem_ex(&writer, "iTunesArtwork",
                                  options.artwork->data(),
                                  options.artwork->size(), nullptr, 0,
                                  MZ_DEFAULT_COMPRESSION, 0, 0)) {
      const std::string message = zip_error(writer, "cannot write artwork");
      cleanup();
      return fail(ErrorCode::kStorage, message);
    }
  }

  if (!mz_zip_writer_finalize_archive(&writer) ||
      !mz_zip_writer_end(&writer)) {
    const std::string message = zip_error(writer, "cannot finalize IPA");
    mz_zip_reader_end(&reader);
    std::filesystem::remove(temporary, ignored);
    return fail(ErrorCode::kStorage, message);
  }
  mz_zip_reader_end(&reader);

  std::filesystem::rename(temporary, options.destination_path, ignored);
  if (ignored) {
    std::error_code remove_error;
    std::filesystem::remove(options.destination_path, remove_error);
    std::filesystem::rename(temporary, options.destination_path, ignored);
  }
  if (ignored) {
    std::error_code remove_error;
    std::filesystem::remove(temporary, remove_error);
    return fail(ErrorCode::kStorage, "cannot move patched IPA into place");
  }
  return ok_status();
}

Status verify_ipa_fairplay(
    const std::string& path,
    const std::vector<std::pair<std::string, Bytes>>& expected) {
  auto info = inspect_ipa(path);
  if (!info) return info.error();

  if (expected.empty()) {
    // Nothing was injected, so the archive must stand on its own: any
    // declared FairPlay slot left empty means the response supplied no
    // sinf data for a package that needs it.
    if (info.value().sinf_paths.empty() && info.value().sinf_files.empty()) {
      return ok_status();
    }
    if (info.value().sinf_paths.empty()) {
      // Files exist without a manifest; they are whatever the store
      // package carried. Non-empty is all that can be checked here.
      for (const auto& [file, size] : info.value().sinf_files) {
        if (size == 0) {
          return fail(ErrorCode::kParse,
                      "the package carries an empty FairPlay file: " + file);
        }
      }
      return ok_status();
    }
  }

  mz_zip_archive archive{};
  if (!mz_zip_reader_init_file(&archive, path.c_str(), 0)) {
    return fail(ErrorCode::kParse,
                zip_error(archive, "cannot reopen patched IPA"));
  }
  struct Guard {
    mz_zip_archive* archive;
    ~Guard() { mz_zip_reader_end(archive); }
  } guard{&archive};

  const auto find_index = [&](const std::string& name) -> int {
    const mz_uint count = mz_zip_reader_get_num_files(&archive);
    for (mz_uint index = 0; index < count; ++index) {
      mz_zip_archive_file_stat stat{};
      if (mz_zip_reader_file_stat(&archive, index, &stat) &&
          matches_name(stat.m_filename, name)) {
        return static_cast<int>(index);
      }
    }
    return -1;
  };

  // Every declared slot must be filled, by an injected blob or by
  // content the package already carried.
  for (const std::string& slot : info.value().sinf_paths) {
    const int index = find_index(slot);
    if (index < 0) {
      return fail(ErrorCode::kParse,
                  "the package is missing its FairPlay file: " + slot);
    }
    mz_zip_archive_file_stat stat{};
    if (!mz_zip_reader_file_stat(&archive, static_cast<mz_uint>(index),
                                 &stat) ||
        stat.m_uncomp_size == 0) {
      return fail(ErrorCode::kParse,
                  "the package carries an empty FairPlay file: " + slot);
    }
  }

  // Injected blobs must have landed byte-for-byte.
  for (const auto& [name, blob] : expected) {
    const int index = find_index(name);
    if (index < 0) {
      return fail(ErrorCode::kParse,
                  "the patched package is missing FairPlay file: " + name);
    }
    mz_zip_archive_file_stat stat{};
    if (!mz_zip_reader_file_stat(&archive, static_cast<mz_uint>(index),
                                 &stat)) {
      return fail(ErrorCode::kParse, "cannot stat FairPlay file: " + name);
    }
    auto stored = extract_entry(archive, static_cast<mz_uint>(index),
                                stat.m_uncomp_size, "patched sinf");
    if (!stored) return stored.error();
    if (stored.value() != blob) {
      return fail(ErrorCode::kParse,
                  "patched FairPlay data does not match the download "
                  "response: " +
                      name);
    }
  }
  return ok_status();
}

PlistValue build_itunes_metadata(const DownloadOffer& offer,
                                 const Account& account) {
  // The server-provided metadata already carries the item identity; the
  // only additions Apple's installers expect are the purchasing account.
  PlistValue metadata = offer.metadata.is_dictionary()
                            ? offer.metadata
                            : PlistValue::dictionary();
  if (!account.email.empty()) {
    metadata.set("apple-id", PlistValue::string(account.email));
    metadata.set("userName", PlistValue::string(account.email));
  }
  return metadata;
}

}  // namespace ipashelf
