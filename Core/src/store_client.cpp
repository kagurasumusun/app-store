// SPDX-License-Identifier: MIT

#include "ipashelf/store_client.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <thread>

#include "ipashelf/crypto.hpp"
#include "ipashelf/json.hpp"
#include "ipashelf/plist.hpp"
#include "ipashelf/url.hpp"

namespace ipashelf {
namespace {

constexpr std::string_view kITunesHost = "itunes.apple.com";
constexpr std::string_view kAuthPath =
    "/WebObjects/MZFinance.woa/wa/authenticate";
constexpr std::string_view kPurchasePath =
    "/WebObjects/MZFinance.woa/wa/buyProduct";
constexpr std::string_view kVolumePath =
    "/WebObjects/MZFinance.woa/wa/volumeStoreDownloadProduct";

constexpr std::string_view kBadLoginMessage =
    "MZFinance.BadLogin.Configurator_message";
constexpr std::string_view kAccountDisabledMessage = "Your account is disabled.";
constexpr std::string_view kSubscriptionMessage = "Subscription Required";
constexpr std::string_view kPasswordChangedMessage = "Your password has changed.";

constexpr std::string_view kPriceFree = "0";
constexpr std::string_view kProductTypeApp = "C";
constexpr std::string_view kPricingStandard = "STDQ";
constexpr std::string_view kPricingArcade = "GAME";

bool valid_region(std::string_view region) {
  if (region.size() != 2) return false;
  return std::isalpha(static_cast<unsigned char>(region[0])) != 0 &&
         std::isalpha(static_cast<unsigned char>(region[1])) != 0;
}

std::string region_or_default(std::string_view region) {
  if (valid_region(region)) return to_lower(region);
  return "us";
}

std::string strip_whitespace(std::string_view value) {
  std::string result;
  result.reserve(value.size());
  for (const char character : value) {
    if (std::isspace(static_cast<unsigned char>(character)) == 0) {
      result.push_back(character);
    }
  }
  return result;
}

std::string sanitize_pod(std::string_view pod) {
  std::string result;
  for (const char character : pod) {
    if (std::isdigit(static_cast<unsigned char>(character)) != 0) {
      result.push_back(character);
    }
  }
  return result;
}

std::string apply_pod(std::string_view url, std::string_view pod) {
  const std::string clean_pod = sanitize_pod(pod);
  if (clean_pod.empty()) return std::string(url);
  auto parsed = Url::parse(url);
  if (!parsed) return std::string(url);
  Url result = parsed.value();
  if (!host_is_or_subdomain(result.host, "buy.itunes.apple.com")) {
    return std::string(url);
  }
  if (!starts_with(result.host, "p")) {
    result.host = "p" + clean_pod + "-" + result.host;
  }
  return result.serialize();
}

bool retryable_authentication_status(int status) {
  return status == 204 || status == 403 || status == 404 || status == 429 ||
         (status >= 500 && status <= 599);
}

std::chrono::seconds parse_retry_after(std::string_view value) {
  const std::string trimmed = trim(value);
  if (trimmed.empty()) return std::chrono::seconds(0);
  char* end = nullptr;
  const long long seconds = std::strtoll(trimmed.c_str(), &end, 10);
  if (end != nullptr && *end == '\0' && seconds >= 0) {
    return std::chrono::seconds(seconds);
  }
  return std::chrono::seconds(0);
}

Result<PlistValue> decode_plist_response(const HttpResponse& response) {
  if (response.body.empty()) {
    return make_error(ErrorCode::kParse,
                      "Apple returned an empty response (HTTP " +
                          std::to_string(response.status) +
                          ") - unsigned requests are rejected by Apple");
  }
  auto plist = PlistValue::parse(response.body);
  if (!plist) {
    std::string preview(response.body.begin(),
                        response.body.begin() +
                            std::min(response.body.size(), std::size_t(128)));
    return make_error(
        ErrorCode::kParse,
        "Apple returned a non-property-list response (HTTP " +
            std::to_string(response.status) + "): " + preview);
  }
  return plist;
}

std::vector<std::string> string_array(const Json* value) {
  std::vector<std::string> output;
  if (value == nullptr || !value->is_array()) return output;
  output.reserve(value->array_items().size());
  for (const auto& entry : value->array_items()) {
    if (entry.is_string()) output.push_back(entry.string_value());
  }
  return output;
}

std::vector<Platform> platforms_from_json(const Json& item) {
  std::vector<Platform> platforms;
  const auto add = [&](Platform platform) {
    if (std::find(platforms.begin(), platforms.end(), platform) ==
        platforms.end()) {
      platforms.push_back(platform);
    }
  };

  if (const Json* devices = item.find("supportedDevices");
      devices != nullptr && devices->is_array()) {
    for (const auto& device : devices->array_items()) {
      const std::string name = device.string_or();
      if (starts_with(name, "iPhone") || starts_with(name, "iPod")) {
        add(Platform::kIPhone);
      } else if (starts_with(name, "iPad")) {
        add(Platform::kIPad);
      } else if (starts_with(name, "AppleTV")) {
        add(Platform::kAppleTV);
      } else if (starts_with(name, "Watch")) {
        add(Platform::kWatchOS);
      } else if (starts_with(name, "RealityDevice")) {
        add(Platform::kVisionOS);
      } else if (starts_with(name, "Mac")) {
        add(Platform::kMacOS);
      }
    }
  }
  const Json* kind_value = item.find("kind");
  const std::string kind = kind_value != nullptr ? kind_value->string_or()
                                                 : std::string();
  if (kind == "mac-software") add(Platform::kMacOS);
  if (kind == "iPadSoftware") add(Platform::kIPad);
  if (kind == "software" && platforms.empty()) add(Platform::kIPhone);
  return platforms;
}

Result<App> app_from_json(const Json& item) {
  if (!item.is_object()) {
    return make_error(ErrorCode::kParse, "catalog entry is not an object");
  }
  App app;
  const Json* track_id = item.find("trackId");
  if (track_id != nullptr) app.id = track_id->int_or();
  if (app.id <= 0) {
    const Json* adam_id = item.find("adamId");
    if (adam_id != nullptr) app.id = adam_id->int_or();
  }
  if (const Json* bundle = item.find("bundleId"); bundle != nullptr) {
    app.bundle_id = bundle->string_or();
  }
  if (const Json* name = item.find("trackName"); name != nullptr) {
    app.name = name->string_or();
  } else if (const Json* name = item.find("trackCensoredName");
             name != nullptr) {
    app.name = name->string_or();
  }
  if (const Json* version = item.find("version"); version != nullptr) {
    app.version = version->string_or();
  }
  if (const Json* seller = item.find("sellerName"); seller != nullptr) {
    app.seller = seller->string_or();
  }
  if (const Json* artwork = item.find("artworkUrl512"); artwork != nullptr) {
    app.artwork_url = artwork->string_or();
  } else if (const Json* artwork = item.find("artworkUrl100");
             artwork != nullptr) {
    app.artwork_url = artwork->string_or();
  }
  if (const Json* description = item.find("description");
      description != nullptr) {
    app.description = description->string_or();
  }
  if (const Json* notes = item.find("releaseNotes"); notes != nullptr) {
    app.release_notes = notes->string_or();
  }
  if (const Json* release = item.find("releaseDate"); release != nullptr) {
    app.release_date = release->string_or();
  }
  if (const Json* minimum = item.find("minimumOsVersion"); minimum != nullptr) {
    app.minimum_os_version = minimum->string_or();
  }
  if (const Json* kind = item.find("kind"); kind != nullptr) {
    app.kind = kind->string_or();
  }
  if (const Json* price = item.find("price"); price != nullptr) {
    app.price = price->number_or();
  }
  if (const Json* arcade = item.find("hasInAppPurchases"); arcade != nullptr) {
    app.has_arcade_offer = arcade->bool_or();
  }
  if (const Json* artist = item.find("artistName"); artist != nullptr) {
    app.artist_name = artist->string_or();
  }
  if (app.artist_name.empty()) app.artist_name = app.seller;
  if (const Json* url = item.find("sellerUrl"); url != nullptr) {
    app.seller_url = url->string_or();
  }
  if (const Json* genre = item.find("primaryGenreName"); genre != nullptr) {
    app.primary_genre = genre->string_or();
  }
  if (const Json* genre_id = item.find("primaryGenreId");
      genre_id != nullptr) {
    app.genre_id = genre_id->int_or();
  }
  if (const Json* advisory = item.find("contentAdvisoryRating");
      advisory != nullptr) {
    app.content_advisory_rating = advisory->string_or();
  }
  if (const Json* currency = item.find("currency"); currency != nullptr) {
    app.currency = currency->string_or();
  }
  if (const Json* view = item.find("trackViewUrl"); view != nullptr) {
    app.track_view_url = view->string_or();
  }
  if (const Json* languages = item.find("languageCodesISO2A");
      languages != nullptr) {
    app.languages = string_array(languages);
    app.language_codes = join(app.languages, ",");
  }
  if (const Json* shots = item.find("screenshotUrls"); shots != nullptr) {
    app.screenshot_urls = string_array(shots);
  }
  if (const Json* shots = item.find("ipadScreenshotUrls"); shots != nullptr) {
    app.ipad_screenshot_urls = string_array(shots);
  }
  if (const Json* devices = item.find("supportedDevices");
      devices != nullptr) {
    app.supported_devices = string_array(devices);
  }
  if (const Json* rating = item.find("averageUserRating");
      rating != nullptr) {
    app.average_rating = rating->number_or();
  }
  if (const Json* count = item.find("userRatingCount"); count != nullptr) {
    app.rating_count = count->int_or();
  }
  if (const Json* rating = item.find("averageUserRatingForCurrentVersion");
      rating != nullptr) {
    app.current_version_average_rating = rating->number_or();
  }
  if (const Json* count = item.find("userRatingCountForCurrentVersion");
      count != nullptr) {
    app.current_version_rating_count = count->int_or();
  }
  if (const Json* size = item.find("fileSizeBytes"); size != nullptr) {
    if (size->is_string()) {
      app.byte_size = std::strtoll(size->string_value().c_str(), nullptr, 10);
    } else {
      app.byte_size = size->int_or();
    }
  }
  if (const Json* date = item.find("currentVersionReleaseDate");
      date != nullptr) {
    app.current_version_release_date = date->string_or();
  }
  if (const Json* features = item.find("features");
      features != nullptr && features->is_array()) {
    for (const auto& feature : features->array_items()) {
      if (feature.string_or() == "iosUniversal") app.is_universal = true;
    }
  }
  app.platforms = platforms_from_json(item);
  if (app.id <= 0 || app.name.empty()) {
    return make_error(ErrorCode::kParse, "catalog entry is missing an identity");
  }
  return app;
}

Result<SearchResults> parse_search_document(std::string_view body) {
  auto document = Json::parse(body);
  if (!document) return document.error();
  const Json* results = document.value().find("results");
  if (results == nullptr || !results->is_array()) {
    return make_error(ErrorCode::kParse, "Apple search response has no results");
  }
  SearchResults output;
  output.count = static_cast<int>(results->array_items().size());
  if (const Json* count = document.value().find("resultCount");
      count != nullptr) {
    output.count = static_cast<int>(count->int_or(output.count));
  }
  for (const auto& entry : results->array_items()) {
    auto app = app_from_json(entry);
    if (!app) continue;
    output.results.push_back(std::move(app).value());
  }
  return output;
}

std::string metadata_string(const PlistValue& metadata, std::string_view key) {
  const PlistValue* value = metadata.find(key);
  if (value == nullptr) return {};
  if (value->is_string()) return value->string_value();
  if (value->is_integer()) return std::to_string(value->integer_value());
  return {};
}

std::int64_t metadata_integer(const PlistValue& metadata,
                              std::string_view key) {
  const PlistValue* value = metadata.find(key);
  if (value == nullptr) return 0;
  if (value->is_integer()) return value->integer_value();
  if (value->is_string()) {
    char* end = nullptr;
    const long long parsed = std::strtoll(value->string_value().c_str(), &end, 10);
    if (end != nullptr && *end == '\0') return static_cast<std::int64_t>(parsed);
  }
  return 0;
}

bool download_item_matches(const DownloadOffer& offer, const App& app,
                           std::string_view external_version_id) {
  if (offer.item_id != 0 && offer.item_id != app.id) return false;
  if (!app.bundle_id.empty() && !offer.bundle_id.empty() &&
      offer.bundle_id != app.bundle_id) {
    return false;
  }
  if (!external_version_id.empty() && !offer.external_version_id.empty() &&
      offer.external_version_id != external_version_id) {
    return false;
  }
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public protocol helpers
// ---------------------------------------------------------------------------

Bytes build_login_body(std::string_view email, std::string_view password,
                       std::string_view auth_code, std::string_view guid,
                       int attempt) {
  const std::string code = strip_whitespace(auth_code);
  const std::vector<std::pair<std::string, std::string>> values = {
      {"appleId", std::string(email)},
      {"attempt", std::to_string(attempt)},
      {"guid", std::string(guid)},
      {"password", std::string(password) + code},
      {"rmp", "0"},
      {"why", "signIn"},
  };
  const std::string encoded = form_encode(values);
  return Bytes(encoded.begin(), encoded.end());
}

Bytes build_purchase_body(std::int64_t app_id, std::string_view guid,
                          std::string_view pricing_parameter) {
  PlistValue body = PlistValue::dictionary();
  body.set("appExtVrsId", PlistValue::string("0"));
  body.set("hasAskedToFulfillPreorder", PlistValue::string("true"));
  body.set("buyWithoutAuthorization", PlistValue::string("true"));
  body.set("hasDoneAgeCheck", PlistValue::string("true"));
  body.set("guid", PlistValue::string(std::string(guid)));
  body.set("needDiv", PlistValue::string("0"));
  body.set("origPage", PlistValue::string("Software-" + std::to_string(app_id)));
  body.set("origPageLocation", PlistValue::string("Buy"));
  body.set("price", PlistValue::string(std::string(kPriceFree)));
  body.set("pricingParameters",
           PlistValue::string(std::string(pricing_parameter)));
  body.set("productType", PlistValue::string(std::string(kProductTypeApp)));
  body.set("salableAdamId", PlistValue::integer(app_id));
  const std::string xml = body.dump_xml();
  return Bytes(xml.begin(), xml.end());
}

Bytes build_download_body(std::int64_t app_id, std::string_view guid,
                          std::string_view version_key,
                          std::string_view external_version_id) {
  PlistValue body = PlistValue::dictionary();
  body.set("creditDisplay", PlistValue::string(""));
  body.set("guid", PlistValue::string(std::string(guid)));
  body.set("salableAdamId", PlistValue::integer(app_id));
  body.set("serialNumber", PlistValue::string("0"));
  if (!external_version_id.empty() && !version_key.empty()) {
    body.set(std::string(version_key),
             PlistValue::string(std::string(external_version_id)));
  }
  const std::string xml = body.dump_xml();
  return Bytes(xml.begin(), xml.end());
}

Result<Bytes> plist_body_to_bytes(const PlistValue& value) {
  const std::string xml = value.dump_xml();
  return Bytes(xml.begin(), xml.end());
}

Result<PlistValue> parse_apple_response(const HttpResponse& response) {
  if (response.status != 200) {
    return Error{ErrorCode::kHttpStatus,
                 "Apple returned HTTP " + std::to_string(response.status),
                 response.status};
  }
  return decode_plist_response(response);
}

Status map_store_failure(std::string_view failure_type,
                         std::string_view customer_message,
                         bool for_purchase) {
  const std::string failure = trim(failure_type);
  const std::string message = trim(customer_message);

  if (failure == "2034" || failure == "2042" || failure == "1008") {
    return Status{Error{ErrorCode::kTokenExpired,
                        "Apple requires the account to sign in again"}};
  }
  if (failure == "5002") {
    if (for_purchase) return ok_status();
    return Status{Error{ErrorCode::kTokenExpired,
                        "Apple reports that the license already exists"}};
  }
  if (failure == "9610") {
    return Status{Error{ErrorCode::kLicenseRequired,
                        "This account does not own the app yet"}};
  }
  if (failure == "2059") {
    return Status{Error{ErrorCode::kTemporarilyUnavailable,
                        "The item is temporarily unavailable"}};
  }
  if (failure == "-5000") {
    return Status{Error{ErrorCode::kUnauthorized,
                        message.empty() ? "Apple rejected the credentials"
                                        : message}};
  }
  if (failure.empty() && message.empty()) return ok_status();
  if (!message.empty()) {
    return Status{Error{ErrorCode::kUnauthorized, message}};
  }
  return Status{Error{ErrorCode::kUnauthorized,
                      "Apple rejected the request (" + failure + ")"}};
}

Result<DownloadOffer> parse_download_response(const PlistValue& document) {
  if (!document.is_dictionary()) {
    return make_error(ErrorCode::kParse, "download response is not a dictionary");
  }
  if (auto failure = document.string_for("failureType");
      failure && !failure->empty()) {
    auto status = map_store_failure(*failure,
                                    document.string_or("customerMessage"), false);
    if (!status) return status.error();
  }
  const PlistValue* items = document.find("songList");
  if (items == nullptr || !items->is_array() || items->array_items().empty()) {
    return make_error(ErrorCode::kNoOffer, "download response has no items");
  }
  const PlistValue& item = items->array_items().front();
  if (!item.is_dictionary()) {
    return make_error(ErrorCode::kParse, "download item is not a dictionary");
  }

  DownloadOffer offer;
  offer.item_id = metadata_integer(
      item.find("metadata") != nullptr && item.find("metadata")->is_dictionary()
          ? *item.find("metadata")
          : PlistValue::dictionary(),
      "itemId");
  offer.url = item.string_or("URL");
  offer.preflight_url = item.string_or("preflightPackageURL");
  offer.artwork_url = item.string_or("artworkURL");
  offer.md5 = to_lower(item.string_or("md5"));
  if (const PlistValue* metadata = item.find("metadata");
      metadata != nullptr && metadata->is_dictionary()) {
    offer.metadata = *metadata;
    offer.bundle_id = metadata_string(*metadata, "softwareVersionBundleId");
    offer.display_version = metadata_string(*metadata, "bundleShortVersionString");
    offer.external_version_id =
        metadata_string(*metadata, "softwareVersionExternalIdentifier");
    offer.software_platform = metadata_string(*metadata, "software-platform");
    offer.byte_size = metadata_integer(*metadata, "size");
  }

  if (const PlistValue* sinfs = item.find("sinfs");
      sinfs != nullptr && sinfs->is_array()) {
    for (const auto& entry : sinfs->array_items()) {
      if (!entry.is_dictionary()) continue;
      Sinf sinf;
      if (const PlistValue* id = entry.find("id")) {
        if (id->is_integer()) sinf.id = id->integer_value();
      }
      if (const PlistValue* data = entry.find("sinf");
          data != nullptr && data->is_data()) {
        sinf.data = data->data_value();
      }
      if (!sinf.data.empty()) offer.sinfs.push_back(std::move(sinf));
    }
  }
  if (offer.url.empty()) {
    return make_error(ErrorCode::kParse, "download item has no package URL");
  }
  return offer;
}

// ---------------------------------------------------------------------------
// StoreClient
// ---------------------------------------------------------------------------

StoreClient::StoreClient(HttpTransport& transport,
                         StoreConfiguration configuration)
    : transport_(transport), configuration_(std::move(configuration)) {}

Result<Bag> StoreClient::fetch_bag(const HardwareIdentity& hardware,
                                   const CancellationToken& token) {
  if (hardware.bytes.empty()) {
    return make_error(ErrorCode::kInvalidArgument, "hardware identity is empty");
  }
  HttpRequest request;
  request.method = "GET";
  request.url = bag_request_url(hardware);
  request.headers.set("Accept", "application/xml");
  request.headers.set("User-Agent", configuration_.user_agent);
  auto response = transport_.send(request, token);
  if (!response) return response.error();
  if (response.value().status != 200) {
    return Error{ErrorCode::kHttpStatus,
                 "Apple URL bag returned HTTP " +
                     std::to_string(response.value().status),
                 response.value().status};
  }
  auto document = PlistValue::parse(response.value().body);
  if (!document) return document.error();
  return Bag::parse(document.value());
}

Result<HttpResponse> StoreClient::send_login_request(
    const HttpRequest& request, const CancellationToken& token) {
  const int max_attempts = std::max(1, configuration_.max_authentication_attempts);
  std::vector<std::string> statuses;
  for (int attempt = 1; attempt <= max_attempts; ++attempt) {
    if (token.cancelled()) {
      return make_error(ErrorCode::kCancelled, "sign-in cancelled");
    }
    auto response = transport_.send(request, token);
    if (!response) {
      if (response.error().code == ErrorCode::kCancelled) return response;
      if (attempt == max_attempts) return response;
      statuses.push_back("transport error");
      const int backoff = std::min(
          configuration_.authentication_retry_seconds * (1 << (attempt - 1)),
          configuration_.max_authentication_retry_seconds);
      configuration_.sleep(std::chrono::seconds(backoff));
      continue;
    }
    const int status = response.value().status;
    if (!retryable_authentication_status(status)) return response;
    statuses.push_back("HTTP " + std::to_string(status));
    if (attempt == max_attempts) {
      return Error{ErrorCode::kHttpStatus,
                   "Apple authentication failed after " +
                       std::to_string(max_attempts) + " attempts",
                   status};
    }
    auto retry_after = parse_retry_after(response.value().header("Retry-After"));
    int delay = std::min(
        configuration_.authentication_retry_seconds * (1 << (attempt - 1)),
        configuration_.max_authentication_retry_seconds);
    if (retry_after.count() > 0) {
      if (retry_after.count() > configuration_.max_authentication_retry_seconds) {
        return Error{ErrorCode::kHttpStatus,
                     "Apple requested a longer wait than this build allows",
                     status};
      }
      delay = std::max<int>(1, static_cast<int>(retry_after.count()));
    }
    configuration_.sleep(std::chrono::seconds(delay));
  }
  return make_error(ErrorCode::kInternal, "authentication retry loop ended");
}

Result<LoginResult> StoreClient::login(const LoginRequest& request,
                                       const CancellationToken& token) {
  if (request.email.empty() || request.password.empty()) {
    return make_error(ErrorCode::kInvalidArgument,
                      "email and password are required");
  }
  const std::string code = strip_whitespace(request.auth_code);
  if (!code.empty()) {
    if (code.size() != 6 ||
        std::all_of(code.begin(), code.end(), [](char character) {
          return std::isdigit(static_cast<unsigned char>(character)) != 0;
        }) == false) {
      return make_error(ErrorCode::kInvalidArgument,
                        "the verification code must be six digits");
    }
  }
  if (!request.hardware.bytes.empty() && request.hardware.bytes.size() != 6) {
    return make_error(ErrorCode::kInvalidArgument,
                      "hardware identity must be six bytes");
  }

  auto bag = fetch_bag(request.hardware, token);
  if (!bag) return bag.error();

  std::unique_ptr<SapSession> session;
  if (!bag.value().sap_setup_url.empty() &&
      !bag.value().sap_setup_cert_url.empty()) {
    SapConfiguration configuration;
    configuration.setup_url = bag.value().sap_setup_url;
    configuration.certificate_url = bag.value().sap_setup_cert_url;
    configuration.version =
        bag.value().sap_version == 0 ? 200 : bag.value().sap_version;
    configuration.hardware = request.hardware;

    Result<std::shared_ptr<SapBackend>> backend =
        make_error(ErrorCode::kSignerUnavailable, "no SAP backend");
    if (configuration_.sap_backend_factory) {
      backend = configuration_.sap_backend_factory();
    } else {
      backend = create_platform_sap_backend();
    }
    if (!backend) return backend.error();

    auto established = establish_sap_session(transport_, configuration,
                                             configuration_.user_agent, token,
                                             backend.value());
    if (!established) return established.error();
    session = std::move(established).value();
  } else {
    IPASHELF_LOG_WARNING(
        "URL bag has no SAP endpoints; attempting an unsigned sign-in");
  }

  auto result = perform_login(request, bag.value(), session.get(), token);
  if (session) {
    auto close_status = session->close();
    if (!close_status && result) {
      return close_status.error();
    }
  }
  return result;
}

Result<LoginResult> StoreClient::perform_login(const LoginRequest& request,
                                               const Bag& bag,
                                               SapSigner* signer,
                                               const CancellationToken& token) {
  auto endpoint = normalize_auth_endpoint(bag.auth_endpoint);
  if (!endpoint) return endpoint.error();
  std::string url = endpoint.value();
  const std::string guid = request.hardware.guid();
  int attempt = 1;
  int redirects = 0;
  const std::string code = strip_whitespace(request.auth_code);

  while (true) {
    if (token.cancelled()) {
      return make_error(ErrorCode::kCancelled, "sign-in cancelled");
    }
    const Bytes body =
        build_login_body(request.email, request.password, code, guid, attempt);
    HttpRequest http_request;
    http_request.method = "POST";
    http_request.url = url;
    http_request.follow_redirects = false;
    http_request.headers.set("Content-Type",
                             "application/x-www-form-urlencoded");
    http_request.headers.set("User-Agent", configuration_.user_agent);
    http_request.body = body;
    if (signer != nullptr) {
      auto signature = signer->sign(body);
      if (!signature) return signature.error();
      http_request.headers.set("X-Apple-ActionSignature",
                               base64_encode(signature.value()));
    }

    auto response = send_login_request(http_request, token);
    if (!response) return response.error();
    const HttpResponse& http = response.value();

    if (http.status >= 300 && http.status < 400) {
      const std::string location = http.header("Location");
      if (location.empty()) {
        return Error{ErrorCode::kHttpStatus,
                     "Apple authentication redirect has no destination",
                     http.status};
      }
      if (redirects >= 4) {
        return Error{ErrorCode::kHttpStatus,
                     "too many Apple authentication redirects", http.status};
      }
      auto resolved = resolve_url(url, location);
      if (!resolved) return resolved.error();
      auto parsed = Url::parse(resolved.value());
      if (!parsed || !parsed.value().https() || parsed.value().userinfo.size() > 0 ||
          !host_is_or_subdomain(parsed.value().host, "itunes.apple.com") ||
          (parsed.value().path != std::string(kAuthPath) &&
           parsed.value().path != std::string(kAuthPath) + "/")) {
        return Error{ErrorCode::kHttpStatus,
                     "Apple authentication redirect points to an unexpected host",
                     http.status};
      }
      url = resolved.value();
      ++redirects;
      continue;
    }

    auto document = decode_plist_response(http);
    if (!document) {
      return Error{ErrorCode::kUnauthorized,
                   "Apple did not return a usable sign-in response",
                   http.status};
    }
    const PlistValue& root = document.value();
    const std::string failure = root.string_or("failureType");
    const std::string customer_message = root.string_or("customerMessage");

    if (failure == "-5000" && attempt == 1 && code.empty()) {
      // Apple answers the first probe with an invalid-credentials status for
      // accounts that require two-factor verification.
      attempt = 2;
      continue;
    }
    if (customer_message == std::string(kBadLoginMessage)) {
      if (code.empty()) {
        LoginResult result;
        result.two_factor_required = true;
        return result;
      }
      return Error{ErrorCode::kUnauthorized,
                   "Apple did not accept the verification code", http.status};
    }
    if (customer_message == std::string(kAccountDisabledMessage)) {
      return Error{ErrorCode::kUnauthorized, "the Apple Account is disabled",
                   http.status};
    }
    if (!failure.empty()) {
      auto status = map_store_failure(failure, customer_message, false);
      return status.error();
    }

    const std::string token_value = root.string_or("passwordToken");
    const std::string dsid = root.string_or("dsPersonId");
    if (http.status != 200 || token_value.empty() || dsid.empty()) {
      return Error{ErrorCode::kUnauthorized,
                   "Apple returned no usable account credentials", http.status};
    }

    Account account;
    account.email = request.email;
    account.password = request.password;
    account.password_token = token_value;
    account.directory_services_id = dsid;
    account.storefront = http.header("X-Set-Apple-Store-Front");
    if (account.storefront.empty()) {
      return make_error(ErrorCode::kUnauthorized,
                        "Apple did not return a storefront");
    }
    account.pod = http.header("pod");
    account.region = region_or_default(request.region);
    account.device_id = guid;

    if (const PlistValue* info = root.find("accountInfo");
        info != nullptr && info->is_dictionary()) {
      if (auto email = info->string_for("appleId"); email && !email->empty()) {
        account.email = *email;
      }
      std::string first;
      std::string last;
      if (const PlistValue* address = info->find("address");
          address != nullptr && address->is_dictionary()) {
        first = address->string_or("firstName");
        last = address->string_or("lastName");
      }
      account.name = trim(first + " " + last);
    }
    if (account.name.empty()) account.name = account.email;

    LoginResult result;
    result.account = std::move(account);
    return result;
  }
}

Result<HttpResponse> StoreClient::send_plist_post(
    const std::string& url, const HttpHeaders& request_headers,
    const PlistValue& payload, bool sign, SapSigner* signer,
    const CancellationToken& token) {
  auto bytes = plist_body_to_bytes(payload);
  if (!bytes) return bytes.error();
  HttpRequest request;
  request.method = "POST";
  request.url = url;
  request.headers = request_headers;
  request.headers.set("Content-Type", "application/x-apple-plist");
  request.headers.set("User-Agent", configuration_.user_agent);
  request.body = bytes.value();
  if (sign && signer != nullptr) {
    auto signature = signer->sign(request.body);
    if (!signature) return signature.error();
    request.headers.set("X-Apple-ActionSignature",
                        base64_encode(signature.value()));
  }
  return transport_.send(request, token);
}

std::string build_search_url(const SearchQuery& query) {
  auto entity = search_entity(query.platform);
  const std::string entity_value =
      entity ? std::move(entity).value() : std::string("software");
  const int limit = std::clamp(query.limit, 1, 200);
  std::vector<std::pair<std::string, std::string>> params = {
      {"entity", entity_value},
      {"limit", std::to_string(limit)},
      {"media", "software"},
      {"term", trim(query.term)},
      {"country", region_or_default(query.region)},
  };
  if (query.genre_id > 0) {
    params.emplace_back("genreId", std::to_string(query.genre_id));
  }
  return "https://" + std::string(kITunesHost) + "/search?" + build_query(params);
}

Result<SearchResults> StoreClient::search(const SearchQuery& query,
                                          const CancellationToken& token) {
  if (trim(query.term).empty()) {
    return make_error(ErrorCode::kInvalidArgument, "search term is empty");
  }
  const std::string url = build_search_url(query);
  HttpRequest request;
  request.method = "GET";
  request.url = url;
  request.headers.set("Accept", "application/json");
  request.headers.set("User-Agent", configuration_.user_agent);
  auto response = transport_.send(request, token);
  if (!response) return response.error();
  if (response.value().status != 200) {
    return Error{ErrorCode::kHttpStatus,
                 "App Store search returned HTTP " +
                     std::to_string(response.value().status),
                 response.value().status};
  }
  auto results = parse_search_document(
      std::string_view(reinterpret_cast<const char*>(response.value().body.data()),
                       response.value().body.size()));
  if (!results) return results.error();
  auto& apps = results.value().results;
  if (query.platform == Platform::kWatchOS) {
    apps.erase(std::remove_if(apps.begin(), apps.end(), [](const App& app) {
                 return std::find(app.platforms.begin(), app.platforms.end(),
                                  Platform::kWatchOS) == app.platforms.end();
               }),
               apps.end());
  }
  if (query.free_only.has_value()) {
    const bool want_free = query.free_only.value();
    apps.erase(std::remove_if(apps.begin(), apps.end(),
                              [want_free](const App& app) {
                                return app.is_free() != want_free;
                              }),
               apps.end());
  }
  const std::string sort = to_lower(trim(query.sort));
  if (sort == "name") {
    std::stable_sort(apps.begin(), apps.end(), [](const App& lhs, const App& rhs) {
      return to_lower(lhs.name) < to_lower(rhs.name);
    });
  } else if (sort == "rating") {
    std::stable_sort(apps.begin(), apps.end(), [](const App& lhs, const App& rhs) {
      if (lhs.average_rating != rhs.average_rating) {
        return lhs.average_rating > rhs.average_rating;
      }
      return lhs.rating_count > rhs.rating_count;
    });
  } else if (sort == "recent") {
    std::stable_sort(apps.begin(), apps.end(), [](const App& lhs, const App& rhs) {
      return lhs.release_date > rhs.release_date;
    });
  }
  results.value().count = static_cast<int>(apps.size());
  return results;
}

Result<SearchResults> StoreClient::search(std::string_view term,
                                          std::string_view region,
                                          Platform platform, int limit,
                                          const CancellationToken& token) {
  SearchQuery query;
  query.term = std::string(term);
  query.region = std::string(region);
  query.platform = platform;
  query.limit = limit;
  return search(query, token);
}

Result<ReviewsPage> parse_reviews_document(std::string_view body, int page) {
  auto document = Json::parse(body);
  if (!document) return document.error();
  const Json* feed = document.value().find("feed");
  if (feed == nullptr || !feed->is_object()) {
    return make_error(ErrorCode::kParse, "review feed has no feed object");
  }
  ReviewsPage output;
  output.page = page;
  if (const Json* links = feed->find("link"); links != nullptr) {
    const auto check_link = [&output](const Json& entry) {
      const Json* attributes = entry.find("attributes");
      if (attributes == nullptr) return;
      if (attributes->find("rel") != nullptr &&
          attributes->find("rel")->string_or() == "next") {
        output.has_more = true;
      }
    };
    if (links->is_array()) {
      for (const auto& entry : links->array_items()) check_link(entry);
    } else {
      check_link(*links);
    }
  }
  const Json* entries = feed->find("entry");
  if (entries == nullptr) return output;
  const auto parse_entry = [&output](const Json& entry) {
    if (!entry.is_object()) return;
    Review review;
    const auto label = [](const Json* value) -> std::string {
      if (value == nullptr || !value->is_object()) return {};
      const Json* text = value->find("label");
      return text == nullptr ? std::string() : text->string_or();
    };
    review.id = label(entry.find("id"));
    review.title = label(entry.find("title"));
    review.body = label(entry.find("content"));
    review.updated = label(entry.find("updated"));
    review.version = label(entry.find("im:version"));
    const Json* author = entry.find("author");
    if (author != nullptr) review.author = label(author->find("name"));
    const Json* rating = entry.find("im:rating");
    if (rating != nullptr) {
      const std::string text = label(rating);
      review.rating = std::atoi(text.c_str());
    }
    if (review.id.empty() && review.title.empty() && review.body.empty()) {
      return;
    }
    output.reviews.push_back(std::move(review));
  };
  if (entries->is_array()) {
    for (const auto& entry : entries->array_items()) parse_entry(entry);
  } else {
    parse_entry(*entries);
  }
  return output;
}

Result<ReviewsPage> StoreClient::reviews(std::int64_t app_id,
                                         std::string_view region, int page,
                                         const CancellationToken& token) {
  if (app_id <= 0) {
    return make_error(ErrorCode::kInvalidArgument, "app id is required");
  }
  const int safe_page = std::clamp(page, 1, 10);
  const std::string country = region_or_default(region);
  const std::string url =
      "https://" + std::string(kITunesHost) + "/" + country +
      "/rss/customerreviews/page=" + std::to_string(safe_page) + "/id=" +
      std::to_string(app_id) + "/sortby=mostrecent/json";
  HttpRequest request;
  request.method = "GET";
  request.url = url;
  request.headers.set("Accept", "application/json");
  request.headers.set("User-Agent", configuration_.user_agent);
  auto response = transport_.send(request, token);
  if (!response) return response.error();
  if (response.value().status == 404) {
    return make_error(ErrorCode::kParse, "this app has no public reviews");
  }
  if (response.value().status != 200) {
    return Error{ErrorCode::kHttpStatus,
                 "review feed returned HTTP " +
                     std::to_string(response.value().status),
                 response.value().status};
  }
  auto parsed = parse_reviews_document(
      std::string_view(reinterpret_cast<const char*>(response.value().body.data()),
                       response.value().body.size()),
      safe_page);
  return parsed;
}

Result<App> StoreClient::lookup(std::int64_t app_id,
                                std::string_view bundle_id,
                                std::string_view region, Platform platform,
                                const CancellationToken& token) {
  if (app_id <= 0 && bundle_id.empty()) {
    return make_error(ErrorCode::kInvalidArgument,
                      "app id or bundle id is required");
  }
  auto entity = lookup_entity(platform);
  if (!entity) return entity.error();
  std::vector<std::pair<std::string, std::string>> query = {
      {"entity", entity.value()},
      {"limit", "1"},
      {"media", "software"},
  };
  if (app_id > 0) {
    query.emplace_back("id", std::to_string(app_id));
  } else {
    query.emplace_back("bundleId", std::string(bundle_id));
  }
  query.emplace_back("country", region_or_default(region));

  HttpRequest request;
  request.method = "GET";
  request.url = "https://" + std::string(kITunesHost) + "/lookup?" +
                build_query(query);
  request.headers.set("Accept", "application/json");
  request.headers.set("User-Agent", configuration_.user_agent);
  auto response = transport_.send(request, token);
  if (!response) return response.error();
  if (response.value().status != 200) {
    return Error{ErrorCode::kHttpStatus,
                 "App Store lookup returned HTTP " +
                     std::to_string(response.value().status),
                 response.value().status};
  }
  auto results = parse_search_document(
      std::string_view(reinterpret_cast<const char*>(response.value().body.data()),
                       response.value().body.size()));
  if (!results) return results.error();
  if (results.value().results.empty()) {
    return make_error(ErrorCode::kAppNotFound, "app was not found in this region");
  }
  if (platform == Platform::kWatchOS &&
      std::find(results.value().results.front().platforms.begin(),
                results.value().results.front().platforms.end(),
                Platform::kWatchOS) ==
          results.value().results.front().platforms.end()) {
    return make_error(ErrorCode::kAppNotFound,
                      "app has no watchOS build in this region");
  }
  return results.value().results.front();
}

Result<std::string> StoreClient::latest_external_version_id(
    const App& app, std::string_view region, Platform platform,
    const CancellationToken& token) {
  if (app.id <= 0) {
    return make_error(ErrorCode::kInvalidArgument, "app id is required");
  }
  auto catalog = catalog_platform(platform);
  if (!catalog) return catalog.error();
  const std::string country = region_or_default(region);
  const std::string url = "https://apps.apple.com/api/apps/v1/catalog/" +
                          percent_encode(country) + "/apps/" +
                          std::to_string(app.id) + "?platform=" +
                          percent_encode(catalog.value());
  HttpRequest request;
  request.method = "GET";
  request.url = url;
  request.headers.set("Accept", "application/json");
  request.headers.set("User-Agent", configuration_.user_agent);
  auto response = transport_.send(request, token);
  if (!response) return response.error();
  if (response.value().status == 404) {
    return Error{ErrorCode::kAppNotFound,
                 "app is not present in the public catalog", 404};
  }
  if (response.value().status != 200) {
    return Error{ErrorCode::kHttpStatus,
                 "catalog lookup returned HTTP " +
                     std::to_string(response.value().status),
                 response.value().status};
  }
  auto document = Json::parse(std::string_view(
      reinterpret_cast<const char*>(response.value().body.data()),
      response.value().body.size()));
  if (!document) return document.error();
  const Json* data = document.value().find("data");
  if (data == nullptr || !data->is_array() || data->array_items().size() != 1) {
    return make_error(ErrorCode::kParse,
                      "catalog response must contain exactly one app");
  }
  const Json& item = data->array_items().front();
  if (item.find("id") == nullptr || item.find("id")->int_or() != app.id) {
    return make_error(ErrorCode::kParse, "catalog returned a different app");
  }
  if (item.find("type") != nullptr && item.find("type")->string_or() != "apps") {
    return make_error(ErrorCode::kParse, "catalog returned a different resource");
  }
  const Json* attributes = item.find("attributes");
  if (attributes == nullptr || !attributes->is_object()) {
    return make_error(ErrorCode::kParse, "catalog response has no attributes");
  }
  const std::string wanted = catalog.value();
  bool family_match = false;
  if (const Json* families = attributes->find("deviceFamilies");
      families != nullptr && families->is_array()) {
    for (const auto& family : families->array_items()) {
      if (family.string_or() == wanted) {
        family_match = true;
        break;
      }
    }
  }
  if (!family_match) {
    return make_error(ErrorCode::kAppNotFound,
                      "catalog has no build for the requested platform");
  }
  const Json* platform_attributes = attributes->find("platformAttributes");
  if (platform_attributes == nullptr || !platform_attributes->is_object()) {
    return make_error(ErrorCode::kParse,
                      "catalog response has no platform attributes");
  }
  const Json* platform_entry = platform_attributes->find(
      (platform == Platform::kIPhone || platform == Platform::kIPad)
          ? "ios"
          : wanted);
  if (platform_entry == nullptr) platform_entry = platform_attributes->find("ios");
  if (platform_entry == nullptr || !platform_entry->is_object()) {
    return make_error(ErrorCode::kAppNotFound,
                      "catalog has no version metadata for the platform");
  }
  if (const Json* bundle = platform_entry->find("bundleId");
      bundle != nullptr && !app.bundle_id.empty() &&
      bundle->string_or() != app.bundle_id) {
    return make_error(ErrorCode::kParse,
                      "catalog returned a different bundle identifier");
  }
  const Json* external = platform_entry->find("externalVersionId");
  if (external == nullptr) {
    return make_error(ErrorCode::kParse,
                      "catalog response has no external version id");
  }
  std::string version;
  if (external->is_string()) {
    version = external->string_or();
  } else if (external->is_number()) {
    version = external->dump();
  }
  if (version.empty() || version == "0") {
    return make_error(ErrorCode::kParse,
                      "catalog response has an invalid external version id");
  }
  return version;
}

Result<DownloadOffer> StoreClient::request_download_at(
    const Account& account, const App& app, std::string_view base_url,
    const char* version_key, std::string_view external_version_id,
    std::string_view guid, Platform platform, const CancellationToken& token,
    int* http_status) {
  const std::string url =
      apply_pod(base_url, account.pod) + "?guid=" + percent_encode(guid);
  const Bytes body = build_download_body(app.id, guid,
                                         version_key == nullptr ? "" : version_key,
                                         external_version_id);
  HttpRequest request;
  request.method = "POST";
  request.url = url;
  request.headers.set("Content-Type", "application/x-apple-plist");
  request.headers.set("iCloud-DSID", account.directory_services_id);
  request.headers.set("X-Dsid", account.directory_services_id);
  request.headers.set("User-Agent", configuration_.user_agent);
  request.body = body;
  auto response = transport_.send(request, token);
  if (!response) return response.error();
  if (http_status != nullptr) *http_status = response.value().status;
  if (response.value().status != 200) {
    return Error{ErrorCode::kHttpStatus,
                 "download request returned HTTP " +
                     std::to_string(response.value().status),
                 response.value().status};
  }
  auto document = decode_plist_response(response.value());
  if (!document) return document.error();
  if (auto failure = document.value().string_for("failureType");
      failure && !failure->empty()) {
    const std::string message = document.value().string_or("customerMessage");
    auto status = map_store_failure(*failure, message, false);
    if (!status) return status.error();
  }
  if (auto message = document.value().string_for("customerMessage");
      message && !message->empty()) {
    const ErrorCode code =
        to_lower(*message).find("no longer available") != std::string::npos
            ? ErrorCode::kTemporarilyUnavailable
            : ErrorCode::kUnauthorized;
    return Error{code, *message, response.value().status};
  }
  auto offer = parse_download_response(document.value());
  if (!offer) return offer.error();
  if (!download_item_matches(offer.value(), app, external_version_id)) {
    return make_error(ErrorCode::kParse,
                      "download response does not match the app");
  }
  (void)platform;
  return offer;
}

Result<DownloadOffer> StoreClient::send_unpinned_redownload(
    const Account& account, const App& app, std::string_view endpoint,
    std::string_view guid, const CancellationToken& token) {
  HttpRequest request;
  request.method = "POST";
  request.url = apply_pod(endpoint, account.pod) + "?guid=" +
                percent_encode(guid);
  const Bytes body = build_download_body(app.id, guid, "", "");
  request.headers.set("Content-Type", "application/x-apple-plist");
  request.headers.set("iCloud-DSID", account.directory_services_id);
  request.headers.set("X-Dsid", account.directory_services_id);
  request.headers.set("User-Agent", configuration_.user_agent);
  request.body = body;
  auto response = transport_.send(request, token);
  if (!response) return response.error();
  if (response.value().status != 200) {
    return Error{ErrorCode::kHttpStatus,
                 "redownload returned HTTP " +
                     std::to_string(response.value().status),
                 response.value().status};
  }
  auto document = decode_plist_response(response.value());
  if (!document) return document.error();
  if (auto failure = document.value().string_for("failureType");
      failure && !failure->empty()) {
    auto status = map_store_failure(*failure,
                                    document.value().string_or("customerMessage"),
                                    false);
    if (!status) return status.error();
  }
  if (auto message = document.value().string_for("customerMessage");
      message && !message->empty()) {
    return Error{ErrorCode::kTemporarilyUnavailable, *message, 200};
  }
  auto offer = parse_download_response(document.value());
  if (!offer) return offer.error();
  if (!download_item_matches(offer.value(), app, "")) {
    return make_error(ErrorCode::kParse,
                      "redownload response does not match the app");
  }
  return offer;
}

Result<DownloadOffer> StoreClient::download_offer(
    const Account& account, const App& app,
    std::string_view external_version_id, Platform platform,
    const CancellationToken& token) {
  if (app.id <= 0) {
    return make_error(ErrorCode::kInvalidArgument, "app id is required");
  }
  auto hardware = HardwareIdentity::from_hex(account.device_id);
  if (!hardware) return hardware.error();
  auto bag = fetch_bag(hardware.value(), token);
  if (!bag) return bag.error();
  const std::string guid = hardware.value().guid();

  std::string volume_url = bag.value().volume_store_endpoint;
  if (volume_url.empty()) {
    volume_url = "https://buy.itunes.apple.com" + std::string(kVolumePath);
  }
  int first_status = 0;
  auto first = request_download_at(account, app, volume_url, "externalVersionId",
                                   external_version_id, guid, platform, token,
                                   &first_status);
  if (first) {
    return first;
  }
  const bool empty_or_unavailable =
      first.error().code == ErrorCode::kNoOffer ||
      first.error().code == ErrorCode::kTemporarilyUnavailable;
  if (!empty_or_unavailable) {
    return first.error();
  }

  if (bag.value().redownload_endpoint.empty()) {
    return first.error();
  }

  std::string version(external_version_id);
  if (version.empty() &&
      (platform == Platform::kIPhone || platform == Platform::kIPad)) {
    auto latest = latest_external_version_id(app, account.region, platform, token);
    if (!latest) {
      if (latest.error().code == ErrorCode::kAppNotFound) {
        return send_unpinned_redownload(account, app,
                                        bag.value().redownload_endpoint, guid,
                                        token);
      }
      return latest.error();
    }
    version = latest.value();
  }

  int redownload_status = 0;
  auto redownload = request_download_at(
      account, app, bag.value().redownload_endpoint, "appExtVrsId", version,
      guid, platform, token, &redownload_status);
  if (redownload) return redownload;

  const bool retry_with_update =
      !bag.value().update_endpoint.empty() && !version.empty() &&
      (redownload_status == 500 ||
       redownload.error().code == ErrorCode::kNoOffer ||
       redownload.error().code == ErrorCode::kTemporarilyUnavailable);
  if (retry_with_update) {
    auto update = request_download_at(account, app,
                                      bag.value().update_endpoint,
                                      "appExtVrsId", version, guid, platform,
                                      token, nullptr);
    if (update) return update;
    if (update.error().code != ErrorCode::kNoOffer) {
      return update.error();
    }
  }
  return redownload.error();
}

Status StoreClient::purchase(const Account& account, const App& app,
                             Platform platform,
                             const CancellationToken& token) {
  if (app.price > 0.0001) {
    return Status{Error{ErrorCode::kUnsupported,
                        "paid apps cannot be purchased by this client"}};
  }
  if (account.password_token.empty() || account.directory_services_id.empty()) {
    return Status{Error{ErrorCode::kUnauthorized, "account is not signed in"}};
  }
  std::string base = "https://buy.itunes.apple.com" + std::string(kPurchasePath);
  base = apply_pod(base, account.pod);
  auto hardware = HardwareIdentity::from_hex(account.device_id);
  if (!hardware) return hardware.error();
  const std::string guid = hardware.value().guid();

  const auto attempt_purchase = [&](std::string_view pricing) -> Status {
    PlistValue payload = PlistValue::dictionary();
    payload.set("appExtVrsId", PlistValue::string("0"));
    payload.set("hasAskedToFulfillPreorder", PlistValue::string("true"));
    payload.set("buyWithoutAuthorization", PlistValue::string("true"));
    payload.set("hasDoneAgeCheck", PlistValue::string("true"));
    payload.set("guid", PlistValue::string(guid));
    payload.set("needDiv", PlistValue::string("0"));
    payload.set("origPage", PlistValue::string("Software-" +
                                               std::to_string(app.id)));
    payload.set("origPageLocation", PlistValue::string("Buy"));
    payload.set("price", PlistValue::string(std::string(kPriceFree)));
    payload.set("pricingParameters", PlistValue::string(std::string(pricing)));
    payload.set("productType", PlistValue::string(std::string(kProductTypeApp)));
    payload.set("salableAdamId", PlistValue::integer(app.id));

    HttpHeaders headers;
    headers.set("iCloud-DSID", account.directory_services_id);
    headers.set("X-Dsid", account.directory_services_id);
    headers.set("X-Apple-Store-Front", account.storefront);
    headers.set("X-Token", account.password_token);
    headers.set("User-Agent", std::string(kPurchaseUserAgent));

    auto response = send_plist_post(base, headers, payload, false, nullptr, token);
    if (!response) return response.error();
    if (response.value().status != 200) {
      if (response.value().status == 500) {
        IPASHELF_LOG_INFO("purchase returned HTTP 500; assuming an existing license");
        return ok_status();
      }
      return Error{ErrorCode::kHttpStatus,
                   "purchase returned HTTP " +
                       std::to_string(response.value().status),
                   response.value().status};
    }
    auto document = decode_plist_response(response.value());
    if (!document) return document.error();
    const PlistValue& root = document.value();
    const std::string failure = root.string_or("failureType");
    const std::string message = root.string_or("customerMessage");
    if (message == std::string(kSubscriptionMessage)) {
      return Status{Error{ErrorCode::kUnsupported,
                          "this item requires a subscription"}};
    }
    if (failure == "5002") return ok_status();
    if (failure == "2059") {
      return Status{Error{ErrorCode::kTemporarilyUnavailable,
                          "the item is temporarily unavailable"}};
    }
    if (failure == "2034" || failure == "2042" || failure == "1008" ||
        message == std::string(kPasswordChangedMessage)) {
      return Status{Error{ErrorCode::kTokenExpired,
                          "the account needs to sign in again"}};
    }
    if (!failure.empty()) {
      auto status = map_store_failure(failure, message, true);
      if (!status) return status;
    }
    const std::string jingle = root.string_or("jingleDocType");
    const std::int64_t document_status =
        root.integer_for("status").value_or(0);
    if (jingle != "purchaseSuccess" || document_status != 0) {
      return Status{Error{ErrorCode::kUnauthorized,
                          message.empty() ? "Apple did not confirm the purchase"
                                          : message}};
    }
    return ok_status();
  };

  auto result = attempt_purchase(kPricingStandard);
  if (!result && result.error().code == ErrorCode::kTemporarilyUnavailable &&
      platform != Platform::kMacOS) {
    auto arcade = attempt_purchase(kPricingArcade);
    if (arcade) return ok_status();
    return arcade;
  }
  return result;
}

Result<std::vector<std::string>> StoreClient::version_identifiers(
    const Account& account, const App& app, Platform platform,
    const CancellationToken& token) {
  auto offer = download_offer(account, app, "", platform, token);
  if (!offer) return offer.error();
  std::vector<std::string> versions;
  const PlistValue* list =
      offer.value().metadata.find("softwareVersionExternalIdentifiers");
  if (list != nullptr && list->is_array()) {
    for (const auto& entry : list->array_items()) {
      if (entry.is_integer()) {
        versions.push_back(std::to_string(entry.integer_value()));
      } else if (entry.is_string()) {
        versions.push_back(entry.string_value());
      }
    }
  }
  const std::string latest = offer.value().external_version_id;
  if (!latest.empty() &&
      std::find(versions.begin(), versions.end(), latest) == versions.end()) {
    versions.insert(versions.begin(), latest);
  }
  if (versions.empty()) {
    return make_error(ErrorCode::kParse,
                      "download response has no version identifiers");
  }
  return versions;
}

}  // namespace ipashelf
