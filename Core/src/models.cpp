// SPDX-License-Identifier: MIT

#include "ipashelf/models.hpp"

#include "ipashelf/json.hpp"

#include <algorithm>
#include <random>

namespace ipashelf {

std::string to_string(Platform platform) {
  switch (platform) {
    case Platform::kIPhone:
      return "iphone";
    case Platform::kIPad:
      return "ipad";
    case Platform::kAppleTV:
      return "appletv";
    case Platform::kWatchOS:
      return "watchos";
    case Platform::kVisionOS:
      return "visionos";
    case Platform::kMacOS:
      return "macos";
    case Platform::kUnspecified:
      break;
  }
  return "unknown";
}

Result<Platform> parse_platform(std::string_view value) {
  const std::string normalized = to_lower(trim(value));
  if (normalized.empty()) return Platform::kUnspecified;
  if (normalized == "iphone" || normalized == "ios") return Platform::kIPhone;
  if (normalized == "ipad" || normalized == "ipados") return Platform::kIPad;
  if (normalized == "appletv" || normalized == "tvos" || normalized == "tv") {
    return Platform::kAppleTV;
  }
  if (normalized == "watch" || normalized == "watchos") {
    return Platform::kWatchOS;
  }
  if (normalized == "vision" || normalized == "visionos" ||
      normalized == "xros" || normalized == "visionpro") {
    return Platform::kVisionOS;
  }
  if (normalized == "mac" || normalized == "macos" || normalized == "osx") {
    return Platform::kMacOS;
  }
  return make_error(ErrorCode::kInvalidArgument, "unknown platform");
}

Result<std::string> search_entity(Platform platform) {
  switch (platform) {
    case Platform::kUnspecified:
      return std::string("software,iPadSoftware");
    case Platform::kIPhone:
      return std::string("software");
    case Platform::kIPad:
      return std::string("iPadSoftware");
    case Platform::kAppleTV:
      return std::string("software,tvSoftware");
    case Platform::kWatchOS:
      return std::string("software,watchSoftware");
    case Platform::kVisionOS:
      return std::string("xrosSoftware");
    case Platform::kMacOS:
      return std::string("macSoftware");
  }
  return make_error(ErrorCode::kInvalidArgument, "unknown platform");
}

Result<std::string> lookup_entity(Platform platform) {
  switch (platform) {
    case Platform::kUnspecified:
      return std::string("software,iPadSoftware");
    case Platform::kIPhone:
      return std::string("software");
    case Platform::kIPad:
      return std::string("iPadSoftware");
    case Platform::kAppleTV:
      return std::string("tvSoftware");
    case Platform::kWatchOS:
      return std::string("watchSoftware");
    case Platform::kVisionOS:
      return std::string("xrosSoftware");
    case Platform::kMacOS:
      return std::string("macSoftware");
  }
  return make_error(ErrorCode::kInvalidArgument, "unknown platform");
}

Result<std::string> catalog_platform(Platform platform) {
  switch (platform) {
    case Platform::kIPhone:
    case Platform::kUnspecified:
      return std::string("iphone");
    case Platform::kIPad:
      return std::string("ipad");
    case Platform::kAppleTV:
      return std::string("appletv");
    case Platform::kWatchOS:
      return std::string("watch");
    case Platform::kVisionOS:
      return std::string("vision");
    case Platform::kMacOS:
      return std::string("mac");
  }
  return make_error(ErrorCode::kInvalidArgument, "unknown platform");
}

Result<std::string> bundle_platform(Platform platform) {
  switch (platform) {
    case Platform::kIPhone:
    case Platform::kIPad:
    case Platform::kUnspecified:
      return std::string("iPhoneOS");
    case Platform::kAppleTV:
      return std::string("AppleTVOS");
    case Platform::kWatchOS:
      return std::string("WatchOS");
    case Platform::kVisionOS:
      return std::string("XROS");
    case Platform::kMacOS:
      return make_error(ErrorCode::kUnsupported,
                        "macOS packages are not supported by the IPA pipeline");
  }
  return make_error(ErrorCode::kInvalidArgument, "unknown platform");
}

std::string Account::identifier() const { return to_lower(trim(email)); }

std::string Account::storefront_country_prefix() const {
  const std::size_t comma = storefront.find(',');
  const std::string first = storefront.substr(0, comma);
  const std::size_t dash = first.find('-');
  return dash == std::string::npos ? first : first.substr(0, dash);
}

std::string HardwareIdentity::guid() const { return hex_encode(bytes, true); }

Result<std::vector<std::uint8_t>> HardwareIdentity::sap_block() const {
  if (bytes.empty() || bytes.size() > 20) {
    return make_error(ErrorCode::kInvalidArgument,
                      "hardware identity must contain 1 to 20 bytes");
  }
  std::vector<std::uint8_t> block(24, 0);
  const auto size = static_cast<std::uint32_t>(bytes.size());
  block[0] = static_cast<std::uint8_t>(size & 0xff);
  block[1] = static_cast<std::uint8_t>((size >> 8) & 0xff);
  block[2] = static_cast<std::uint8_t>((size >> 16) & 0xff);
  block[3] = static_cast<std::uint8_t>((size >> 24) & 0xff);
  std::copy(bytes.begin(), bytes.end(), block.begin() + 4);
  return block;
}

Result<HardwareIdentity> HardwareIdentity::from_hex(std::string_view hex) {
  auto decoded = hex_decode(hex);
  if (!decoded) return decoded.error();
  HardwareIdentity identity;
  identity.bytes = std::move(decoded).value();
  if (identity.bytes.size() != 6) {
    return make_error(ErrorCode::kInvalidArgument,
                      "device identifier must be six bytes");
  }
  return identity;
}

HardwareIdentity HardwareIdentity::random() {
  std::random_device device;
  std::mt19937_64 generator(device());
  HardwareIdentity identity;
  identity.bytes.resize(6);
  for (auto& byte : identity.bytes) {
    byte = static_cast<std::uint8_t>(generator() & 0xff);
  }
  // Use a locally administered unicast MAC-like value. It is not a real MAC;
  // it is an opaque protocol identifier persisted in the Keychain.
  identity.bytes[0] = static_cast<std::uint8_t>((identity.bytes[0] & 0xfe) | 0x02);
  return identity;
}

}  // namespace ipashelf

