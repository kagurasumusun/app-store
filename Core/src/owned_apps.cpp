// SPDX-License-Identifier: MIT
//
// Purchase history (`owned-apps`) over Apple's DAAP endpoint. The flow is a
// session login, a revision update and an items query against
// MZPurchaseDaap; the update and items requests are SAP-signed like the
// Store sign-in.

#include <algorithm>
#include <chrono>
#include <limits>
#include <map>

#include "ipashelf/crypto.hpp"
#include "ipashelf/store_client.hpp"

namespace ipashelf {
namespace {

constexpr std::string_view kDaapBaseUrl =
    "https://pd.itunes.apple.com/WebObjects/MZPurchaseDaap.woa/purchase";
constexpr int kDefaultOwnedAppsLimit = 10;
constexpr int kMaxOwnedAppsLimit = 100;
constexpr std::uint64_t kMediaKindApps = 131072;
constexpr std::uint64_t kMediaKindArcade = 262144;
constexpr std::uint64_t kMediaKindMac = 67108864;

// ---------------------------------------------------------------------------
// Calendar formatting for the DAAP time headers.
// ---------------------------------------------------------------------------

struct CivilDate {
  int year;
  unsigned month;
  unsigned day;
};

// Inverse of days_from_civil (Howard Hinnant's civil calendar algorithm).
CivilDate civil_from_days(std::int64_t days) {
  days += 719468;
  const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const unsigned day_of_era = static_cast<unsigned>(days - era * 146097);
  const unsigned year_of_era =
      (day_of_era - day_of_era / 1460 + day_of_era / 36524 -
       day_of_era / 146096) /
      365;
  const std::int64_t year = static_cast<std::int64_t>(year_of_era) + era * 400;
  const unsigned day_of_year =
      day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
  const unsigned month_prime = (5 * day_of_year + 2) / 153;
  const unsigned day = day_of_year - (153 * month_prime + 2) / 5 + 1;
  const unsigned month = month_prime < 10 ? month_prime + 3 : month_prime - 9;
  return {static_cast<int>(year + (month <= 2 ? 1 : 0)), month, day};
}

std::string pad2(int value) {
  return (value < 10 ? "0" : "") + std::to_string(value);
}

std::string format_http_date(std::int64_t epoch) {
  static constexpr const char* kWeekdays[7] = {"Mon", "Tue", "Wed", "Thu",
                                               "Fri", "Sat", "Sun"};
  static constexpr const char* kMonths[12] = {
      "Jan", "Feb", "Mar", "Apr", "May", "Jun",
      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  const std::int64_t days = epoch >= 0 ? epoch / 86400 : -(-epoch + 86399) / 86400;
  const std::int64_t seconds = epoch - days * 86400;
  const CivilDate date = civil_from_days(days);
  // 1970-01-01 was a Thursday; index with Monday = 0.
  const int weekday = static_cast<int>(((days % 7) + 7 + 3) % 7);
  return std::string(kWeekdays[weekday]) + ", " + pad2(date.day) + " " +
         kMonths[date.month - 1] + " " + std::to_string(date.year) + " " +
         pad2(static_cast<int>(seconds / 3600)) + ":" +
         pad2(static_cast<int>((seconds / 60) % 60)) + ":" +
         pad2(static_cast<int>(seconds % 60)) + " GMT";
}

std::string format_client_time(std::int64_t epoch) {
  const std::int64_t days = epoch >= 0 ? epoch / 86400 : -(-epoch + 86399) / 86400;
  const std::int64_t seconds = epoch - days * 86400;
  const CivilDate date = civil_from_days(days);
  return std::to_string(date.year) + "-" + pad2(date.month) + "-" +
         pad2(date.day) + "T" + pad2(static_cast<int>(seconds / 3600)) +
         ":" + pad2(static_cast<int>((seconds / 60) % 60)) + ":" +
         pad2(static_cast<int>(seconds % 60)) + "Z";
}

// ---------------------------------------------------------------------------
// DMAP (tag + big-endian length + payload) encoding and walking.
// ---------------------------------------------------------------------------

void append_be32(Bytes& output, std::uint32_t value) {
  output.push_back(static_cast<std::uint8_t>((value >> 24) & 0xff));
  output.push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
  output.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
  output.push_back(static_cast<std::uint8_t>(value & 0xff));
}

Bytes dmap_tag(std::string_view name, const Bytes& payload) {
  Bytes result;
  result.reserve(8 + payload.size());
  result.insert(result.end(), name.begin(), name.end());
  append_be32(result, static_cast<std::uint32_t>(payload.size()));
  result.insert(result.end(), payload.begin(), payload.end());
  return result;
}

Bytes dmap_uint32(std::string_view name, std::uint32_t value) {
  Bytes payload;
  append_be32(payload, value);
  return dmap_tag(name, payload);
}

Bytes dmap_uint8(std::string_view name, std::uint8_t value) {
  return dmap_tag(name, Bytes{value});
}

Bytes dmap_string(std::string_view name, std::string_view value) {
  return dmap_tag(name, Bytes(value.begin(), value.end()));
}

std::uint32_t read_be32(const std::uint8_t* bytes) {
  return (static_cast<std::uint32_t>(bytes[0]) << 24) |
         (static_cast<std::uint32_t>(bytes[1]) << 16) |
         (static_cast<std::uint32_t>(bytes[2]) << 8) |
         static_cast<std::uint32_t>(bytes[3]);
}

bool valid_dmap_tag(const std::uint8_t* tag) {
  for (int index = 0; index < 4; ++index) {
    if (tag[index] < 0x20 || tag[index] > 0x7e) return false;
  }
  return true;
}

bool is_dmap_container(std::string_view tag) {
  static constexpr std::string_view kContainers[] = {
      "adbs", "adsr", "aply", "avdb", "mbcl", "mccr", "mcty",
      "mdcl", "mlcl", "mlit", "mlog", "msrv", "mupd"};
  for (const std::string_view container : kContainers) {
    if (tag == container) return true;
  }
  return false;
}

using DmapVisitor =
    std::function<Status(std::string_view tag, const Bytes& payload)>;

Status walk_dmap(const Bytes& data, int depth, const DmapVisitor& visit) {
  if (depth > 16) {
    return fail(ErrorCode::kParse, "DMAP nesting is too deep");
  }
  std::size_t offset = 0;
  while (offset < data.size()) {
    if (data.size() - offset < 8) {
      return fail(ErrorCode::kParse, "truncated DMAP tag header at byte " +
                                         std::to_string(offset));
    }
    const std::uint8_t* tag_bytes = data.data() + offset;
    if (!valid_dmap_tag(tag_bytes)) {
      return fail(ErrorCode::kParse,
                  "invalid DMAP tag at byte " + std::to_string(offset));
    }
    const std::uint32_t length = read_be32(tag_bytes + 4);
    const std::size_t remaining = data.size() - offset - 8;
    if (length > remaining) {
      return fail(ErrorCode::kParse, "DMAP tag length exceeds the response");
    }
    const std::string tag(reinterpret_cast<const char*>(tag_bytes), 4);
    const Bytes payload(data.begin() + static_cast<std::ptrdiff_t>(offset + 8),
                        data.begin() +
                            static_cast<std::ptrdiff_t>(offset + 8 + length));
    if (auto status = visit(tag, payload); !status) return status;
    if (is_dmap_container(tag)) {
      if (auto status = walk_dmap(payload, depth + 1, visit); !status) {
        return status;
      }
    }
    offset += 8 + length;
  }
  return ok_status();
}

Result<std::optional<std::uint64_t>> first_dmap_uint(const Bytes& data,
                                                     std::string_view target) {
  std::optional<std::uint64_t> result;
  auto status = walk_dmap(
      data, 0, [&](std::string_view tag, const Bytes& payload) -> Status {
        if (result || tag != target) return ok_status();
        if (payload.size() == 4) {
          result = read_be32(payload.data());
          return ok_status();
        }
        if (payload.size() == 8) {
          result = (static_cast<std::uint64_t>(read_be32(payload.data()))
                    << 32) |
                   read_be32(payload.data() + 4);
          return ok_status();
        }
        return fail(ErrorCode::kParse, "tag " + std::string(target) +
                                           " has an invalid integer length");
      });
  if (!status) return status.error();
  return result;
}

Result<std::uint64_t> dmap_integer(std::string_view tag, const Bytes& payload,
                                   bool allow_small) {
  if (allow_small && payload.size() == 1) return payload[0];
  if (allow_small && payload.size() == 2) {
    return static_cast<std::uint64_t>(
        (static_cast<std::uint32_t>(payload[0]) << 8) | payload[1]);
  }
  if (payload.size() == 4) return read_be32(payload.data());
  if (payload.size() == 8) {
    return (static_cast<std::uint64_t>(read_be32(payload.data())) << 32) |
           read_be32(payload.data() + 4);
  }
  return make_error(ErrorCode::kParse, "owned app " + std::string(tag) +
                                           " has an invalid integer length");
}

Result<App> parse_owned_app(const Bytes& data) {
  App app;
  std::uint64_t media_kind = 0;
  std::uint64_t supported_products = 0;
  auto status = walk_dmap(
      data, 0, [&](std::string_view tag, const Bytes& payload) -> Status {
        if (tag == "aeMk" || tag == "aeSS") {
          auto value = dmap_integer(tag, payload, true);
          if (!value) return value.error();
          if (tag == "aeMk") {
            media_kind = value.value();
          } else {
            supported_products = value.value();
          }
        } else if (tag == "aeSI") {
          auto value = dmap_integer(tag, payload, false);
          if (!value) return value.error();
          if (value.value() >
              static_cast<std::uint64_t>(
                  std::numeric_limits<std::int64_t>::max())) {
            return fail(ErrorCode::kParse, "owned app ID exceeds int64");
          }
          app.id = static_cast<std::int64_t>(value.value());
        } else if (tag == "aeBI") {
          app.bundle_id.assign(payload.begin(), payload.end());
        } else if (tag == "aeLN") {
          app.name.assign(payload.begin(), payload.end());
        } else if (tag == "minm") {
          if (app.name.empty()) app.name.assign(payload.begin(), payload.end());
        } else if (tag == "aePd") {
          app.version.assign(payload.begin(), payload.end());
        } else if (tag == "asdp") {
          if (payload.size() != 4) {
            return fail(ErrorCode::kParse,
                        "owned app purchase date has an invalid length");
          }
          app.purchase_date_unix = read_be32(payload.data());
        }
        return ok_status();
      });
  if (!status) return status.error();

  // Mac software uses a separate media kind; its supported-products bits
  // do not describe mobile device support.
  if (media_kind == kMediaKindMac) {
    app.platforms.push_back(Platform::kMacOS);
  } else if (media_kind == kMediaKindApps ||
             media_kind == kMediaKindArcade) {
    // The purchase-history mask uses 8 for Mac and 16 for visionOS. It
    // does not encode Apple TV support.
    static constexpr std::pair<std::uint64_t, Platform> kProducts[] = {
        {1, Platform::kIPhone},
        {2, Platform::kIPad},
        {16, Platform::kVisionOS},
        {8, Platform::kMacOS}};
    for (const auto& product : kProducts) {
      if ((supported_products & product.first) != 0) {
        app.platforms.push_back(product.second);
      }
    }
  }
  if (app.platforms.empty()) {
    app.platforms.push_back(Platform::kUnspecified);
  }
  return app;
}

std::vector<App> merge_owned_apps(const std::vector<App>& apps) {
  std::vector<App> merged;
  std::map<std::int64_t, std::size_t> seen;
  for (const App& app : apps) {
    auto found = seen.find(app.id);
    if (found != seen.end()) {
      App& existing = merged[found->second];
      for (const Platform platform : app.platforms) {
        if (std::find(existing.platforms.begin(), existing.platforms.end(),
                      platform) == existing.platforms.end()) {
          existing.platforms.push_back(platform);
        }
      }
      if (app.purchase_date_unix > existing.purchase_date_unix) {
        existing.purchase_date_unix = app.purchase_date_unix;
      }
      continue;
    }
    seen.emplace(app.id, merged.size());
    merged.push_back(app);
  }
  for (App& app : merged) {
    if (app.platforms.size() > 1) {
      app.platforms.erase(
          std::remove(app.platforms.begin(), app.platforms.end(),
                      Platform::kUnspecified),
          app.platforms.end());
    }
  }
  return merged;
}

Result<std::vector<App>> parse_owned_apps(const Bytes& data) {
  std::vector<App> apps;
  auto status = walk_dmap(
      data, 0, [&](std::string_view tag, const Bytes& payload) -> Status {
        if (tag != "mlit") return ok_status();
        auto app = parse_owned_app(payload);
        if (!app) return app.error();
        if (app.value().id != 0) apps.push_back(std::move(app).value());
        return ok_status();
      });
  if (!status) return status.error();
  return merge_owned_apps(apps);
}

Status owned_response_error(std::string_view label, int http_status) {
  if (http_status == 401 || http_status == 403) {
    return Error{ErrorCode::kTokenExpired,
                 std::string(label) + " request returned HTTP " +
                     std::to_string(http_status),
                 http_status};
  }
  if (http_status != 200) {
    return Error{ErrorCode::kHttpStatus,
                 std::string(label) + " request returned HTTP " +
                     std::to_string(http_status),
                 http_status};
  }
  return ok_status();
}

Status owned_dmap_status_error(std::string_view label, const Bytes& data) {
  auto status = first_dmap_uint(data, "mstt");
  if (!status) return status.error();
  if (!status.value()) return ok_status();
  const std::uint64_t value = *status.value();
  if (value == 401 || value == 403) {
    return Error{ErrorCode::kTokenExpired,
                 std::string(label) + " response returned DAAP status " +
                     std::to_string(value),
                 static_cast<int>(value)};
  }
  if (value != 200) {
    return fail(ErrorCode::kParse, std::string(label) +
                                       " response returned DAAP status " +
                                       std::to_string(value));
  }
  return ok_status();
}

}  // namespace

Result<OwnedAppsPage> StoreClient::owned_apps(const Account& account,
                                              std::optional<Platform> platform,
                                              int page, int limit,
                                              const CancellationToken& token) {
  if (platform && *platform != Platform::kIPhone &&
      *platform != Platform::kIPad && *platform != Platform::kAppleTV &&
      *platform != Platform::kVisionOS && *platform != Platform::kMacOS &&
      *platform != Platform::kUnspecified) {
    return make_error(ErrorCode::kInvalidArgument,
                      "unsupported platform filter for purchase history");
  }
  if (page == 0) page = 1;
  if (limit == 0) limit = kDefaultOwnedAppsLimit;
  if (page < 1) {
    return make_error(ErrorCode::kInvalidArgument,
                      "page must be greater than 0");
  }
  if (limit < 1) {
    return make_error(ErrorCode::kInvalidArgument,
                      "limit must be greater than 0");
  }
  if (limit > kMaxOwnedAppsLimit) {
    return make_error(
        ErrorCode::kInvalidArgument,
        "limit must not exceed " + std::to_string(kMaxOwnedAppsLimit));
  }
  if (account.password_token.empty() ||
      account.directory_services_id.empty()) {
    return make_error(ErrorCode::kUnauthorized, "account is not signed in");
  }
  auto hardware = HardwareIdentity::from_hex(account.device_id);
  if (!hardware) return hardware.error();
  const std::string guid = hardware.value().guid();

  auto established = open_sap_session(hardware.value(), token, nullptr);
  if (!established) return established.error();
  std::unique_ptr<SapSession> session = std::move(established).value();
  SapSigner* signer = session.get();

  const auto now_seconds = [&]() {
    if (configuration_.now_unix) return configuration_.now_unix();
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
  };

  const auto headers_for = [&](const std::string& storefront,
                               std::int64_t now) {
    HttpHeaders headers;
    headers.set("Accept", "*/*");
    headers.set("Accept-Language", "en-us");
    headers.set("Client-Cloud-DAAP-Request-Reason", "5");
    headers.set("Client-Cloud-Purchase-Daap-Version", "1.1/Configurator-2.0");
    headers.set("Client-DAAP-Version", "3.12");
    headers.set("Date", format_http_date(now));
    headers.set("iCloud-DSID", account.directory_services_id);
    headers.set("X-Apple-I-Client-Time", format_client_time(now));
    headers.set("X-Apple-I-Locale", "en_US");
    headers.set("X-Apple-I-TimeZone", configuration_.time_zone_name);
    headers.set("X-Apple-Store-Front", storefront);
    headers.set("X-Apple-TZ",
                std::to_string(configuration_.utc_offset_seconds / 60));
    headers.set("X-Dsid", account.directory_services_id);
    headers.set("X-Guid", guid);
    headers.set("X-Token", account.password_token);
    headers.set("User-Agent", configuration_.user_agent);
    return headers;
  };

  const std::string query =
      "('com.apple.itunes.extended\\-media\\-kind:" +
      std::to_string(kMediaKindApps) +
      "','com.apple.itunes.extended\\-media\\-kind:" +
      std::to_string(kMediaKindArcade) +
      "','com.apple.itunes.extended\\-media\\-kind:" +
      std::to_string(kMediaKindMac) + "')";

  const auto send_daap = [&](const std::string& url, const Bytes& body,
                             HttpHeaders headers, bool sign,
                             std::string_view label) -> Result<Bytes> {
    HttpRequest request;
    request.method = "POST";
    request.url = url;
    request.follow_redirects = false;
    request.headers = std::move(headers);
    request.body = body;
    if (sign) {
      auto signature = signer->sign(body);
      if (!signature) return signature.error();
      request.headers.set("X-Apple-ActionSignature",
                          base64_encode(signature.value()));
    }
    auto response = transport_.send(request, token);
    if (!response) return response.error();
    if (auto status =
            owned_response_error(label, response.value().status);
        !status) {
      return status.error();
    }
    if (auto status = owned_dmap_status_error(label, response.value().body);
        !status) {
      return status.error();
    }
    return std::move(response.value().body);
  };

  const auto fetch_for_storefront = [&](const std::string& storefront)
      -> Result<std::vector<App>> {
    const std::int64_t login_now = now_seconds();
    auto login = send_daap(std::string(kDaapBaseUrl) + "/login", Bytes{},
                           headers_for(storefront, login_now), false,
                           "purchase history login");
    if (!login) return login.error();
    auto session_id = first_dmap_uint(login.value(), "mlid");
    if (!session_id) return session_id.error();
    if (!session_id.value() || *session_id.value() > 0xffffffffu) {
      return make_error(
          ErrorCode::kParse,
          "purchase history login response did not contain a valid "
          "session ID");
    }
    const auto session32 =
        static_cast<std::uint32_t>(*session_id.value());

    const std::string update_body =
        "session-id=" + std::to_string(session32) +
        "&revision-number=(null)&query=" + query;
    HttpHeaders update_headers = headers_for(storefront, now_seconds());
    update_headers.set("Content-Type", "application/x-www-form-urlencoded");
    auto update = send_daap(std::string(kDaapBaseUrl) + "/update",
                            Bytes(update_body.begin(), update_body.end()),
                            std::move(update_headers), true,
                            "purchase history update");
    if (!update) return update.error();
    auto revision = first_dmap_uint(update.value(), "musr");
    if (!revision) return revision.error();
    if (!revision.value() || *revision.value() > 0xffffffffu) {
      return make_error(
          ErrorCode::kParse,
          "purchase history update response did not contain a valid "
          "revision");
    }
    const auto revision32 = static_cast<std::uint32_t>(*revision.value());

    const std::int64_t items_now = now_seconds();
    Bytes payload;
    const auto append = [&](Bytes part) {
      payload.insert(payload.end(), part.begin(), part.end());
    };
    append(dmap_uint32("mstc", static_cast<std::uint32_t>(items_now)));
    append(dmap_uint32("mlid", session32));
    append(dmap_uint8("mikd", 2));
    append(dmap_uint32("musr", revision32));
    append(dmap_uint32("mder", 0));
    append(dmap_string("mque", query));
    append(dmap_tag("aetl", Bytes{}));
    const Bytes items_body = dmap_tag("adsr", payload);

    HttpHeaders items_headers = headers_for(storefront, items_now);
    items_headers.set("Content-Type", "application/x-dmap-tagged");
    auto items = send_daap(std::string(kDaapBaseUrl) + "/databases/" +
                               std::to_string(revision32) + "/items",
                           items_body, std::move(items_headers), true,
                           "purchase history items");
    if (!items) return items.error();
    return parse_owned_apps(items.value());
  };

  std::string storefront_base = account.storefront;
  if (const std::size_t comma = storefront_base.find(',');
      comma != std::string::npos) {
    storefront_base = storefront_base.substr(0, comma);
  }

  std::vector<App> all;
  Result<OwnedAppsPage> result =
      make_error(ErrorCode::kInternal, "purchase history did not run");
  // Configurator (34) lists mobile apps, while the Mac App Store (13) also
  // lists Mac software and mobile apps available on Apple silicon.
  for (const char* suffix : {"34", "13"}) {
    auto fetched = fetch_for_storefront(storefront_base + "," + suffix);
    if (!fetched) {
      result = fetched.error();
      all.clear();
      break;
    }
    all.insert(all.end(), fetched.value().begin(), fetched.value().end());
    result = OwnedAppsPage{};
  }

  if (result) {
    std::vector<App> apps = merge_owned_apps(all);
    if (platform && *platform != Platform::kUnspecified) {
      apps.erase(
          std::remove_if(apps.begin(), apps.end(),
                         [&](const App& app) {
                           return std::find(app.platforms.begin(),
                                            app.platforms.end(),
                                            *platform) ==
                                  app.platforms.end();
                         }),
          apps.end());
    }
    std::stable_sort(apps.begin(), apps.end(),
                     [](const App& left, const App& right) {
                       if (left.purchase_date_unix == 0) return false;
                       if (right.purchase_date_unix == 0) return true;
                       return left.purchase_date_unix >
                              right.purchase_date_unix;
                     });
    OwnedAppsPage output;
    output.total = static_cast<int>(apps.size());
    output.page = page;
    output.limit = limit;
    const std::size_t start =
        static_cast<std::size_t>(page - 1) * static_cast<std::size_t>(limit);
    if (start < apps.size()) {
      const std::size_t end =
          std::min(apps.size(), start + static_cast<std::size_t>(limit));
      output.apps.assign(apps.begin() + static_cast<std::ptrdiff_t>(start),
                         apps.begin() + static_cast<std::ptrdiff_t>(end));
    }
    result = std::move(output);
  }

  auto close_status = session->close();
  if (!close_status && result) return close_status.error();
  return result;
}

}  // namespace ipashelf
