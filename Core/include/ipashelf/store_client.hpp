// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "ipashelf/bag.hpp"
#include "ipashelf/base.hpp"
#include "ipashelf/http.hpp"
#include "ipashelf/models.hpp"
#include "ipashelf/sap.hpp"

namespace ipashelf {

inline constexpr std::string_view kDefaultUserAgent =
    "Configurator/2.17 (Macintosh; OS X 15.2; 24C5089c) "
    "AppleWebKit/0620.1.16.11.6";

inline constexpr std::string_view kPurchaseUserAgent =
    "Configurator/2.17 (Macintosh; OS X 15.2; 24C5089c) "
    "AppleWebKit/0620.1.16.11.6";

struct StoreConfiguration {
  std::string user_agent = std::string(kDefaultUserAgent);
  std::function<void(std::chrono::milliseconds)> sleep =
      [](std::chrono::milliseconds duration) {
        std::this_thread::sleep_for(duration);
      };
  int max_authentication_attempts = 3;
  int authentication_retry_seconds = 10;
  int max_authentication_retry_seconds = 30;
  using SapBackendFactory =
      std::function<Result<std::shared_ptr<SapBackend>>()>;
  SapBackendFactory sap_backend_factory;

  /// Generates the device kbsync blob required by the ent/download
  /// endpoint from the six-byte hardware identity and numeric account DSID.
  /// When unset (no platform generator is linked), downloads use the legacy
  /// volumeStore/redownload/updateProduct chain, exactly like a build
  /// without kbsync support.
  using KbsyncGenerator =
      std::function<Result<Bytes>(const Bytes& hardware_id,
                                  std::uint64_t directory_services_id)>;
  KbsyncGenerator kbsync_generator;
  /// Persistence hooks for the kbsync cache entry (one JSON string). The
  /// platform layer stores it in the Keychain under the key "kbsync-v1".
  std::function<std::optional<std::string>()> kbsync_cache_load;
  std::function<void(std::string_view)> kbsync_cache_store;
  std::function<void()> kbsync_cache_remove;

  /// Local timezone used by the purchase-history (DAAP) requests. The
  /// platform layer fills these from the device timezone.
  std::string time_zone_name = "UTC";
  int utc_offset_seconds = 0;
  /// Clock override for tests; returns Unix seconds.
  std::function<std::int64_t()> now_unix;
};

struct LoginRequest {
  std::string email;
  std::string password;
  std::string auth_code;
  std::string region;
  HardwareIdentity hardware;
};

struct LoginResult {
  Account account;
  bool two_factor_required = false;
};

/// External version identifiers reported by the download metadata, newest
/// semantics preserved: `identifiers` is the full server list and `latest`
/// is the identifier of the build the server selected.
struct AppVersions {
  std::vector<std::string> identifiers;
  std::string latest;
};

/// Display version and release date of one build, read from the item
/// metadata of the version-pinned download response (whose own external
/// identifier is verified against the request before use).
struct AppVersionMetadata {
  std::string display_version;
  std::int64_t release_date_unix = 0;
};

/// One page of the account's purchase history.
struct OwnedAppsPage {
  std::vector<App> apps;
  int total = 0;
  int page = 1;
  int limit = 200;
};

/// Synchronous App Store client. Every method takes a cancellation token and
/// returns protocol errors without exposing credentials.
class StoreClient {
 public:
  explicit StoreClient(HttpTransport& transport,
                       StoreConfiguration configuration = {});

  [[nodiscard]] Result<Bag> fetch_bag(const HardwareIdentity& hardware,
                                      const CancellationToken& token);

  [[nodiscard]] Result<LoginResult> login(const LoginRequest& request,
                                          const CancellationToken& token);

  [[nodiscard]] Result<SearchResults> search(std::string_view term,
                                             std::string_view region,
                                             Platform platform, int limit,
                                             const CancellationToken& token);

  /// Search with genre, price and sort filters applied.
  [[nodiscard]] Result<SearchResults> search(const SearchQuery& query,
                                             const CancellationToken& token);

  /// Reads one page of customer reviews from Apple's public review feed.
  [[nodiscard]] Result<ReviewsPage> reviews(std::int64_t app_id,
                                            std::string_view region, int page,
                                            const CancellationToken& token);

  [[nodiscard]] Result<App> lookup(std::int64_t app_id,
                                   std::string_view bundle_id,
                                   std::string_view region, Platform platform,
                                   const CancellationToken& token);

  /// Reads the current external version id from the public App Store catalog.
  [[nodiscard]] Result<std::string> latest_external_version_id(
      const App& app, std::string_view region, Platform platform,
      const CancellationToken& token);

