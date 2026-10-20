// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ipashelf/base.hpp"
#include "ipashelf/models.hpp"
#include "ipashelf/plist.hpp"

namespace ipashelf {

/// Facts read from the main application bundle inside an unencrypted IPA.
struct IpaInfo {
  std::string bundle_id;
  std::string display_version;
  std::string build_version;
  std::string executable;
  std::string app_directory;  // "Payload/Example.app/"
  std::vector<std::string> supported_platforms;
  std::uint64_t file_size = 0;
};

/// Reads `Payload/*.app/Info.plist` without extracting the archive.
[[nodiscard]] Result<IpaInfo> inspect_ipa(const std::string& path);

/// The conventional location for the first Sinf blob inside the app bundle.
[[nodiscard]] std::string default_sinf_path(const IpaInfo& info);

struct IpaPatchOptions {
  std::string source_path;
  std::string destination_path;
  /// Contents of the root-level iTunesMetadata.plist.
  PlistValue metadata = PlistValue::dictionary();
  /// Optional artwork written as root-level iTunesArtwork.
  std::optional<Bytes> artwork;
  /// Optional FairPlay Sinf blob installed inside the app bundle.
  Bytes sinf;
  std::string sinf_path;
};

/// Copies the source archive into a patched archive. Existing iTunes metadata,
/// artwork and sinf entries are replaced; everything else is copied verbatim.
[[nodiscard]] Status write_patched_ipa(const IpaPatchOptions& options,
                                       const CancellationToken& token);

/// Builds the metadata document expected by Apple's installation tooling.
[[nodiscard]] PlistValue build_itunes_metadata(const App& app,
                                              const DownloadOffer& offer,
                                              const Account& account,
                                              std::string_view sinf_path = {});

}  // namespace ipashelf