namespace ipashelf {

std::string account_to_json(const Account& account) {
  Json root = Json::object();
  root.set("email", Json::string(account.email));
  root.set("name", Json::string(account.name));
  root.set("password", Json::string(account.password));
  root.set("password_token", Json::string(account.password_token));
  root.set("directory_services_id", Json::string(account.directory_services_id));
  root.set("storefront", Json::string(account.storefront));
  root.set("pod", Json::string(account.pod));
  root.set("region", Json::string(account.region));
  root.set("device_id", Json::string(account.device_id));
  return root.dump();
}

Result<Account> account_from_json(std::string_view json) {
  auto parsed = Json::parse(json);
  if (!parsed) return parsed.error();
  const Json& root = parsed.value();
  if (!root.is_object()) {
    return make_error(ErrorCode::kParse, "account record is not an object");
  }
  Account account;
  account.email = root.find("email") != nullptr ? root.find("email")->string_or()
                                                : std::string();
  account.name = root.find("name") != nullptr ? root.find("name")->string_or()
                                              : std::string();
  account.password = root.find("password") != nullptr
                         ? root.find("password")->string_or()
                         : std::string();
  account.password_token = root.find("password_token") != nullptr
                               ? root.find("password_token")->string_or()
                               : std::string();
  account.directory_services_id =
      root.find("directory_services_id") != nullptr
          ? root.find("directory_services_id")->string_or()
          : std::string();
  account.storefront = root.find("storefront") != nullptr
                           ? root.find("storefront")->string_or()
                           : std::string();
  account.pod = root.find("pod") != nullptr ? root.find("pod")->string_or()
                                            : std::string();
  account.region = root.find("region") != nullptr
                       ? root.find("region")->string_or()
                       : std::string();
  account.device_id = root.find("device_id") != nullptr
                          ? root.find("device_id")->string_or()
                          : std::string();
  if (account.email.empty() || account.directory_services_id.empty()) {
    return make_error(ErrorCode::kParse, "account record is incomplete");
  }
  return account;
}

std::string accounts_to_json(const std::vector<Account>& accounts) {
  std::vector<Json> items;
  items.reserve(accounts.size());
  for (const auto& account : accounts) {
    auto parsed = Json::parse(account_to_json(account));
    if (parsed) items.push_back(std::move(parsed).value());
  }
  return Json::array(std::move(items)).dump();
}

Result<std::vector<Account>> accounts_from_json(std::string_view json) {
  auto parsed = Json::parse(json);
  if (!parsed) return parsed.error();
  if (!parsed.value().is_array()) {
    return make_error(ErrorCode::kParse, "account list is not an array");
  }
  std::vector<Account> accounts;
  for (const auto& entry : parsed.value().array_items()) {
    auto account = account_from_json(entry.dump());
    if (!account) continue;
    accounts.push_back(std::move(account).value());
  }
  return accounts;
}

namespace {

// Apple's numeric storefront identifiers. These are protocol constants, not
// copied implementation: each entry names the storefront front used by
// `X-Apple-Store-Front` for that country.
const std::vector<Storefront>& storefront_table() {
  static const std::vector<Storefront> table = {
      {"US", "United States", "143441", "USD"},
      {"JP", "Japan", "143462", "JPY"},
      {"GB", "United Kingdom", "143444", "GBP"},
      {"CA", "Canada", "143455", "CAD"},
      {"AU", "Australia", "143460", "AUD"},
      {"NZ", "New Zealand", "143461", "NZD"},
      {"DE", "Germany", "143443", "EUR"},
      {"FR", "France", "143442", "EUR"},
      {"IT", "Italy", "143450", "EUR"},
      {"ES", "Spain", "143454", "EUR"},
      {"NL", "Netherlands", "143452", "EUR"},
      {"BE", "Belgium", "143446", "EUR"},
      {"AT", "Austria", "143445", "EUR"},
      {"CH", "Switzerland", "143459", "CHF"},
      {"SE", "Sweden", "143456", "SEK"},
      {"NO", "Norway", "143457", "NOK"},
      {"DK", "Denmark", "143458", "DKK"},
      {"FI", "Finland", "143447", "EUR"},
      {"IE", "Ireland", "143449", "EUR"},
      {"PT", "Portugal", "143453", "EUR"},
      {"PL", "Poland", "143478", "PLN"},
      {"CZ", "Czechia", "143489", "CZK"},
      {"GR", "Greece", "143448", "EUR"},
      {"TR", "Türkiye", "143480", "TRY"},
      {"UA", "Ukraine", "143492", "UAH"},
      {"IL", "Israel", "143471", "ILS"},
      {"SA", "Saudi Arabia", "143479", "SAR"},
      {"AE", "United Arab Emirates", "143481", "AED"},
      {"EG", "Egypt", "143516", "EGP"},
      {"NG", "Nigeria", "143561", "NGN"},
      {"ZA", "South Africa", "143472", "ZAR"},
      {"IN", "India", "143467", "INR"},
      {"SG", "Singapore", "143464", "SGD"},
      {"HK", "Hong Kong", "143463", "HKD"},
      {"TW", "Taiwan", "143470", "TWD"},
      {"CN", "China mainland", "143465", "CNY"},
      {"KR", "Korea", "143466", "KRW"},
      {"TH", "Thailand", "143475", "THB"},
      {"MY", "Malaysia", "143473", "MYR"},
      {"ID", "Indonesia", "143476", "IDR"},
      {"PH", "Philippines", "143474", "PHP"},
      {"VN", "Vietnam", "143482", "VND"},
      {"BR", "Brazil", "143503", "BRL"},
      {"MX", "Mexico", "143468", "MXN"},
      {"AR", "Argentina", "143505", "ARS"},
      {"CL", "Chile", "143483", "CLP"},
      {"CO", "Colombia", "143501", "COP"},
      {"PE", "Peru", "143504", "PEN"},
      {"RU", "Russia", "143469", "RUB"},
  };
  return table;
}

}  // namespace

const std::vector<Storefront>& storefronts() { return storefront_table(); }

std::optional<Storefront> storefront_for_region(std::string_view code) {
  const std::string wanted = to_upper(trim(code));
  for (const auto& entry : storefront_table()) {
    if (entry.code == wanted) return entry;
  }
  return std::nullopt;
}

std::string storefront_header(const Storefront& storefront) {
  if (storefront.storefront_id.empty()) return {};
  return storefront.storefront_id + "-1,29";
}

}  // namespace ipashelf
