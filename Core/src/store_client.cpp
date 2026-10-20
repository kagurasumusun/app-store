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

#include "miniz.h"

namespace ipashelf {
namespace {

constexpr std::string_view kITunesHost = "itunes.apple.com";
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

// Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's
// civil calendar algorithm).
std::int64_t days_from_civil(std::int64_t year, unsigned month, unsigned day) {
  year -= month <= 2 ? 1 : 0;
  const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
  const unsigned year_of_era =
      static_cast<unsigned>(year - era * 400);
  const unsigned day_of_year =
      (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
  const unsigned day_of_era = year_of_era * 365 + year_of_era / 4 -
                              year_of_era / 100 + day_of_year;
  return era * 146097 + static_cast<std::int64_t>(day_of_era) - 719468;
}

bool parse_http_date(std::string_view text, std::int64_t& epoch_seconds) {
  // Accepts the three HTTP-date formats (IMF-fixdate, RFC 850, asctime).
  static constexpr const char* kMonths[12] = {
      "Jan", "Feb", "Mar", "Apr", "May", "Jun",
      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  const auto digits_at = [&](std::size_t offset, std::size_t count,
                             int& value) {
    if (offset + count > text.size()) return false;
    value = 0;
    for (std::size_t index = 0; index < count; ++index) {
      const char character = text[offset + index];
      if (character < '0' || character > '9') return false;
      value = value * 10 + (character - '0');
    }
    return true;
  };
  const auto month_at = [&](std::size_t offset, int& month) {
    if (offset + 3 > text.size()) return false;
    for (int index = 0; index < 12; ++index) {
      if (text.compare(offset, 3, kMonths[index]) == 0) {
        month = index + 1;
        return true;
      }
    }
    return false;
  };
  const auto time_at = [&](std::size_t offset, int& seconds_of_day) {
    int hour = 0, minute = 0, second = 0;
    if (!digits_at(offset, 2, hour) || offset + 8 > text.size() ||
        text[offset + 2] != ':' || !digits_at(offset + 3, 2, minute) ||
        text[offset + 5] != ':' || !digits_at(offset + 6, 2, second)) {
      return false;
    }
    if (hour > 23 || minute > 59 || second > 60) return false;
    seconds_of_day = hour * 3600 + minute * 60 + second;
    return true;
  };

  int day = 0, month = 0, year = 0, seconds_of_day = 0;
  // IMF-fixdate: "Sun, 06 Nov 1994 08:49:37 GMT"
  if (text.size() == 29 && text[3] == ',' && text[4] == ' ' &&
      digits_at(5, 2, day) && text[7] == ' ' && month_at(8, month) &&
      text[11] == ' ' && digits_at(12, 4, year) && text[16] == ' ' &&
      time_at(17, seconds_of_day) && text.compare(25, 4, " GMT") == 0) {
    epoch_seconds = days_from_civil(year, month, day) * 86400 +
                    seconds_of_day;
    return true;
  }
  // RFC 850: "Sunday, 06-Nov-94 08:49:37 GMT"
  const std::size_t comma = text.find(',');
  if (comma != std::string_view::npos && text.size() == comma + 24 &&
      text[comma + 1] == ' ' && digits_at(comma + 2, 2, day) &&
      text[comma + 4] == '-' && month_at(comma + 5, month) &&
      text[comma + 8] == '-' && digits_at(comma + 9, 2, year) &&
      text[comma + 11] == ' ' && time_at(comma + 12, seconds_of_day) &&
      text.compare(comma + 20, 4, " GMT") == 0) {
    year += year < 70 ? 2000 : 1900;
    epoch_seconds = days_from_civil(year, month, day) * 86400 +
                    seconds_of_day;
    return true;
  }
  // asctime: "Sun Nov  6 08:49:37 1994"
  if (text.size() == 24 && month_at(4, month) && text[7] == ' ' &&
      (text[8] == ' ' || (text[8] >= '0' && text[8] <= '9')) &&
      digits_at(9, 1, day) && text[10] == ' ' &&
      time_at(11, seconds_of_day) && text[19] == ' ' &&
      digits_at(20, 4, year)) {
    if (text[8] >= '0' && text[8] <= '9') day += (text[8] - '0') * 10;
    epoch_seconds = days_from_civil(year, month, day) * 86400 +
                    seconds_of_day;
    return true;
  }
  return false;
}

std::chrono::seconds parse_retry_after(
    std::string_view value, std::chrono::system_clock::time_point now) {
  const std::string trimmed = trim(value);
  if (trimmed.empty()) return std::chrono::seconds(0);
  char* end = nullptr;
  const long long seconds = std::strtoll(trimmed.c_str(), &end, 10);
  if (end != nullptr && *end == '\0' && seconds >= 0) {
    // Saturate rather than overflow; a wait beyond the caller's budget ends
    // the login instead of retrying before Apple's deadline.
    return std::chrono::seconds(
        std::min<long long>(seconds, 3600 * 24));
  }
  std::int64_t epoch = 0;
  if (parse_http_date(trimmed, epoch)) {
    const auto now_seconds =
        std::chrono::duration_cast<std::chrono::seconds>(
            now.time_since_epoch())
            .count();
    if (epoch > now_seconds) {
      return std::chrono::seconds(
          std::min<std::int64_t>(epoch - now_seconds, 3600 * 24));
    }
    return std::chrono::seconds(0);
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
  // The Store authentication endpoint expects an XML property list whose
  // values are all strings, even though the request carries the historical
  // form content type. A form-encoded body is never accepted.
  const std::string code = strip_whitespace(auth_code);
  PlistValue body = PlistValue::dictionary();
  body.set("appleId", PlistValue::string(std::string(email)));
  body.set("attempt", PlistValue::string(std::to_string(attempt)));
  body.set("guid", PlistValue::string(std::string(guid)));
  body.set("password",
           PlistValue::string(std::string(password) + code));
  body.set("rmp", PlistValue::string("0"));
  body.set("why", PlistValue::string("signIn"));
  const std::string xml = body.dump_xml();
  return Bytes(xml.begin(), xml.end());
}

std::string phone_storefront_header(std::string_view apple_id) {
  // Supplies a routing hint for phone-based Apple Accounts in mainland
  // China and India. It does not change the Apple ID sent to Apple.
  std::string number = trim(apple_id);
  std::string digits;
  for (const char character : number) {
    if (character != ' ' && character != '-') digits.push_back(character);
  }
  number = std::move(digits);

  bool international = false;
  if (starts_with(number, "+")) {
    international = true;
    number.erase(number.begin());
  } else if (starts_with(number, "00")) {
    international = true;
    number.erase(0, 2);
  }

  for (const char digit : number) {
    if (digit < '0' || digit > '9') return {};
  }

  const auto storefront_id = [](std::string_view region) {
    auto storefront = storefront_for_region(region);
    return storefront ? storefront->storefront_id : std::string{};
  };

  // Check the full length before removing a country code: a local Indian
  // number can itself start with 91. Explicit international numbers must
  // match a supported country code and cannot fall back to a local format.
  if (number.size() == 13 && starts_with(number, "861")) {
    return storefront_id("CN");
  }
  if (number.size() == 12 && starts_with(number, "91")) {
    number.erase(0, 2);
  } else if (international) {
    return {};
  }

  if (number.size() == 11 && number[0] == '1') return storefront_id("CN");

  // India also permits a trunk prefix when writing a national number.
  if (!international && number.size() == 11 && number[0] == '0') {
    number.erase(number.begin());
  }

  if (number.size() == 10 && number[0] >= '6' && number[0] <= '9') {
    return storefront_id("IN");
  }
  return {};
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
    const HttpRequest& request, SapSigner* signer,
    const CancellationToken& token) {
  const int max_attempts = std::max(1, configuration_.max_authentication_attempts);
  std::vector<std::string> statuses;
  for (int attempt = 1; attempt <= max_attempts; ++attempt) {
    if (token.cancelled()) {
      return make_error(ErrorCode::kCancelled, "sign-in cancelled");
    }
    // Every transmission is signed afresh: SAP signatures carry a fresh IV
    // drawn from the session stream and must not be replayed verbatim.
    HttpRequest attempt_request = request;
    if (signer != nullptr) {
      auto signature = signer->sign(attempt_request.body);
      if (!signature) return signature.error();
      attempt_request.headers.set("X-Apple-ActionSignature",
                                  base64_encode(signature.value()));
    }
    auto response = transport_.send(attempt_request, token);
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
    auto retry_after = parse_retry_after(
        response.value().header("Retry-After"),
        std::chrono::system_clock::now());
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

  Bag bag;
  auto established = open_sap_session(request.hardware, token, &bag);
  if (!established) return established.error();
  std::unique_ptr<SapSession> session = std::move(established).value();

  auto result = perform_login(request, bag, session.get(), token);
  auto close_status = session->close();
  if (!close_status && result) {
    return close_status.error();
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
    if (const std::string storefront =
            phone_storefront_header(request.email);
        !storefront.empty()) {
      http_request.headers.set("X-Apple-Store-Front", storefront);
    }
    http_request.body = body;

    auto response = send_login_request(http_request, signer, token);
    if (!response) return response.error();
    const HttpResponse& http = response.value();

    if (http.status >= 300 && http.status < 400) {
      // Only these statuses may carry an authentication redirect; the
      // destination must itself be a valid authentication endpoint.
      const bool redirect_status = http.status == 301 || http.status == 302 ||
                                   http.status == 307 || http.status == 308;
      const std::string location = http.header("Location");
      if (!redirect_status || location.empty()) {
        return Error{ErrorCode::kHttpStatus,
                     "unsupported Apple authentication redirect",
                     http.status};
      }
      if (redirects >= 4) {
        return Error{ErrorCode::kHttpStatus,
                     "too many Apple authentication redirects", http.status};
      }
      auto resolved = resolve_url(url, location);
      if (!resolved) return resolved.error();
      if (auto status = validate_auth_endpoint(resolved.value()); !status) {
        return Error{ErrorCode::kHttpStatus,
                     "Apple authentication redirect points to an unexpected "
                     "endpoint",
                     http.status};
      }
      // Only the destination changes: redirects belong to the same login
      // attempt and retain its payload, including the attempt value.
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

    if (failure == "-5000" && attempt == 1) {
      // The first exchange is a probe: Apple answers it with an
      // invalid-credentials status even for otherwise valid sign-ins.
      attempt = 2;
      continue;
    }
    if (failure.empty() &&
        customer_message == std::string(kBadLoginMessage)) {
      if (code.empty()) {
        LoginResult result;
        result.two_factor_required = true;
        return result;
      }
      return Error{ErrorCode::kUnauthorized,
                   "Apple did not complete verification; try a fresh "
                   "verification code",
                   http.status};
    }
    if (failure.empty() &&
        customer_message == std::string(kAccountDisabledMessage)) {
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

// ---------------------------------------------------------------------------
// SAP session sharing
// ---------------------------------------------------------------------------

Result<std::unique_ptr<SapSession>> StoreClient::open_sap_session(
    const HardwareIdentity& hardware, const CancellationToken& token,
    Bag* bag_out) {
  auto bag = fetch_bag(hardware, token);
  if (!bag) return bag.error();
  // SAP setup is mandatory: Apple rejects unsigned requests before looking
  // at their contents, so there is no unsigned fallback.
  if (auto status = bag.value().validate_for_authentication(); !status) {
    return status.error();
  }

  SapConfiguration configuration;
  configuration.setup_url = bag.value().sap_setup_url;
  configuration.certificate_url = bag.value().sap_setup_cert_url;
  configuration.version =
      static_cast<std::uint32_t>(bag.value().sap_version);
  configuration.hardware = hardware;

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
  if (bag_out != nullptr) *bag_out = bag.value();
  return established;
}

// ---------------------------------------------------------------------------
// Download product
// ---------------------------------------------------------------------------

namespace {

struct DownloadDocument {
  PlistValue root = PlistValue::dictionary();
  int http_status = 0;
};

Result<DownloadDocument> post_download_document(
    HttpTransport& transport, const std::string& url, const Bytes& body,
    const HttpHeaders& headers, const CancellationToken& token) {
  HttpRequest request;
  request.method = "POST";
  request.url = url;
  request.headers = headers;
  request.body = body;
  auto response = transport.send(request, token);
  if (!response) return response.error();
  if (response.value().status != 200) {
    return Error{ErrorCode::kHttpStatus,
                 "download request returned HTTP " +
                     std::to_string(response.value().status),
                 response.value().status};
  }
  auto document = decode_plist_response(response.value());
  if (!document) return document.error();
  return DownloadDocument{std::move(document).value(),
                          response.value().status};
}

HttpHeaders download_headers(const Account& account,
                             std::string_view user_agent) {
  HttpHeaders headers;
  headers.set("Content-Type", "application/x-apple-plist");
  headers.set("iCloud-DSID", account.directory_services_id);
  headers.set("X-Dsid", account.directory_services_id);
  headers.set("User-Agent", std::string(user_agent));
  return headers;
}

std::size_t download_item_count(const PlistValue& root) {
  const PlistValue* items = root.find("songList");
  if (items == nullptr || !items->is_array()) return 0;
  return items->array_items().size();
}

bool is_empty_download_response(const DownloadDocument& document) {
  return document.http_status == 200 &&
         document.root.string_or("failureType").empty() &&
         document.root.string_or("customerMessage").empty() &&
         download_item_count(document.root) == 0;
}

bool is_unavailable_message(std::string_view message) {
  const std::string lowered = to_lower(trim(message));
  return lowered == "no longer available" ||
         ends_with(lowered, " no longer available");
}

// Limit recovery to the observed availability response; other customer
// messages and structured failures retain their normal error handling.
bool is_unavailable_download_response(const DownloadDocument& document) {
  return document.http_status == 200 &&
         document.root.string_or("failureType").empty() &&
         download_item_count(document.root) == 0 &&
         is_unavailable_message(document.root.string_or("customerMessage"));
}

// Interprets a download response the way the Store client must: structured
// failures become typed errors, exactly the identity checks the calling
// path promises are applied, and the first item becomes the offer.
Result<DownloadOffer> interpret_download_response(const PlistValue& root,
                                                  const App& app,
                                                  std::string_view version,
                                                  bool strict) {
  const std::string failure = root.string_or("failureType");
  const std::string message = root.string_or("customerMessage");
  const std::size_t items = download_item_count(root);

  if (failure == "2034" || failure == "2042" || failure == "1008" ||
      failure == "5002") {
    return Error{ErrorCode::kTokenExpired,
                 "Apple requires the account to sign in again", 200};
  }
  if (failure == "9610") {
    return Error{ErrorCode::kLicenseRequired,
                 "This account does not own the app yet", 200};
  }
  if (!message.empty() && (!failure.empty() || items == 0)) {
    if (failure.empty() && is_unavailable_message(message)) {
      return Error{ErrorCode::kTemporarilyUnavailable, message, 200};
    }
    return Error{ErrorCode::kUnauthorized, message, 200};
  }
  if (!failure.empty()) {
    auto status = map_store_failure(failure, message, false);
    if (!status) return status.error();
  }
  if (items == 0) {
    return make_error(ErrorCode::kNoOffer,
                      "download response has no items");
  }
  auto offer = parse_download_response(root);
  if (!offer) return offer;
  if (strict) {
    // An empty requested version lets the server select a version while
    // retaining the app identity checks; explicit versions must match.
    if (items != 1 || offer.value().item_id != app.id ||
        (!version.empty() &&
         offer.value().external_version_id != version)) {
      return make_error(
          ErrorCode::kParse,
          "download response does not match the requested app or version");
    }
    if (offer.value().bundle_id.empty() ||
        (!app.bundle_id.empty() &&
         offer.value().bundle_id != app.bundle_id)) {
      return make_error(
          ErrorCode::kParse,
          "download response does not match the requested bundle identifier");
    }
  } else if (!download_item_matches(offer.value(), app, version)) {
    return make_error(ErrorCode::kParse,
                      "download response does not match the app");
  }
  return offer;
}

bool valid_dispatch_endpoint(std::string_view endpoint,
                             std::string_view kind) {
  Bag probe;
  if (kind == "ent") {
    probe.ent_download_endpoint = std::string(endpoint);
  } else if (kind == "redownload") {
    probe.redownload_endpoint = std::string(endpoint);
  } else {
    probe.update_endpoint = std::string(endpoint);
  }
  return probe.validate().ok();
}

std::string volume_store_url(const Account& account) {
  // The legacy volume store URL is not read from the bag (that key now
  // returns the ent/download URL); it is rebuilt from the account pod.
  const std::string pod = sanitize_pod(account.pod);
  return "https://" + (pod.empty() ? std::string{} : "p" + pod + "-") +
         "buy.itunes.apple.com" + std::string(kVolumePath);
}

}  // namespace

std::string StoreClient::cached_kbsync(std::uint64_t dsid,
                                       std::string_view guid) {
  const auto read_entry = [](const std::string& encoded, std::uint64_t wanted,
                             std::string_view wanted_guid) -> std::string {
    auto parsed = Json::parse(encoded);
    if (!parsed || !parsed.value().is_object()) return {};
    const Json* entry_dsid = parsed.value().find("dsid");
    const Json* entry_guid = parsed.value().find("guid");
    const Json* entry_data = parsed.value().find("data");
    if (entry_dsid == nullptr || entry_guid == nullptr ||
        entry_data == nullptr) {
      return {};
    }
    if (static_cast<std::uint64_t>(entry_dsid->int_value()) != wanted ||
        entry_guid->string_or() != wanted_guid) {
      return {};
    }
    const std::string data = entry_data->string_or();
    auto decoded = base64_decode(data);
    if (!decoded || decoded.value().empty()) return {};
    return data;
  };

  std::lock_guard<std::mutex> lock(kbsync_mutex_);
  if (!kbsync_cache_entry_.empty()) {
    if (std::string data = read_entry(kbsync_cache_entry_, dsid, guid);
        !data.empty()) {
      return data;
    }
  }
  if (!configuration_.kbsync_cache_load) return {};
  auto stored = configuration_.kbsync_cache_load();
  if (!stored) return {};
  std::string data = read_entry(*stored, dsid, guid);
  if (!data.empty()) kbsync_cache_entry_ = *stored;
  return data;
}

void StoreClient::store_kbsync(std::uint64_t dsid, std::string_view guid,
                               std::string_view data) {
  // Only blobs that served a valid ent/download response reach this point.
  Json entry = Json::object({
      {"dsid", Json::integer(static_cast<std::int64_t>(dsid))},
      {"guid", Json::string(std::string(guid))},
      {"data", Json::string(std::string(data))},
  });
  const std::string encoded = entry.dump();
  std::lock_guard<std::mutex> lock(kbsync_mutex_);
  kbsync_cache_entry_ = encoded;
  if (configuration_.kbsync_cache_store) {
    // Cache storage is best effort: an unavailable store must not turn a
    // successful generation into a failed download.
    configuration_.kbsync_cache_store(encoded);
  }
}

void StoreClient::invalidate_kbsync(std::uint64_t dsid, std::string_view guid,
                                    std::string_view data) {
  Json rejected = Json::object({
      {"dsid", Json::integer(static_cast<std::int64_t>(dsid))},
      {"guid", Json::string(std::string(guid))},
      {"data", Json::string(std::string(data))},
  });
  const std::string rejected_encoded = rejected.dump();
  std::lock_guard<std::mutex> lock(kbsync_mutex_);
  if (kbsync_cache_entry_ == rejected_encoded) kbsync_cache_entry_.clear();
  if (!configuration_.kbsync_cache_load ||
      !configuration_.kbsync_cache_remove) {
    return;
  }
  // A concurrent request may have cached a different account or a newer
  // blob. Only remove the exact entry used by the failed request.
  if (auto stored = configuration_.kbsync_cache_load();
      stored && *stored == rejected_encoded) {
    configuration_.kbsync_cache_remove();
  }
}

Result<DownloadOffer> StoreClient::send_ent_download(
    const Account& account, const App& app, const HardwareIdentity& hardware,
    std::string_view endpoint, std::string_view external_version_id,
    const CancellationToken& token) {
  if (hardware.bytes.size() != 6) {
    return make_error(ErrorCode::kInvalidArgument,
                      "ent/download requires a six-byte device GUID");
  }
  std::uint64_t dsid = 0;
  {
    const std::string text = trim(account.directory_services_id);
    if (text.empty() || text.size() > 20) {
      return make_error(
          ErrorCode::kInvalidArgument,
          "ent/download requires a nonzero numeric account DSID");
    }
    for (const char character : text) {
      if (character < '0' || character > '9') {
        return make_error(
            ErrorCode::kInvalidArgument,
            "ent/download requires a nonzero numeric account DSID");
      }
      dsid = dsid * 10 + static_cast<std::uint64_t>(character - '0');
    }
    if (dsid == 0) {
      return make_error(
          ErrorCode::kInvalidArgument,
          "ent/download requires a nonzero numeric account DSID");
    }
  }
  const std::string guid = hardware.guid();

  Bytes serial = {0x54, 0xc8, 0xb0, 0xa9, 0x88};
  serial.insert(serial.end(), hardware.bytes.begin() + 2,
                hardware.bytes.end());
  const std::string serial_number = base64_encode(serial);

  const auto attempt = [&](const std::string& blob) -> Result<DownloadOffer> {
    PlistValue payload = PlistValue::dictionary();
    payload.set("creditDisplay", PlistValue::string(""));
    payload.set("guid", PlistValue::string(guid));
    payload.set("kbsync", PlistValue::string(blob));
    payload.set("salableAdamId",
                PlistValue::string(std::to_string(app.id)));
    payload.set("serialNumber", PlistValue::string(serial_number));
    payload.set("externalVersionId",
                PlistValue::string(std::string(external_version_id)));
    auto body = plist_body_to_bytes(payload);
    if (!body) return body.error();

    HttpHeaders headers;
    headers.set("Content-Type",
                "application/x-www-form-urlencoded; charset=utf-8");
    headers.set("iCloud-DSID", account.directory_services_id);
    headers.set("X-Dsid", account.directory_services_id);
    headers.set("User-Agent",
                "Configurator/2.18 (Macintosh; OS X 15.3.2; 24D81) "
                "AppleWebKit/0620.2.4.11.6");
    headers.set("X-Apple-Store-Front", account.storefront);
    headers.set("X-Token", account.password_token);

    HttpRequest request;
    request.method = "POST";
    request.url =
        std::string(endpoint) + "?guid=" + percent_encode(guid);
    // The X-Token header must not be forwarded to redirect destinations.
    request.follow_redirects = false;
    request.headers = headers;
    request.body = body.value();
    auto response = transport_.send(request, token);
    if (!response) return response.error();
    if (response.value().status != 200) {
      return Error{ErrorCode::kHttpStatus,
                   "ent/download returned HTTP " +
                       std::to_string(response.value().status),
                   response.value().status};
    }
    auto document = decode_plist_response(response.value());
    if (!document) return document.error();
    if (const std::string failure =
            document.value().string_or("failureType");
        !failure.empty()) {
      auto status = map_store_failure(
          failure, document.value().string_or("customerMessage"), false);
      if (!status) return status.error();
      return make_error(ErrorCode::kUnauthorized,
                        "ent/download failed with failure " + failure);
    }
    return interpret_download_response(document.value(), app,
                                       external_version_id, true);
  };

  if (const std::string cached = cached_kbsync(dsid, guid);
      !cached.empty()) {
    auto result = attempt(cached);
    if (result) return result;
    if (token.cancelled()) return result.error();
    // Retry a rejected cached blob once with a freshly generated one,
    // bypassing the cache even if its persistent entry cannot be removed.
    // Fresh blobs are never retried.
    invalidate_kbsync(dsid, guid, cached);
  }

  if (!configuration_.kbsync_generator) {
    return make_error(ErrorCode::kSignerUnavailable,
                      "kbsync generator is unavailable");
  }
  auto generated = configuration_.kbsync_generator(hardware.bytes, dsid);
  if (!generated) return generated.error();
  if (generated.value().empty()) {
    return make_error(ErrorCode::kInternal,
                      "generated download kbsync is empty");
  }
  const std::string blob = base64_encode(generated.value());
  auto result = attempt(blob);
  if (result) store_kbsync(dsid, guid, blob);
  return result;
}

Result<DownloadOffer> StoreClient::send_download_product(
    const Account& account, const App& app, const HardwareIdentity& hardware,
    std::string_view external_version_id, Platform platform,
    const CancellationToken& token) {
  const std::string guid = hardware.guid();
  std::string version(external_version_id);

  // An unpinned request can return the iOS build of a universal app.
  // Select the Mac offer before accepting any download response.
  if (version.empty() && platform == Platform::kMacOS) {
    auto latest =
        latest_external_version_id(app, account.region, platform, token);
    if (!latest) return latest.error();
    version = latest.value();
  }

  // Prefer ent/download when its generator and bag endpoint are available.
  // Keep the fetched bag for the legacy chain if this attempt cannot serve
  // the requested app; asset, network and response failures can fall back.
  std::optional<Bag> bag;
  if (configuration_.kbsync_generator) {
    if (auto fetched = fetch_bag(hardware, token); fetched) {
      bag = fetched.value();
      if (!bag->ent_download_endpoint.empty() &&
          valid_dispatch_endpoint(bag->ent_download_endpoint, "ent")) {
        const Platform ent_platform =
            platform == Platform::kUnspecified ? Platform::kIPhone
                                               : platform;
        std::string ent_version = version;
        bool resolved = true;
        if (ent_version.empty()) {
          auto latest = latest_external_version_id(app, account.region,
                                                   ent_platform, token);
          if (latest) {
            ent_version = latest.value();
          } else {
            resolved = false;
          }
        }
        if (resolved) {
          auto offer = send_ent_download(account, app, hardware,
                                         bag->ent_download_endpoint,
                                         ent_version, token);
          if (offer) return offer;
          if (token.cancelled()) return offer.error();
        }
      }
    }
  }

  const auto legacy_request = [&](std::string_view base_url,
                                  const char* version_key,
                                  std::string_view request_version) {
    const Bytes body =
        build_download_body(app.id, guid, version_key, request_version);
    return post_download_document(
        transport_,
        std::string(base_url) + "?guid=" + percent_encode(guid), body,
        download_headers(account, configuration_.user_agent), token);
  };

  auto volume = legacy_request(volume_store_url(account),
                               "externalVersionId", version);
  if (!volume) return volume.error();
  if (!is_empty_download_response(volume.value()) &&
      !is_unavailable_download_response(volume.value())) {
    return interpret_download_response(volume.value().root, app, version,
                                        false);
  }

  // Try redownload when the volume store returns no items, either silently
  // or with the message-only "No Longer Available" response.
  if (!bag) {
    auto fetched = fetch_bag(hardware, token);
    if (!fetched) return fetched.error();
    bag = fetched.value();
  }
  if (bag->redownload_endpoint.empty()) {
    return interpret_download_response(volume.value().root, app, version,
                                        false);
  }
  if (!valid_dispatch_endpoint(bag->redownload_endpoint, "redownload")) {
    return make_error(ErrorCode::kParse,
                      "invalid download endpoint in Apple bag");
  }

  if (version.empty() && (platform == Platform::kUnspecified ||
                          platform == Platform::kIPhone ||
                          platform == Platform::kIPad)) {
    // Unpinned redownloads can fail or return a tvOS package. Select the
    // current iOS build when the catalog has an offer.
    const Platform pinned_platform =
        platform == Platform::kUnspecified ? Platform::kIPhone : platform;
    auto latest = latest_external_version_id(app, account.region,
                                             pinned_platform, token);
    if (!latest) {
      if (latest.error().code == ErrorCode::kAppNotFound) {
        // A delisted app can still be available to this account. Try once
        // without a version; the caller validates the package platform.
        auto unpinned = legacy_request(bag->redownload_endpoint,
                                       "appExtVrsId", "");
        if (!unpinned) return unpinned.error();
        return interpret_download_response(unpinned.value().root, app, "",
                                            true);
      }
      return latest.error();
    }
    version = latest.value();
  }

  auto redownload =
      legacy_request(bag->redownload_endpoint, "appExtVrsId", version);
  const bool update_platform =
      platform == Platform::kUnspecified || platform == Platform::kIPhone ||
      platform == Platform::kIPad || platform == Platform::kMacOS ||
      platform == Platform::kAppleTV || platform == Platform::kWatchOS;
  const bool try_update =
      !bag->update_endpoint.empty() && !version.empty() && update_platform &&
      ((!redownload && redownload.error().http_status == 500) ||
       (redownload && is_unavailable_download_response(redownload.value())));
  if (try_update) {
    // The bag's updateProduct can serve pinned versions when redownload
    // returns an empty HTTP 500 or a message-only availability error.
    if (!valid_dispatch_endpoint(bag->update_endpoint, "update")) {
      return make_error(ErrorCode::kParse,
                        "invalid download endpoint in Apple bag");
    }
    auto update = legacy_request(bag->update_endpoint, "appExtVrsId", version);
    if (!update) return update.error();
    return interpret_download_response(update.value().root, app, version,
                                        true);
  }
  if (!redownload) return redownload.error();
  return interpret_download_response(redownload.value().root, app, version,
                                      false);
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

  std::string version(external_version_id);
  if (version.empty() && (platform == Platform::kAppleTV ||
                          platform == Platform::kWatchOS ||
                          platform == Platform::kVisionOS)) {
    auto latest =
        latest_external_version_id(app, account.region, platform, token);
    // Delisted tvOS apps may have no catalog offer but still be available
    // for redownload; the returned package platform is validated later.
    const bool missing_tv_offer =
        platform == Platform::kAppleTV &&
        latest.error().code == ErrorCode::kAppNotFound;
    if (!latest && !missing_tv_offer) return latest.error();
    if (latest) version = latest.value();
  }
  return send_download_product(account, app, hardware.value(), version,
                               platform, token);
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

Result<AppVersions> StoreClient::version_list(
    const Account& account, const App& app, Platform platform,
    const CancellationToken& token) {
  if (app.id <= 0) {
    return make_error(ErrorCode::kInvalidArgument, "app id is required");
  }
  if (platform == Platform::kUnspecified) platform = Platform::kIPhone;
  auto hardware = HardwareIdentity::from_hex(account.device_id);
  if (!hardware) return hardware.error();

  // Pin the platform's current build first so the server cannot answer
  // with another platform's version list. (send_download_product pins
  // macOS itself, but listing versions needs the pin for every platform
  // whose builds differ from the iOS one.)
  std::string version;
  if (platform == Platform::kAppleTV || platform == Platform::kWatchOS ||
      platform == Platform::kVisionOS || platform == Platform::kMacOS) {
    auto latest =
        latest_external_version_id(app, account.region, platform, token);
    if (!latest) return latest.error();
    version = latest.value();
  }

  auto offer = send_download_product(account, app, hardware.value(), version,
                                     platform, token);
  if (!offer) return offer.error();

  const PlistValue* list =
      offer.value().metadata.find("softwareVersionExternalIdentifiers");
  if (list == nullptr || !list->is_array()) {
    return make_error(
        ErrorCode::kParse,
        "failed to get version identifiers from item metadata");
  }
  AppVersions versions;
  for (const auto& entry : list->array_items()) {
    if (entry.is_integer()) {
      versions.identifiers.push_back(std::to_string(entry.integer_value()));
    } else if (entry.is_string()) {
      versions.identifiers.push_back(entry.string_value());
    }
  }
  versions.latest = offer.value().external_version_id;
  if (versions.latest.empty()) {
    return make_error(ErrorCode::kParse,
                      "failed to get latest version from item metadata");
  }
  return versions;
}

// ---------------------------------------------------------------------------
// Version metadata via partial zip
//
// The download API can return stale version and release date values, so the
// package's own Info.plist is the source of truth. Only the zip tail, the
// central directory and the Info.plist entry are fetched, with HTTP range
// requests — the package itself is never downloaded here.
// ---------------------------------------------------------------------------

namespace {

struct RemoteFile {
  HttpTransport& transport;
  std::string url;
  std::string user_agent;
  std::uint64_t size = 0;

  Result<Bytes> read(std::uint64_t offset, std::size_t count,
                     const CancellationToken& token) const {
    if (count == 0) return Bytes{};
    if (offset >= size) {
      return make_error(ErrorCode::kParse, "range read beyond package end");
    }
    const std::uint64_t end =
        std::min(size - 1, offset + static_cast<std::uint64_t>(count) - 1);
    HttpRequest request;
    request.method = "GET";
    request.url = url;
    request.headers.set("Accept-Encoding", "identity");
    request.headers.set("User-Agent", user_agent);
    request.range_start = offset;
    request.range_end = end;
    auto response = transport.send(request, token);
    if (!response) return response.error();
    if (response.value().status != 206) {
      return Error{ErrorCode::kHttpStatus,
                   "expected partial content response, got HTTP " +
                       std::to_string(response.value().status),
                   response.value().status};
    }
    if (response.value().body.size() != end - offset + 1) {
      return make_error(ErrorCode::kParse,
                        "short read from package range request");
    }
    return std::move(response.value().body);
  }
};

Result<std::uint64_t> remote_file_size(HttpTransport& transport,
                                       const std::string& url,
                                       const std::string& user_agent,
                                       const CancellationToken& token) {
  HttpRequest request;
  request.method = "GET";
  request.url = url;
  request.headers.set("Accept-Encoding", "identity");
  request.headers.set("User-Agent", user_agent);
  request.range_start = 0;
  request.range_end = 0;
  auto response = transport.send(request, token);
  if (!response) return response.error();
  if (response.value().status != 206) {
    return Error{ErrorCode::kHttpStatus,
                 "expected partial content response, got HTTP " +
                     std::to_string(response.value().status),
                 response.value().status};
  }
  const std::string range = response.value().header("Content-Range");
  const std::size_t slash = range.rfind('/');
  if (slash == std::string::npos || slash + 1 >= range.size() ||
      range[slash + 1] == '*') {
    return make_error(ErrorCode::kParse, "invalid Content-Range header");
  }
  char* end = nullptr;
  const unsigned long long size =
      std::strtoull(range.c_str() + slash + 1, &end, 10);
  if (end == nullptr || *end != '\0') {
    return make_error(ErrorCode::kParse, "invalid Content-Range size");
  }
  return static_cast<std::uint64_t>(size);
}

std::uint16_t read_le16(const std::uint8_t* bytes) {
  return static_cast<std::uint16_t>(bytes[0] | (bytes[1] << 8));
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

struct ZipEntry {
  std::string name;
  std::uint16_t method = 0;
  std::uint16_t mod_time = 0;
  std::uint16_t mod_date = 0;
  std::uint64_t compressed_size = 0;
  std::uint64_t uncompressed_size = 0;
  std::uint64_t local_offset = 0;
};

Result<std::vector<ZipEntry>> read_remote_zip_directory(
    const RemoteFile& file, const CancellationToken& token) {
  if (file.size < 22) {
    return make_error(ErrorCode::kParse, "package is too small to be a zip");
  }
  const std::size_t tail_size =
      static_cast<std::size_t>(std::min<std::uint64_t>(file.size, 65557));
  auto tail = file.read(file.size - tail_size, tail_size, token);
  if (!tail) return tail.error();

  // Locate the end of central directory record (scanning backwards so a
  // signature inside the trailing comment cannot win).
  std::size_t eocd_offset = 0;
  bool found = false;
  for (std::size_t index = tail.value().size() - 22;; --index) {
    const std::uint8_t* at = tail.value().data() + index;
    if (at[0] == 0x50 && at[1] == 0x4b && at[2] == 0x05 && at[3] == 0x06 &&
        index + 22 + read_le16(at + 20) == tail.value().size()) {
      eocd_offset = index;
      found = true;
      break;
    }
    if (index == 0) break;
  }
  if (!found) {
    return make_error(ErrorCode::kParse,
                      "package has no zip end of central directory");
  }
  const std::uint8_t* eocd = tail.value().data() + eocd_offset;
  std::uint64_t entries = read_le16(eocd + 10);
  std::uint64_t directory_size = read_le32(eocd + 12);
  std::uint64_t directory_offset = read_le32(eocd + 16);

  if (entries == 0xffff || directory_size == 0xffffffffu ||
      directory_offset == 0xffffffffu) {
    // Zip64: the locator sits immediately before the classic record.
    if (eocd_offset < 20) {
      return make_error(ErrorCode::kParse, "zip64 locator is missing");
    }
    const std::uint8_t* locator = eocd - 20;
    if (locator[0] != 0x50 || locator[1] != 0x4b || locator[2] != 0x06 ||
        locator[3] != 0x07) {
      return make_error(ErrorCode::kParse, "zip64 locator is invalid");
    }
    const std::uint64_t zip64_offset = read_le64(locator + 8);
    auto zip64 = file.read(zip64_offset, 56, token);
    if (!zip64) return zip64.error();
    const std::uint8_t* record = zip64.value().data();
    if (zip64.value().size() < 56 || record[0] != 0x50 || record[1] != 0x4b ||
        record[2] != 0x06 || record[3] != 0x06) {
      return make_error(ErrorCode::kParse, "zip64 directory is invalid");
    }
    entries = read_le64(record + 32);
    directory_size = read_le64(record + 40);
    directory_offset = read_le64(record + 48);
  }

  if (entries > 200000 || directory_size > 64u * 1024 * 1024 ||
      directory_offset + directory_size > file.size) {
    return make_error(ErrorCode::kParse, "zip directory is implausible");
  }
  auto directory = directory_size == 0
                       ? Result<Bytes>(Bytes{})
                       : file.read(directory_offset,
                                   static_cast<std::size_t>(directory_size),
                                   token);
  if (!directory) return directory.error();

  std::vector<ZipEntry> result;
  std::size_t offset = 0;
  for (std::uint64_t index = 0; index < entries; ++index) {
    if (offset + 46 > directory.value().size()) {
      return make_error(ErrorCode::kParse, "zip directory is truncated");
    }
    const std::uint8_t* at = directory.value().data() + offset;
    if (at[0] != 0x50 || at[1] != 0x4b || at[2] != 0x01 || at[3] != 0x02) {
      return make_error(ErrorCode::kParse, "zip directory entry is invalid");
    }
    ZipEntry entry;
    entry.method = read_le16(at + 10);
    entry.mod_time = read_le16(at + 12);
    entry.mod_date = read_le16(at + 14);
    entry.compressed_size = read_le32(at + 20);
    entry.uncompressed_size = read_le32(at + 24);
    const std::size_t name_size = read_le16(at + 28);
    const std::size_t extra_size = read_le16(at + 30);
    const std::size_t comment_size = read_le16(at + 32);
    entry.local_offset = read_le32(at + 42);
    if (offset + 46 + name_size + extra_size + comment_size >
        directory.value().size()) {
      return make_error(ErrorCode::kParse, "zip directory is truncated");
    }
    entry.name.assign(reinterpret_cast<const char*>(at + 46), name_size);
    // Zip64 extra field overrides the 0xffffffff placeholders in order:
    // uncompressed size, compressed size, local offset, disk.
    const std::uint8_t* extra = at + 46 + name_size;
    std::size_t extra_read = 0;
    while (extra_read + 4 <= extra_size) {
      const std::uint16_t field = read_le16(extra + extra_read);
      const std::size_t field_size = read_le16(extra + extra_read + 2);
      extra_read += 4;
      if (extra_read + field_size > extra_size) break;
      if (field == 0x0001) {
        const std::uint8_t* value = extra + extra_read;
        std::size_t value_read = 0;
        const auto take64 = [&](std::uint64_t& target) {
          if (value_read + 8 <= field_size) {
            target = read_le64(value + value_read);
            value_read += 8;
          }
        };
        if (entry.uncompressed_size == 0xffffffffu) {
          take64(entry.uncompressed_size);
        }
        if (entry.compressed_size == 0xffffffffu) {
          take64(entry.compressed_size);
        }
        if (entry.local_offset == 0xffffffffu) {
          take64(entry.local_offset);
        }
      }
      extra_read += field_size;
    }
    result.push_back(std::move(entry));
    offset += 46 + name_size + extra_size + comment_size;
  }
  return result;
}

Result<Bytes> read_remote_zip_entry(const RemoteFile& file,
                                    const ZipEntry& entry,
                                    const CancellationToken& token) {
  auto header = file.read(entry.local_offset, 30, token);
  if (!header) return header.error();
  const std::uint8_t* at = header.value().data();
  if (header.value().size() < 30 || at[0] != 0x50 || at[1] != 0x4b ||
      at[2] != 0x03 || at[3] != 0x04) {
    return make_error(ErrorCode::kParse, "zip local header is invalid");
  }
  const std::uint64_t data_offset = entry.local_offset + 30 +
                                    read_le16(at + 26) + read_le16(at + 28);
  if (entry.uncompressed_size > 4u * 1024 * 1024 ||
      entry.compressed_size > 64u * 1024 * 1024) {
    return make_error(ErrorCode::kParse, "zip entry is implausibly large");
  }
  auto compressed =
      file.read(data_offset, static_cast<std::size_t>(entry.compressed_size),
                token);
  if (!compressed) return compressed.error();
  if (entry.method == 0) return compressed;
  if (entry.method != 8) {
    return make_error(ErrorCode::kUnsupported,
                      "unsupported zip compression method");
  }
  Bytes output(static_cast<std::size_t>(entry.uncompressed_size));
  mz_stream stream{};
  stream.next_in = compressed.value().data();
  stream.avail_in =
      static_cast<unsigned int>(compressed.value().size());
  stream.next_out = output.data();
  stream.avail_out = static_cast<unsigned int>(output.size());
  if (mz_inflateInit2(&stream, -MZ_DEFAULT_WINDOW_BITS) != MZ_OK) {
    return make_error(ErrorCode::kInternal, "cannot initialise inflate");
  }
  const int status = mz_inflate(&stream, MZ_FINISH);
  const std::size_t produced = stream.total_out;
  mz_inflateEnd(&stream);
  if (status != MZ_STREAM_END && status != MZ_OK) {
    return make_error(ErrorCode::kParse, "cannot inflate zip entry");
  }
  output.resize(produced);
  return output;
}

bool is_main_info_plist_path(std::string_view name) {
  std::vector<std::string_view> parts;
  std::size_t start = 0;
  while (start <= name.size()) {
    const std::size_t slash = name.find('/', start);
    if (slash == std::string_view::npos) {
      parts.push_back(name.substr(start));
      break;
    }
    parts.push_back(name.substr(start, slash - start));
    start = slash + 1;
  }
  return parts.size() == 3 && parts[0] == "Payload" &&
         ends_with(parts[1], ".app") && parts[2] == "Info.plist";
}

std::int64_t dos_datetime_to_unix(std::uint16_t date, std::uint16_t time) {
  const unsigned year = ((date >> 9) & 0x7f) + 1980;
  const unsigned month = (date >> 5) & 0x0f;
  const unsigned day = date & 0x1f;
  const unsigned hour = (time >> 11) & 0x1f;
  const unsigned minute = (time >> 5) & 0x3f;
  const unsigned second = (time & 0x1f) * 2;
  if (month < 1 || month > 12 || day < 1 || day > 31) return 0;
  return days_from_civil(year, month, day) * 86400 + hour * 3600 +
         minute * 60 + second;
}

bool parse_release_date_string(std::string_view text, std::int64_t& epoch) {
  const std::string value = trim(text);
  const auto number = [&](std::size_t offset, std::size_t count,
                          int& output) {
    if (offset + count > value.size()) return false;
    output = 0;
    for (std::size_t index = 0; index < count; ++index) {
      const char character = value[offset + index];
      if (character < '0' || character > '9') return false;
      output = output * 10 + (character - '0');
    }
    return true;
  };
  int year = 0, month = 0, day = 0;
  // RFC 3339 (also the binary plist date rendering used by this project).
  if (value.size() >= 20 && value[4] == '-' && value[7] == '-' &&
      (value[10] == 'T' || value[10] == ' ') && value[13] == ':' &&
      value[16] == ':') {
    int hour = 0, minute = 0, second = 0;
    if (!number(0, 4, year) || !number(5, 2, month) || !number(8, 2, day) ||
        !number(11, 2, hour) || !number(14, 2, minute) ||
        !number(17, 2, second)) {
      return false;
    }
    std::int64_t offset_seconds = 0;
    std::size_t cursor = 19;
    if (cursor < value.size() && value[cursor] == '.') {
      while (++cursor < value.size() && value[cursor] >= '0' &&
             value[cursor] <= '9') {
      }
    }
    if (cursor < value.size() && value[cursor] != 'Z' &&
        value[cursor] != 'z') {
      if (value[cursor] != '+' && value[cursor] != '-') return false;
      const int sign = value[cursor] == '-' ? -1 : 1;
      int offset_hour = 0, offset_minute = 0;
      if (!number(cursor + 1, 2, offset_hour)) return false;
      std::size_t minute_offset = cursor + 3;
      if (minute_offset < value.size() && value[minute_offset] == ':') {
        ++minute_offset;
      }
      if (!number(minute_offset, 2, offset_minute)) return false;
      offset_seconds =
          sign * (offset_hour * 3600 + offset_minute * 60);
    }
    epoch = days_from_civil(year, month, day) * 86400 + hour * 3600 +
            minute * 60 + second - offset_seconds;
    return true;
  }
  // Date only: "2006-01-02".
  if (value.size() == 10 && value[4] == '-' && value[7] == '-' &&
      number(0, 4, year) && number(5, 2, month) && number(8, 2, day)) {
    epoch = days_from_civil(year, month, day) * 86400;
    return true;
  }
  // Long form: "Monday, January 2, 2006".
  static constexpr const char* kMonthNames[12] = {
      "January", "February", "March",     "April",   "May",      "June",
      "July",    "August",   "September", "October", "November", "December"};
  const std::size_t comma = value.find(", ");
  if (comma != std::string::npos) {
    const std::string_view rest = std::string_view(value).substr(comma + 2);
    for (int index = 0; index < 12; ++index) {
      const std::string_view month_name = kMonthNames[index];
      if (!starts_with(rest, month_name)) continue;
      std::string_view tail = rest.substr(month_name.size());
      if (!starts_with(tail, " ")) return false;
      tail.remove_prefix(1);
      const std::size_t second_comma = tail.find(", ");
      if (second_comma == std::string_view::npos) return false;
      const std::string day_text = trim(tail.substr(0, second_comma));
      const std::string year_text = trim(tail.substr(second_comma + 2));
      char* end = nullptr;
      day = static_cast<int>(std::strtol(day_text.c_str(), &end, 10));
      if (end == nullptr || *end != '\0') return false;
      year = static_cast<int>(std::strtol(year_text.c_str(), &end, 10));
      if (end == nullptr || *end != '\0') return false;
      epoch = days_from_civil(year, index + 1, day) * 86400;
      return true;
    }
  }
  return false;
}

}  // namespace

Result<AppVersionMetadata> StoreClient::version_metadata(
    const Account& account, const App& app,
    std::string_view external_version_id, Platform platform,
    const CancellationToken& token) {
  if (platform == Platform::kUnspecified) platform = Platform::kIPhone;
  if (platform == Platform::kMacOS) {
    // Native Mac packages are installer archives, not IPAs; reading their
    // metadata requires the Mac package pipeline, which this client does
    // not implement (the iOS app cannot install them either).
    return make_error(ErrorCode::kUnsupported,
                      "macOS package version metadata is not supported");
  }
  auto offer =
      download_offer(account, app, external_version_id, platform, token);
  if (!offer) return offer.error();
  if (offer.value().url.empty()) {
    return make_error(ErrorCode::kParse, "download offer has no package URL");
  }

  // Do not fall back to the item metadata: the download API can return
  // stale version and release date values, so the IPA's Info.plist is the
  // source of truth and failures stay visible.
  auto size = remote_file_size(transport_, offer.value().url,
                               configuration_.user_agent, token);
  if (!size) return size.error();
  RemoteFile file{transport_, offer.value().url, configuration_.user_agent,
                  size.value()};
  auto entries = read_remote_zip_directory(file, token);
  if (!entries) return entries.error();
  const ZipEntry* info_entry = nullptr;
  for (const ZipEntry& entry : entries.value()) {
    if (is_main_info_plist_path(entry.name)) {
      info_entry = &entry;
      break;
    }
  }
  if (info_entry == nullptr) {
    return make_error(ErrorCode::kParse,
                      "could not find Info.plist in the package");
  }
  auto info_bytes = read_remote_zip_entry(file, *info_entry, token);
  if (!info_bytes) return info_bytes.error();
  auto info = PlistValue::parse(info_bytes.value());
  if (!info) return info.error();
  if (!info.value().is_dictionary()) {
    return make_error(ErrorCode::kParse, "Info.plist is not a dictionary");
  }

  AppVersionMetadata metadata;
  for (const char* key :
       {"CFBundleShortVersionString", "bundleShortVersionString"}) {
    if (auto value = info.value().string_for(key); value && !value->empty()) {
      metadata.display_version = trim(*value);
      break;
    }
  }
  if (metadata.display_version.empty()) {
    return make_error(ErrorCode::kParse,
                      "Info.plist does not contain a display version");
  }

  bool have_date = false;
  for (const char* key : {"releaseDate", "ReleaseDate"}) {
    const PlistValue* value = info.value().find(key);
    if (value == nullptr) continue;
    if (value->is_string() || value->type() == PlistValue::Type::kDate) {
      if (!parse_release_date_string(value->string_value(),
                                     metadata.release_date_unix)) {
        return make_error(ErrorCode::kParse,
                          "failed to parse Info.plist release date");
      }
      have_date = true;
    } else if (value->is_integer()) {
      metadata.release_date_unix = value->integer_value();
      have_date = true;
    } else if (value->is_real()) {
      metadata.release_date_unix =
          static_cast<std::int64_t>(value->real_value());
      have_date = true;
    } else {
      return make_error(ErrorCode::kParse,
                        "unsupported Info.plist release date type");
    }
    break;
  }
  if (!have_date) {
    metadata.release_date_unix =
        dos_datetime_to_unix(info_entry->mod_date, info_entry->mod_time);
    if (metadata.release_date_unix == 0) {
      return make_error(ErrorCode::kParse,
                        "Info.plist does not contain a release date");
    }
  }
  return metadata;
}

}  // namespace ipashelf
