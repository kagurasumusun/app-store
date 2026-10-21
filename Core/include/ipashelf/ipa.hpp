// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <optional>
#include <utility>
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
  /// Contents of CFBundleSupportedPlatforms when the key is present.
  std::vector<std::string> supported_platforms;
  /// Whether CFBundleSupportedPlatforms is declared at all. Legacy iOS apps
  /// may predate the key; an absent declaration differs from an empty one.
  bool declares_supported_platforms = false;
  /// Full archive paths from SC_Info/Manifest.plist's SinfPaths, if any.
  std::vector<std::string> sinf_paths;
  /// `.sinf` files actually present under SC_Info, as (archive path,
  /// uncompressed size) pairs. The store package and the download
  /// response between them must leave a non-empty file at every path
  /// in `sinf_paths`; anything else installs as far as iOS's FairPlay
  /// check and fails there with a generic "unable to install".
  std::vector<std::pair<std::string, std::uint64_t>> sinf_files;
  std::uint64_t file_size = 0;
};

/// Reads `Payload/*.app/Info.plist` without extracting the archive.
[[nodiscard]] Result<IpaInfo> inspect_ipa(const std::string& path);

/// The conventional location for the first Sinf blob inside the app bundle.
[[nodiscard]] std::string default_sinf_path(const IpaInfo& info);

struct IpaPatchOptions {
  std::string source_path;
  std::string destination_path;
  /// Contents of the root-level iTunesMetadata.plist (written as a binary
  /// plist, matching what Apple's own tooling produces).
  PlistValue metadata = PlistValue::dictionary();
  /// Optional artwork written as root-level iTunesArtwork.
  std::optional<Bytes> artwork;
  /// FairPlay sinf blobs keyed by their archive path (see
  /// resolve_sinf_entries).
  std::vector<std::pair<std::string, Bytes>> sinf_entries;
};

/// Pairs the sinf blobs from a download response with the archive paths
/// they belong to. When the package ships an SC_Info manifest its SinfPaths
/// are authoritative and must match the blobs one-for-one; otherwise the
/// first blob goes to the conventional SC_Info/<executable>.sinf path.
[[nodiscard]] Result<std::vector<std::pair<std::string, Bytes>>>
resolve_sinf_entries(const IpaInfo& info, const std::vector<Sinf>& sinfs);

/// Verifies that a downloaded package declares support for the platform it
/// was requested for. Unspecified and macOS requests are not checked.
[[nodiscard]] Status validate_ipa_platform(const IpaInfo& info,
                                           Platform platform);

/// Produces the patched archive. Whenever nothing needs replacing — the
/// store package normally carries neither the sinf files nor a metadata
/// document yet — the injected entries are appended and every original byte
/// of the package is preserved, with the new entries written in the record
/// form Apple's own tooling uses (CRC and sizes in the local header, no
/// data descriptor). Layouts that do need an entry replaced fall back to a
/// full rewrite, which still copies untouched entries verbatim. A metadata
/// document is added only when the source carries none, and root artwork is
/// replaced only when the options supply one.
[[nodiscard]] Status write_patched_ipa(const IpaPatchOptions& options,
                                       const CancellationToken& token);

/// Verifies the FairPlay content of a finished archive: every sinf blob
/// in `expected` (the entries handed to `write_patched_ipa`) must be
/// present byte-for-byte, and every path the package's SC_Info manifest
/// lists must hold a non-empty file. A package that fails this would
/// download, transfer and only be rejected by iOS at the FairPlay step
/// with an opaque "unable to install", so callers check before
/// accepting the file into the library.
[[nodiscard]] Status verify_ipa_fairplay(
    const std::string& path,
    const std::vector<std::pair<std::string, Bytes>>& expected);

/// Builds the metadata document expected by Apple's installation tooling:
/// the server-provided metadata plus the purchasing account identity.
[[nodiscard]] PlistValue build_itunes_metadata(const DownloadOffer& offer,
                                               const Account& account);

}  // namespace ipashelf