  /// Obtains a download offer, applying Apple's redownload and update-product
  /// fallbacks when the volume store returns an empty response.
  [[nodiscard]] Result<DownloadOffer> download_offer(
      const Account& account, const App& app,
      std::string_view external_version_id, Platform platform,
      const CancellationToken& token);

  [[nodiscard]] Status purchase(const Account& account, const App& app,
                                Platform platform,
                                const CancellationToken& token);

  /// Lists external version identifiers for an app the account owns.
  [[nodiscard]] Result<AppVersions> version_list(
      const Account& account, const App& app, Platform platform,
      const CancellationToken& token);

  /// Reads the display version and release date of one external version
  /// from the package's own Info.plist, fetched with HTTP range requests.
  [[nodiscard]] Result<AppVersionMetadata> version_metadata(
      const Account& account, const App& app,
      std::string_view external_version_id, Platform platform,
      const CancellationToken& token);

  /// Lists apps the account owns (the DAAP purchase history). Results from
  /// both storefront media kinds are merged, de-duplicated and sorted by
  /// purchase date descending before paging. Requires SAP signing.
  [[nodiscard]] Result<OwnedAppsPage> owned_apps(
      const Account& account, std::optional<Platform> platform, int page,
      int limit, const CancellationToken& token);

  [[nodiscard]] const StoreConfiguration& configuration() const {
    return configuration_;
  }

 private:
  [[nodiscard]] Result<LoginResult> perform_login(
      const LoginRequest& request, const Bag& bag, SapSigner* signer,
      const CancellationToken& token);

  [[nodiscard]] Result<HttpResponse> send_login_request(
      const HttpRequest& request, SapSigner* signer,
      const CancellationToken& token);

  [[nodiscard]] Result<HttpResponse> send_plist_post(
      const std::string& url, const HttpHeaders& headers,
      const PlistValue& payload, bool sign, SapSigner* signer,
      const CancellationToken& token);

  /// Fetches the bag and completes a SAP setup handshake. Shared by the
  /// sign-in and purchase-history flows.
  [[nodiscard]] Result<std::unique_ptr<SapSession>> open_sap_session(
      const HardwareIdentity& hardware, const CancellationToken& token,
      Bag* bag_out = nullptr);

  [[nodiscard]] Result<DownloadOffer> send_download_product(
      const Account& account, const App& app,
      const HardwareIdentity& hardware, std::string_view external_version_id,
      Platform platform, const CancellationToken& token);

  [[nodiscard]] Result<DownloadOffer> send_ent_download(
      const Account& account, const App& app,
      const HardwareIdentity& hardware, std::string_view endpoint,
      std::string_view external_version_id, const CancellationToken& token);

  [[nodiscard]] std::string cached_kbsync(std::uint64_t dsid,
                                          std::string_view guid);
  void store_kbsync(std::uint64_t dsid, std::string_view guid,
                    std::string_view data);
  void invalidate_kbsync(std::uint64_t dsid, std::string_view guid,
                         std::string_view data);

  HttpTransport& transport_;
  StoreConfiguration configuration_;
  std::mutex kbsync_mutex_;
  std::string kbsync_cache_entry_;
};

// ---------------------------------------------------------------------------
// Protocol helpers exposed for tests and platform bridges
// ---------------------------------------------------------------------------

[[nodiscard]] Bytes build_login_body(std::string_view email,
                                     std::string_view password,
                                     std::string_view auth_code,
                                     std::string_view guid, int attempt);

/// Storefront routing hint for phone-based Apple Accounts (mainland China
/// and India). Returns the numeric storefront id or an empty string.
[[nodiscard]] std::string phone_storefront_header(std::string_view apple_id);

[[nodiscard]] Bytes build_purchase_body(std::int64_t app_id,
                                        std::string_view guid,
                                        std::string_view pricing_parameter);

[[nodiscard]] Bytes build_download_body(std::int64_t app_id,
                                        std::string_view guid,
                                        std::string_view version_key,
                                        std::string_view external_version_id);

[[nodiscard]] Result<DownloadOffer> parse_download_response(
    const PlistValue& document);

[[nodiscard]] Status map_store_failure(std::string_view failure_type,
                                       std::string_view customer_message,
                                       bool for_purchase);

[[nodiscard]] Result<Bytes> plist_body_to_bytes(const PlistValue& value);

[[nodiscard]] Result<PlistValue> parse_apple_response(const HttpResponse& response);

/// Builds the public search URL so the UI and tests can share one encoder.
[[nodiscard]] std::string build_search_url(const SearchQuery& query);

/// Parses a review feed document. `page` is echoed in the result.
[[nodiscard]] Result<ReviewsPage> parse_reviews_document(std::string_view body,
                                                         int page = 1);

}  // namespace ipashelf
