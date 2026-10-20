// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
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
  [[nodiscard]] Result<std::vector<std::string>> version_identifiers(
      const Account& account, const App& app, Platform platform,
      const CancellationToken& token);

  [[nodiscard]] const StoreConfiguration& configuration() const {
    return configuration_;
  }

 private:
  [[nodiscard]] Result<LoginResult> perform_login(
      const LoginRequest& request, const Bag& bag, SapSigner* signer,
      const CancellationToken& token);

  [[nodiscard]] Result<HttpResponse> send_login_request(
      const HttpRequest& request, const CancellationToken& token);

  [[nodiscard]] Result<HttpResponse> send_plist_post(
      const std::string& url, const HttpHeaders& headers,
      const PlistValue& payload, bool sign, SapSigner* signer,
      const CancellationToken& token);

  [[nodiscard]] Result<DownloadOffer> request_download_at(
      const Account& account, const App& app, std::string_view base_url,
      const char* version_key, std::string_view external_version_id,
      std::string_view guid, Platform platform,
      const CancellationToken& token, int* http_status = nullptr);

  [[nodiscard]] Result<DownloadOffer> send_unpinned_redownload(
      const Account& account, const App& app, std::string_view endpoint,
      std::string_view guid, const CancellationToken& token);

  HttpTransport& transport_;
  StoreConfiguration configuration_;
};

// ---------------------------------------------------------------------------
// Protocol helpers exposed for tests and platform bridges
// ---------------------------------------------------------------------------

[[nodiscard]] Bytes build_login_body(std::string_view email,
                                     std::string_view password,
                                     std::string_view auth_code,
                                     std::string_view guid, int attempt);

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
