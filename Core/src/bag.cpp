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
constexpr std::string_view kVolumePath =
    "/WebObjects/MZFinance.woa/wa/volumeStoreDownloadProduct";
constexpr std::string_view kRedownloadHost = "downloaddispatch.itunes.apple.com";
constexpr std::string_view kRedownloadPath = "/r/redownload";
constexpr std::string_view kUpdatePath = "/up/updateProduct";

bool store_host(std::string_view host) {
  const std::string lower = to_lower(host);
  return lower == kStoreHost || ends_with(lower, "-buy.itunes.apple.com");
}

bool auth_host(std::string_view host) {
  const std::string lower = to_lower(host);
  return lower == "auth.itunes.apple.com" || store_host(lower);
}

Status validate_endpoint(std::string_view label, std::string_view value,
                         bool required) {
  if (value.empty()) {
    return required ? fail(ErrorCode::kParse,
                           "Apple bag is missing " + std::string(label))
                    : ok_status();
  }
  auto parsed = Url::parse(value);
  if (!parsed) {
    return fail(ErrorCode::kParse, "Apple bag contains an invalid " +
                                       std::string(label) + " URL");
  }
  const Url& url = parsed.value();
  if (!url.https() || url.host.empty() || !url.userinfo.empty() ||
      !url.fragment.empty() || url.port.has_value()) {
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
  if (auto value = root->string_for("volumeStoreDownloadProduct")) {
    bag.volume_store_endpoint = *value;
  }
  if (auto value = root->string_for("redownloadProduct")) {
    bag.redownload_endpoint = *value;
  }
  if (auto value = root->string_for("updateProduct")) {
    bag.update_endpoint = *value;
  }
  if (auto value = root->string_for("entDownloadProduct")) {
    bag.ent_download_endpoint = *value;
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
    if (end != nullptr && *end == '\0') bag.sap_version = static_cast<int>(parsed);
  }

  auto status = bag.validate();
  if (!status) return status.error();
  return bag;
}

Status Bag::validate() const {
  if (auto status = validate_endpoint("authentication", auth_endpoint, true);
      !status) {
    return status;
  }
  if (auto status = validate_endpoint("SAP setup", sap_setup_url, false);
      !status) {
    return status;
  }
  if (auto status = validate_endpoint("SAP certificate", sap_setup_cert_url,
                                      false);
      !status) {
    return status;
  }
  if (auto status = validate_endpoint("volume store", volume_store_endpoint,
                                      false);
      !status) {
    return status;
  }
  if (auto status = validate_endpoint("redownload", redownload_endpoint, false);
      !status) {
    return status;
  }
  if (auto status = validate_endpoint("update", update_endpoint, false);
      !status) {
    return status;
  }

  auto auth = Url::parse(auth_endpoint);
  std::string auth_path = auth ? auth.value().path : std::string();
  if (!auth_path.empty() && auth_path.back() == '/') auth_path.pop_back();
  if (!auth || !auth_host(auth.value().host) || auth_path != std::string(kAuthPath)) {
    return fail(ErrorCode::kParse,
                "Apple bag authentication endpoint is not the Store endpoint");
  }

  if (!volume_store_endpoint.empty()) {
    auto url = Url::parse(volume_store_endpoint);
    if (!url || !store_host(url.value().host) ||
        url.value().path != std::string(kVolumePath)) {
      return fail(ErrorCode::kParse,
                  "Apple bag volume store endpoint is invalid");
    }
  }

  if (!redownload_endpoint.empty()) {
    auto url = Url::parse(redownload_endpoint);
    if (!url || to_lower(url.value().host) != kRedownloadHost ||
        url.value().path != std::string(kRedownloadPath) ||
        !url.value().query.empty()) {
      return fail(ErrorCode::kParse,
                  "Apple bag redownload endpoint is invalid");
    }
  }

  if (!update_endpoint.empty()) {
    auto url = Url::parse(update_endpoint);
    if (!url || to_lower(url.value().host) != kRedownloadHost ||
        url.value().path != std::string(kUpdatePath)) {
      return fail(ErrorCode::kParse, "Apple bag update endpoint is invalid");
    }
  }

  if (sap_version != 0 && sap_version != 200) {
    return fail(ErrorCode::kUnsupported,
                "Apple bag requires an unsupported SAP version " +
                    std::to_string(sap_version));
  }
  return ok_status();
}

Result<std::string> normalize_auth_endpoint(std::string_view endpoint) {
  auto parsed = Url::parse(endpoint);
  if (!parsed) return parsed.error();
  Url url = parsed.value();
  if (!auth_host(url.host)) {
    return make_error(ErrorCode::kParse,
                      "authentication endpoint is not an Apple Store host");
  }
  if (url.path != std::string(kAuthPath) &&
      url.path != std::string(kAuthPath) + "/") {
    return make_error(ErrorCode::kParse,
                      "authentication endpoint path is not supported");
  }
  url.path = std::string(kAuthPath) + "/";
  return url.serialize();
}

}  // namespace ipashelf
