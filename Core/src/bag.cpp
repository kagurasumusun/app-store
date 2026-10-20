// SPDX-License-Identifier: MIT

#include "ipashelf/bag.hpp"

#include <cstdlib>

#include "ipashelf/url.hpp"

namespace ipashelf {
namespace {

constexpr std::string_view kBagHost = "init.itunes.apple.com";
constexpr std::string_view kBagPath = "/bag.xml";
constexpr std::string_view kStoreHost = "buy.itunes.apple.com";
constexpr std::string_view kAuthPath =
    "/WebObjects/MZFinance.woa/wa/authenticate";
constexpr std::string_view kDownloadDispatchHost =
    "downloaddispatch.itunes.apple.com";
constexpr std::string_view kEntDownloadPath =
    "/WebObjects/DownloadDispatch.woa/wa/ent/download";
constexpr std::string_view kRedownloadPath = "/r/redownload";
constexpr std::string_view kUpdatePath = "/up/updateProduct";

bool store_host(std::string_view host) {
  const std::string lower = to_lower(host);
  return lower == kStoreHost || ends_with(lower, "-buy.itunes.apple.com");
}

// Common shape: absolute HTTPS URL with a host, no credentials, no fragment
// and no port other than the default.
bool safe_https(const Url& url) {
  return url.https() && !url.host.empty() && url.userinfo.empty() &&
         url.fragment.empty() && (!url.port || *url.port == 443);
}

Status validate_dispatch_endpoint(std::string_view label,
                                  std::string_view value,
                                  std::string_view expected_path) {
  // No query or fragment of any kind (including a bare trailing '?').
  if (value.find('?') != std::string_view::npos ||
      value.find('#') != std::string_view::npos) {
    return fail(ErrorCode::kParse, "Apple bag contains an unsafe " +
                                       std::string(label) + " URL");
  }
  auto parsed = Url::parse(value);
  if (!parsed || !safe_https(parsed.value()) ||
      to_lower(parsed.value().host) != kDownloadDispatchHost ||
      parsed.value().path != expected_path) {
    return fail(ErrorCode::kParse, "Apple bag contains an unsafe " +
                                       std::string(label) + " URL");
  }
  return ok_status();
}

}  // namespace

std::string bag_request_url(const HardwareIdentity& identity) {
  return "https://" + std::string(kBagHost) + std::string(kBagPath) +
         "?guid=" + percent_encode(identity.guid());
}

Status validate_auth_endpoint(std::string_view endpoint) {
  auto parsed = Url::parse(endpoint);
  if (!parsed || !safe_https(parsed.value())) {
    return fail(ErrorCode::kParse, "invalid authentication endpoint");
  }
  if (!store_host(parsed.value().host)) {
    return fail(ErrorCode::kParse, "unsupported authentication endpoint");
  }
  const std::string& path = parsed.value().path;
  if (path != kAuthPath && path != std::string(kAuthPath) + "/") {
    return fail(ErrorCode::kParse, "unsupported authentication endpoint");
  }
  return ok_status();
}

Result<std::string> normalize_auth_endpoint(std::string_view endpoint) {
  if (auto status = validate_auth_endpoint(endpoint); !status) {
    return status.error();
  }
  auto parsed = Url::parse(endpoint);
  Url url = parsed.value();
  // The bare path can return unusable responses, including a 301 without a
  // Location. Retain host, port and query; only fix the path.
  url.path = std::string(kAuthPath) + "/";
  return url.serialize();
}

Result<Bag> Bag::parse(const PlistValue& document) {
  Bag bag;
  bag.raw = document;
  const PlistValue* root = &document;
  if (const PlistValue* nested = document.find("urlBag");
      nested != nullptr && nested->is_dictionary()) {
    root = nested;
  }

  if (!root->is_dictionary()) {
    return make_error(ErrorCode::kParse, "Apple bag is not a dictionary");
  }

  if (auto value = root->string_for("authenticateAccount")) {
    bag.auth_endpoint = *value;
  }
  // Despite the key name, current bags return the DownloadDispatch
  // ent/download URL here; the legacy volume store URL is rebuilt from the
  // account pod at download time.
  if (auto value = root->string_for("volumeStoreDownloadProduct")) {
    bag.ent_download_endpoint = *value;
  }
  if (auto value = root->string_for("redownloadProduct")) {
    bag.redownload_endpoint = *value;
  }
  if (auto value = root->string_for("updateProduct")) {
    bag.update_endpoint = *value;
  }
  if (auto value = root->string_for("sign-sap-setup")) {
    bag.sap_setup_url = *value;
  }
  if (auto value = root->string_for("sign-sap-setup-cert")) {
    bag.sap_setup_cert_url = *value;
  }
  if (auto value = root->integer_for("sign-sap-version")) {
    bag.sap_version = static_cast<int>(*value);
  } else if (auto value = root->string_for("sign-sap-version")) {
    char* end = nullptr;
    const long parsed = std::strtol(value->c_str(), &end, 10);
    if (end != nullptr && *end == '\0') {
      bag.sap_version = static_cast<int>(parsed);
    }
  }

  return bag;
}

Status Bag::validate() const {
  if (!auth_endpoint.empty()) {
    if (auto status = validate_auth_endpoint(auth_endpoint); !status) {
      return status;
    }
  }
  if (!ent_download_endpoint.empty()) {
    if (auto status = validate_dispatch_endpoint(
            "ent/download", ent_download_endpoint, kEntDownloadPath);
        !status) {
      return status;
    }
  }
  if (!redownload_endpoint.empty()) {
    if (auto status = validate_dispatch_endpoint("redownload",
                                                  redownload_endpoint,
                                                  kRedownloadPath);
        !status) {
      return status;
    }
  }
  if (!update_endpoint.empty()) {
    if (auto status = validate_dispatch_endpoint("update", update_endpoint,
                                                  kUpdatePath);
        !status) {
      return status;
    }
  }
  for (const std::pair<std::string_view, const std::string*> endpoint :
       {std::pair{"SAP setup", &sap_setup_url},
        std::pair{"SAP setup certificate", &sap_setup_cert_url}}) {
    if (endpoint.second->empty()) continue;
    auto parsed = Url::parse(*endpoint.second);
    if (!parsed || !parsed.value().https() || parsed.value().host.empty()) {
      return fail(ErrorCode::kParse, "Apple bag contains an unsafe " +
                                         std::string(endpoint.first) + " URL");
    }
  }
  if (sap_version != 0 && sap_version != 200) {
    return fail(ErrorCode::kUnsupported,
                "Apple bag requires an unsupported SAP version " +
                    std::to_string(sap_version));
  }
  return ok_status();
}

Status Bag::validate_for_authentication() const {
  if (auto status = validate_auth_endpoint(auth_endpoint); !status) {
    return status;
  }
  for (const std::pair<std::string_view, const std::string*> endpoint :
       {std::pair{"SAP setup", &sap_setup_url},
        std::pair{"SAP setup certificate", &sap_setup_cert_url}}) {
    auto parsed = Url::parse(*endpoint.second);
    if (!parsed || !parsed.value().https() || parsed.value().host.empty()) {
      return fail(ErrorCode::kParse, "invalid " +
                                         std::string(endpoint.first) +
                                         " endpoint in Apple bag");
    }
  }
  if (sap_version != 200) {
    return fail(ErrorCode::kUnsupported,
                "unsupported SAP version " + std::to_string(sap_version) +
                    " in Apple bag");
  }
  return ok_status();
}

}  // namespace ipashelf
