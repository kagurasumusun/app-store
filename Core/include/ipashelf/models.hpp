// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ipashelf/base.hpp"
#include "ipashelf/plist.hpp"

namespace ipashelf {

enum class Platform {
  kUnspecified = 0,
  kIPhone,
  kIPad,
  kAppleTV,
  kWatchOS,
  kVisionOS,
  kMacOS,
};

[[nodiscard]] std::string to_string(Platform platform);
[[nodiscard]] Result<Platform> parse_platform(std::string_view value);

/// The value used by Apple's public lookup/search API.
[[nodiscard]] Result<std::string> search_entity(Platform platform);
[[nodiscard]] Result<std::string> lookup_entity(Platform platform);
/// The value used by the catalog API and version lookups.
[[nodiscard]] Result<std::string> catalog_platform(Platform platform);
/// CFBundleSupportedPlatforms value expected in a downloaded IPA.
[[nodiscard]] Result<std::string> bundle_platform(Platform platform);

/// A catalog entry. Only fields used by the app are retained; no code from
/// reference implementations is embedded here. The struct is intentionally a
/// flat value type so it can cross the Objective-C++ bridge cheaply.
struct App {
  std::int64_t id = 0;
  std::string bundle_id;
  std::string name;
  std::string version;
  std::string seller;
  std::string artist_name;
  std::string seller_url;
  std::string artwork_url;
  std::string description;
  std::string release_notes;
  std::string release_date;
  std::string current_version_release_date;
  std::string kind;
  std::string minimum_os_version;
  std::string primary_genre;
  std::string content_advisory_rating;
  std::string currency;
  std::string track_view_url;
  std::string language_codes;
  double price = 0;
  double average_rating = 0;
  double current_version_average_rating = 0;
  std::int64_t rating_count = 0;
  std::int64_t current_version_rating_count = 0;
  std::int64_t byte_size = 0;
  std::int64_t genre_id = 0;
  bool has_arcade_offer = false;
  bool is_universal = false;
  bool is_free() const { return price <= 0; }
  std::vector<Platform> platforms;
  std::vector<std::string> supported_devices;
  std::vector<std::string> languages;
  std::vector<std::string> screenshot_urls;
  std::vector<std::string> ipad_screenshot_urls;
};

/// A customer review from Apple's public review feed.
struct Review {
  std::string id;
  std::string author;
  std::string title;
  std::string body;
  std::string version;
  std::string updated;
  int rating = 0;
};

struct ReviewsPage {
  int page = 1;
  bool has_more = false;
  std::vector<Review> reviews;
};

/// A search request with the filters the UI exposes.
struct SearchQuery {
  std::string term;
  std::string region;
  Platform platform = Platform::kIPhone;
  int limit = 25;
  std::int64_t genre_id = 0;
  /// When set, results are filtered locally by price.
  std::optional<bool> free_only;
  /// One of "relevance", "name", "rating", "recent". Local sorting.
  std::string sort = "relevance";
};

/// An App Store storefront. `storefront_id` is the numeric front Apple uses in
/// the `X-Apple-Store-Front` header. The table is data, not code.
struct Storefront {
  std::string code;      // ISO-3166 alpha-2, upper case
  std::string name;      // English display name
  std::string storefront_id;
  std::string currency;
};

[[nodiscard]] const std::vector<Storefront>& storefronts();
[[nodiscard]] std::optional<Storefront> storefront_for_region(
    std::string_view code);
/// Returns the `X-Apple-Store-Front` value, for example `143441-1,29`.
[[nodiscard]] std::string storefront_header(const Storefront& storefront);

struct Sinf {
  std::int64_t id = 0;
  Bytes data;
};

/// A single entry from the private `volumeStoreDownloadProduct` response.
struct DownloadOffer {
  std::int64_t item_id = 0;
  std::string url;
  std::string preflight_url;
  std::string artwork_url;
  std::string md5;
  std::vector<Sinf> sinfs;
  PlistValue metadata = PlistValue::dictionary();
  std::string bundle_id;
  std::string display_version;
  std::string external_version_id;
  std::string software_platform;
  std::int64_t byte_size = 0;
};

struct VersionIdentifier {
  std::string external_id;
  std::string display_version;
};

struct SearchResults {
  int count = 0;
  std::vector<App> results;
};

/// Persisted account record. Sensitive fields are only written to the
/// Keychain by the platform layer.
struct Account {
  std::string email;
  std::string name;
  std::string password;
  std::string password_token;
  std::string directory_services_id;
  std::string storefront;
  std::string pod;
  std::string region;  // ISO-3166 alpha-2 used for public catalog requests.
  std::string device_id;  // uppercase hex; shared protocol identity.

  [[nodiscard]] std::string identifier() const;
  [[nodiscard]] std::string storefront_country_prefix() const;
};

/// Hardware identity used for `guid`, `<X-Guid>` and SAP setup. iOS has no
/// public MAC address API, so the app persists a random six-byte identity.
struct HardwareIdentity {
  Bytes bytes;
  [[nodiscard]] std::string guid() const;
  [[nodiscard]] Result<std::vector<std::uint8_t>> sap_block() const;
  [[nodiscard]] static Result<HardwareIdentity> from_hex(std::string_view hex);
  [[nodiscard]] static HardwareIdentity random();
};

struct AppDownloadInfo {
  App app;
  Platform platform = Platform::kIPhone;
  std::string external_version_id;
};

/// Serialises account records for secure storage. The JSON must only be
/// written to the Keychain because it contains the password and token.
[[nodiscard]] std::string account_to_json(const Account& account);
[[nodiscard]] Result<Account> account_from_json(std::string_view json);
[[nodiscard]] std::string accounts_to_json(const std::vector<Account>& accounts);
[[nodiscard]] Result<std::vector<Account>> accounts_from_json(std::string_view json);

/// Result of a completed package download.
struct DownloadResult {
  std::string path;
  std::string display_version;
  std::string bundle_id;
  std::int64_t byte_size = 0;
  std::string md5;
  std::vector<Sinf> sinfs;
};

}  // namespace ipashelf
