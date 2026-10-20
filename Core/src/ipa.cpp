// SPDX-License-Identifier: MIT

#include "ipashelf/ipa.hpp"

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

bool is_root_metadata(std::string_view name) {
  return name == "iTunesMetadata.plist" || name == "iTunesArtwork" ||
         name == "iTunesArtwork@2x";
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
  for (mz_uint index = 0; index < count; ++index) {
    mz_zip_archive_file_stat stat{};
    if (!mz_zip_reader_file_stat(&archive, index, &stat)) continue;
    if (!is_main_info_plist(stat.m_filename)) continue;
    const auto parts = split_path(stat.m_filename);
    if (stat.m_uncomp_size == 0 || stat.m_uncomp_size > 4 * 1024 * 1024) {
      return make_error(ErrorCode::kParse, "IPA Info.plist has an invalid size");
    }
    Bytes buffer(static_cast<std::size_t>(stat.m_uncomp_size));
    if (!mz_zip_reader_extract_to_mem(&archive, index, buffer.data(),
                                      buffer.size(), 0)) {
      return make_error(ErrorCode::kParse,
                        zip_error(archive, "cannot extract IPA Info.plist"));
    }
    auto plist = PlistValue::parse(buffer);
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
    return info;
  }
  return make_error(ErrorCode::kParse, "IPA does not contain Payload/*.app");
}

std::string default_sinf_path(const IpaInfo& info) {
  if (info.executable.empty()) return {};
  return info.app_directory + "SC_Info/" + info.executable + ".sinf";
}

Status write_patched_ipa(const IpaPatchOptions& options,
                         const CancellationToken& token) {
  if (options.source_path.empty() || options.destination_path.empty()) {
    return fail(ErrorCode::kInvalidArgument, "IPA paths are required");
  }
  if (!options.sinf.empty() && options.sinf_path.empty()) {
    return fail(ErrorCode::kInvalidArgument,
                "a sinf payload requires a sinf path");
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

  const mz_uint count = mz_zip_reader_get_num_files(&reader);
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
    if (is_root_metadata(stat.m_filename)) continue;
    if (!options.sinf.empty() && matches_name(stat.m_filename, options.sinf_path)) {
      continue;
    }
    if (!mz_zip_writer_add_from_zip_reader(&writer, &reader, index)) {
      const std::string message = zip_error(writer, "cannot copy IPA entry");
      cleanup();
      return fail(ErrorCode::kStorage, message);
    }
  }

  const std::string metadata = options.metadata.dump_xml();
  if (!mz_zip_writer_add_mem_ex(&writer, "iTunesMetadata.plist", metadata.data(),
                                metadata.size(), nullptr, 0,
                                MZ_DEFAULT_COMPRESSION, 0, 0)) {
    const std::string message = zip_error(writer, "cannot write metadata");
    cleanup();
    return fail(ErrorCode::kStorage, message);
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
  if (!options.sinf.empty()) {
    const Bytes& sinf_data = options.sinf;
    if (!mz_zip_writer_add_mem_ex(&writer, options.sinf_path.c_str(),
                                  sinf_data.data(), sinf_data.size(), nullptr, 0,
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

PlistValue build_itunes_metadata(const App& app, const DownloadOffer& offer,
                                 const Account& account,
                                 std::string_view sinf_path) {
  PlistValue metadata = offer.metadata.is_dictionary()
                            ? offer.metadata
                            : PlistValue::dictionary();
  if (app.id > 0) metadata.set("itemId", PlistValue::integer(app.id));
  if (!app.bundle_id.empty()) {
    metadata.set("softwareVersionBundleId",
                 PlistValue::string(app.bundle_id));
  }
  if (!offer.bundle_id.empty()) {
    metadata.set("softwareVersionBundleId",
                 PlistValue::string(offer.bundle_id));
  }
  if (!offer.display_version.empty()) {
    metadata.set("bundleShortVersionString",
                 PlistValue::string(offer.display_version));
  }
  if (!offer.external_version_id.empty()) {
    metadata.set("softwareVersionExternalIdentifier",
                 PlistValue::string(offer.external_version_id));
  }
  if (!app.name.empty()) metadata.set("itemName", PlistValue::string(app.name));
  if (!account.email.empty()) {
    metadata.set("apple-id", PlistValue::string(account.email));
    metadata.set("userName", PlistValue::string(account.email));
  }
  if (!sinf_path.empty()) {
    PlistValue paths = PlistValue::array();
    paths.append(PlistValue::string(std::string(sinf_path)));
    metadata.set("SinfPaths", std::move(paths));
  }
  return metadata;
}

}  // namespace ipashelf
