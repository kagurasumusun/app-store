// SPDX-License-Identifier: MIT

#include "ipashelf/ipa.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
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
  for (const auto& [sinf_path, sinf_data] : options.sinf_entries) {
    if (!mz_zip_writer_add_mem_ex(&writer, sinf_path.c_str(), sinf_data.data(),
                                  sinf_data.size(), nullptr, 0,
                                  MZ_DEFAULT_COMPRESSION, 0, 0)) {
      const std::string message = zip_error(writer, "cannot write sinf");
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
