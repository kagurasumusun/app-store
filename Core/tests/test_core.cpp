// SPDX-License-Identifier: MIT
//
// Portable protocol tests. These run with a fake HTTP transport and a fake SAP
// backend so no Apple service or platform framework is required.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <thread>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "ipashelf/bag.hpp"
#include "ipashelf/base.hpp"
#include "ipashelf/crypto.hpp"
#include "ipashelf/ipa.hpp"
#include "ipashelf/json.hpp"
#include "ipashelf/models.hpp"
#include "ipashelf/ota.hpp"
#include "ipashelf/plist.hpp"
#include "ipashelf/sap.hpp"
#include "ipashelf/store_client.hpp"
#include "ipashelf/url.hpp"
#include "ipashelf/x509.hpp"

#include "miniz.h"

#if defined(__unix__) || defined(__APPLE__)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

int g_failures = 0;
int g_checks = 0;

void expect(bool condition, std::string_view message) {
  ++g_checks;
  if (!condition) {
    ++g_failures;
    std::cerr << "FAIL: " << message << "\n";
  }
}

template <typename Actual, typename Expected>
void expect_eq(const Actual& actual, const Expected& expected,
               std::string_view message) {
  ++g_checks;
  if (!(actual == expected)) {
    ++g_failures;
    std::cerr << "FAIL: " << message << " (actual=" << actual
              << " expected=" << expected << ")\n";
  }
}

void expect_bytes(const ipashelf::Bytes& actual,
                  const ipashelf::Bytes& expected,
                  std::string_view message) {
  ++g_checks;
  if (actual != expected) {
    ++g_failures;
    std::cerr << "FAIL: " << message << "\n";
  }
}

std::string text_of(const ipashelf::Bytes& bytes) {
  return std::string(bytes.begin(), bytes.end());
}

ipashelf::Bytes bytes_of(std::string_view text) {
  return ipashelf::Bytes(text.begin(), text.end());
}

ipashelf::Bytes read_file(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return {};
  return ipashelf::Bytes(std::istreambuf_iterator<char>(stream),
                         std::istreambuf_iterator<char>());
}

// ---------------------------------------------------------------------------
// Fakes
// ---------------------------------------------------------------------------

class FakeTransport final : public ipashelf::HttpTransport {
 public:
  using Handler =
      std::function<ipashelf::Result<ipashelf::HttpResponse>(const ipashelf::HttpRequest&)>;

  void enqueue(Handler handler) { handlers_.push_back(std::move(handler)); }

  void enqueue_response(int status, std::string body,
                        ipashelf::HttpHeaders headers = {}) {
    auto response = ipashelf::HttpResponse{};
    response.status = status;
    response.body = bytes_of(body);
    response.headers = std::move(headers);
    enqueue([response](const ipashelf::HttpRequest&) {
      return ipashelf::Result<ipashelf::HttpResponse>(response);
    });
  }

  void enqueue_plist(const ipashelf::PlistValue& document, int status = 200,
                     ipashelf::HttpHeaders headers = {}) {
    enqueue_response(status, document.dump_xml(), std::move(headers));
  }

  ipashelf::Result<ipashelf::HttpResponse> send(
      const ipashelf::HttpRequest& request,
      const ipashelf::CancellationToken&) override {
    requests.push_back(request);
    if (handlers_.empty()) {
      return ipashelf::make_error(ipashelf::ErrorCode::kInternal,
                                  "FakeTransport ran out of responses");
    }
    auto handler = std::move(handlers_.front());
    handlers_.pop_front();
    return handler(request);
  }

  std::vector<ipashelf::HttpRequest> requests;

 private:
  std::deque<Handler> handlers_;
};

class FakeSapBackend final : public ipashelf::SapBackend {
 public:
  ipashelf::Result<std::uint64_t> initialize(
      const ipashelf::Bytes& hardware_block) override {
    initialized_with = hardware_block;
    return std::uint64_t{0x51};
  }

  ipashelf::Result<std::pair<ipashelf::Bytes, std::int32_t>> exchange(
      std::uint32_t version, const ipashelf::Bytes&, std::uint64_t context,
      const ipashelf::Bytes& input) override {
    ++exchange_calls;
    if (context != 0x51) return make_error(ipashelf::ErrorCode::kInternal,
                                           "unexpected context");
    if (exchange_calls == 1) {
      if (input.empty()) return make_error(ipashelf::ErrorCode::kInternal,
                                           "certificate payload missing");
      if (version != 200) return make_error(ipashelf::ErrorCode::kInternal,
                                            "unexpected SAP version");
      return std::make_pair(bytes_of("sap-request"), std::int32_t{1});
    }
    last_exchange_input = input;
    return std::make_pair(bytes_of("sap-final"), std::int32_t{0});
  }

  ipashelf::Result<ipashelf::Bytes> sign(std::uint64_t context,
                                         const ipashelf::Bytes& input) override {
    ++sign_calls;
    if (context != 0x51) return make_error(ipashelf::ErrorCode::kInternal,
                                           "unexpected context");
    signed_payloads.push_back(text_of(input));
    ipashelf::Bytes output = bytes_of("SIG:");
    output.insert(output.end(), input.begin(), input.end());
    return output;
  }

  ipashelf::Status teardown(std::uint64_t context) override {
    ++teardown_calls;
    if (context != 0x51) return ipashelf::fail(ipashelf::ErrorCode::kInternal,
                                               "unexpected context");
    return ipashelf::ok_status();
  }

  ipashelf::Bytes initialized_with;
  ipashelf::Bytes last_exchange_input;
  std::vector<std::string> signed_payloads;
  int exchange_calls = 0;
  int sign_calls = 0;
  int teardown_calls = 0;
};

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

ipashelf::PlistValue make_bag() {
  ipashelf::PlistValue bag = ipashelf::PlistValue::dictionary();
  bag.set("authenticateAccount",
          ipashelf::PlistValue::string(
              "https://buy.itunes.apple.com/WebObjects/MZFinance.woa/wa/"
              "authenticate/"));
  bag.set("volumeStoreDownloadProduct",
          ipashelf::PlistValue::string(
              "https://p25-buy.itunes.apple.com/WebObjects/MZFinance.woa/wa/"
              "volumeStoreDownloadProduct"));
  bag.set("redownloadProduct",
          ipashelf::PlistValue::string(
              "https://downloaddispatch.itunes.apple.com/r/redownload"));
  bag.set("updateProduct",
          ipashelf::PlistValue::string(
              "https://downloaddispatch.itunes.apple.com/up/updateProduct"));
  bag.set("sign-sap-setup",
          ipashelf::PlistValue::string(
              "https://p25-buy.itunes.apple.com/WebObjects/MZFinance.woa/wa/"
              "signSapSetup"));
  bag.set("sign-sap-setup-cert",
          ipashelf::PlistValue::string(
              "https://p25-buy.itunes.apple.com/WebObjects/MZFinance.woa/wa/"
              "signSapSetupCert"));
  bag.set("sign-sap-version", ipashelf::PlistValue::integer(200));
  return bag;
}

ipashelf::PlistValue make_certificate() {
  ipashelf::PlistValue certificate = ipashelf::PlistValue::dictionary();
  certificate.set("sign-sap-setup-cert",
                  ipashelf::PlistValue::data(bytes_of("certificate-der")));
  return certificate;
}

ipashelf::PlistValue make_setup_reply() {
  ipashelf::PlistValue reply = ipashelf::PlistValue::dictionary();
  reply.set("sign-sap-setup-buffer",
            ipashelf::PlistValue::data(bytes_of("apple-challenge")));
  return reply;
}

ipashelf::PlistValue make_auth_reply() {
  ipashelf::PlistValue reply = ipashelf::PlistValue::dictionary();
  reply.set("passwordToken", ipashelf::PlistValue::string("password-token"));
  reply.set("dsPersonId", ipashelf::PlistValue::string("1234567890"));
  ipashelf::PlistValue address = ipashelf::PlistValue::dictionary();
  address.set("firstName", ipashelf::PlistValue::string("Ada"));
  address.set("lastName", ipashelf::PlistValue::string("Lovelace"));
  ipashelf::PlistValue account = ipashelf::PlistValue::dictionary();
  account.set("appleId", ipashelf::PlistValue::string("user@example.com"));
  account.set("address", std::move(address));
  reply.set("accountInfo", std::move(account));
  return reply;
}

ipashelf::PlistValue make_download_reply() {
  ipashelf::PlistValue sinf = ipashelf::PlistValue::dictionary();
  sinf.set("id", ipashelf::PlistValue::integer(1));
  sinf.set("sinf", ipashelf::PlistValue::data(bytes_of("sinf-bytes")));
  ipashelf::PlistValue sinfs = ipashelf::PlistValue::array();
  sinfs.append(std::move(sinf));

  ipashelf::PlistValue metadata = ipashelf::PlistValue::dictionary();
  metadata.set("bundleShortVersionString",
               ipashelf::PlistValue::string("3.1.4"));
  metadata.set("softwareVersionExternalIdentifier",
               ipashelf::PlistValue::string("9001"));
  metadata.set("softwareVersionBundleId",
               ipashelf::PlistValue::string("com.example.sample"));
  ipashelf::PlistValue versions = ipashelf::PlistValue::array();
  versions.append(ipashelf::PlistValue::integer(9001));
  versions.append(ipashelf::PlistValue::integer(8800));
  metadata.set("softwareVersionExternalIdentifiers", std::move(versions));

  ipashelf::PlistValue item = ipashelf::PlistValue::dictionary();
  item.set("URL", ipashelf::PlistValue::string(
                      "https://iosapps.itunes.apple.com/package.ipa"));
  item.set("md5", ipashelf::PlistValue::string("D41D8CD98F00B204E9800998ECF8427E"));
  item.set("sinfs", std::move(sinfs));
  item.set("metadata", std::move(metadata));

  ipashelf::PlistValue songs = ipashelf::PlistValue::array();
  songs.append(std::move(item));

  ipashelf::PlistValue reply = ipashelf::PlistValue::dictionary();
  reply.set("songList", std::move(songs));
  return reply;
}

std::string catalog_json(std::int64_t app_id, std::string_view external_id,
                         bool include_ios_family = true) {
  return "{\"data\":[{\"id\":" + std::to_string(app_id) +
         ",\"type\":\"apps\",\"attributes\":{\"deviceFamilies\":[\"iphone\"" +
         (include_ios_family ? ",\"ipad\"" : "") +
         "],\"platformAttributes\":{\"ios\":{\"bundleId\":"
         "\"com.example.sample\",\"externalVersionId\":" +
         std::string(external_id) + "}}}}]}";
}

struct LoginFixture {
  FakeTransport transport;
  std::shared_ptr<FakeSapBackend> sap = std::make_shared<FakeSapBackend>();
  std::unique_ptr<ipashelf::StoreClient> client;
  ipashelf::HardwareIdentity hardware;

  LoginFixture() {
    hardware = ipashelf::HardwareIdentity::random();
    ipashelf::StoreConfiguration configuration;
    configuration.sleep = [](std::chrono::milliseconds) {};
    configuration.sap_backend_factory = [this]() {
      return ipashelf::Result<std::shared_ptr<ipashelf::SapBackend>>(sap);
    };
    client = std::make_unique<ipashelf::StoreClient>(transport,
                                                    std::move(configuration));
    transport.enqueue_plist(make_bag());
    transport.enqueue_plist(make_certificate());
    transport.enqueue_plist(make_setup_reply());
  }
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

void test_crypto() {
  using namespace ipashelf;
  expect_eq(md5_hex("", 0), std::string("d41d8cd98f00b204e9800998ecf8427e"),
            "md5 empty");
  expect_eq(md5_hex("abc", 3), std::string("900150983cd24fb0d6963f7d28e17f72"),
            "md5 abc");
  expect_eq(sha256_hex("abc", 3),
            std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
            "sha256 abc");
  expect_eq(base64_encode(bytes_of("hello")), std::string("aGVsbG8="),
            "base64 encode");

  auto decoded = base64_decode("aGVsbG8=");
  expect(decoded.has_value(), "base64 decode succeeds");
  if (decoded) expect_eq(text_of(decoded.value()), std::string("hello"),
                         "base64 round trip");
  expect(!base64_decode("!!!").has_value(), "base64 rejects invalid input");
}

void test_json() {
  using namespace ipashelf;
  auto parsed = Json::parse(
      "{\"a\":1,\"b\":[true,null,\"x\"],\"c\":1.50,"
      "\"unicode\":\"\\u30a2\\ud83d\\ude00\"}");
  expect(parsed.has_value(), "json parses");
  if (!parsed) return;
  const Json& root = parsed.value();
  expect(root.is_object(), "json root is object");
  expect_eq(root.find("a")->dump(), std::string("1"), "json integer");
  expect_eq(root.find("c")->dump(), std::string("1.50"),
            "json preserves number spelling");
  expect(root.find("b")->array_items().size() == 3, "json array size");
  expect_eq(root.find("unicode")->string_or(), std::string("\u30a2\U0001F600"),
            "json unicode escapes");
  expect_eq(root.find("missing"), nullptr, "missing key");
}

void test_plist_xml() {
  using namespace ipashelf;
  PlistValue root = PlistValue::dictionary();
  root.set("name", PlistValue::string("example"));
  PlistValue list = PlistValue::array();
  list.append(PlistValue::integer(1));
  list.append(PlistValue::boolean(true));
  root.set("items", std::move(list));

  const std::string xml = root.dump_xml();
  auto parsed = PlistValue::parse(bytes_of(xml));
  expect(parsed.has_value(), "plist xml parses");
  if (!parsed) return;
  expect_eq(parsed.value().string_or("name"), std::string("example"),
            "plist string");
  const PlistValue* items = parsed.value().find("items");
  expect(items != nullptr && items->is_array() && items->array_items().size() == 2,
         "plist array");
  if (items != nullptr && items->array_items().size() == 2) {
    expect(items->array_items()[0].integer_value() == 1, "plist integer");
    expect(items->array_items()[1].bool_value(), "plist boolean");
  }
}

void test_plist_binary(const std::string& fixture_path) {
  using namespace ipashelf;
  const Bytes data = read_file(fixture_path);
  expect(!data.empty(), "binary plist fixture exists");
  if (data.empty()) return;
  auto parsed = PlistValue::parse(data);
  expect(parsed.has_value(), "binary plist parses");
  if (!parsed) return;
  const PlistValue& root = parsed.value();
  expect_eq(root.string_or("CFBundleIdentifier"),
            std::string("com.example.sample"), "bplist bundle id");
  expect_eq(root.string_or("CFBundleShortVersionString"), std::string("2.4.1"),
            "bplist version");
  expect_eq(root.integer_for("ItemCount").value_or(0), 42, "bplist integer");
  expect_eq(root.find("Payload")->data_value().size(), std::size_t{5},
            "bplist data");
  expect_eq(root.find("Rating")->real_value(), 4.5, "bplist real");
  expect(root.find("IsBeta") != nullptr && !root.find("IsBeta")->bool_value(),
         "bplist false");
  expect_eq(root.find("SupportedDevices")->array_items().size(),
            std::size_t{2}, "bplist array");
  expect(root.find("LaunchDate") != nullptr &&
             root.find("LaunchDate")->string_value().find("2026-09-01") !=
                 std::string::npos,
         "bplist date");
  auto missing = root.find("Nope");
  expect(missing == nullptr, "bplist missing key");
}

void test_plist_binary_write(const std::string& fixtures) {
  using namespace ipashelf;
  PlistValue root = PlistValue::dictionary();
  root.set("Name", PlistValue::string("IpaStore"));
  root.set("Unicode", PlistValue::string("\u65e5\u672c\u8a9e \U0001F680"));
  root.set("Small", PlistValue::integer(7));
  root.set("Medium", PlistValue::integer(70000));
  root.set("Large", PlistValue::integer(9000000000LL));
  root.set("Negative", PlistValue::integer(-42));
  root.set("Ratio", PlistValue::real(0.1));
  root.set("Enabled", PlistValue::boolean(true));
  root.set("Disabled", PlistValue::boolean(false));
  root.set("Blob", PlistValue::data(bytes_of("payload")));
  root.set("When", PlistValue::date("2026-09-01T12:30:00Z"));

  PlistValue long_list = PlistValue::array();
  for (int index = 0; index < 20; ++index) {
    long_list.append(PlistValue::integer(index));
  }
  root.set("List", std::move(long_list));

  PlistValue nested = PlistValue::dictionary();
  nested.set("Key", PlistValue::string("Value"));
  root.set("Nested", std::move(nested));

  const Bytes encoded = root.dump_binary();
  expect(encoded.size() > 8 && std::memcmp(encoded.data(), "bplist00", 8) == 0,
         "binary plist header");
  auto parsed = PlistValue::parse(encoded);
  expect(parsed.has_value(), "generated binary plist parses");
  if (!parsed) {
    std::cerr << "  parse error: " << parsed.error().message << "\n";
    return;
  }
  const PlistValue& copy = parsed.value();
  expect_eq(copy.string_or("Name"), std::string("IpaStore"), "bplist write name");
  expect_eq(copy.string_or("Unicode"), std::string("\u65e5\u672c\u8a9e \U0001F680"),
            "bplist write unicode");
  expect_eq(copy.integer_for("Small").value_or(0), 7, "bplist write small");
  expect_eq(copy.integer_for("Medium").value_or(0), 70000, "bplist write medium");
  expect_eq(copy.integer_for("Large").value_or(0), 9000000000LL,
            "bplist write large");
  expect_eq(copy.integer_for("Negative").value_or(0), -42, "bplist write negative");
  expect_eq(copy.find("Enabled")->bool_value(), true, "bplist write true");
  expect_eq(copy.find("Disabled")->bool_value(), false, "bplist write false");
  expect_eq(copy.find("List")->array_items().size(), std::size_t{20},
            "bplist write long array");
  expect_eq(copy.find("Blob")->data_value().size(), std::size_t{7},
            "bplist write data");
  expect(copy.find("When") != nullptr &&
             copy.find("When")->string_value() == "2026-09-01T12:30:00Z",
         "bplist write date");
  expect(copy.find("Nested") != nullptr &&
             copy.find("Nested")->string_or("Key") == "Value",
         "bplist write nested");

  std::ofstream out(fixtures + "/generated.bplist", std::ios::binary);
  out.write(reinterpret_cast<const char*>(encoded.data()),
            static_cast<std::streamsize>(encoded.size()));
}

void test_url() {
  using namespace ipashelf;
  auto resolved = resolve_url("https://buy.itunes.apple.com/a/b/c", "../d?x=1");
  expect(resolved.has_value(), "url resolves");
  if (resolved) {
    expect_eq(resolved.value(), std::string("https://buy.itunes.apple.com/a/d?x=1"),
              "relative resolution");
  }
  expect(host_is_or_subdomain("x.buy.itunes.apple.com", "buy.itunes.apple.com"),
         "subdomain accepted");
  expect(!host_is_or_subdomain("p25-buy.itunes.apple.com",
                               "buy.itunes.apple.com"),
         "pod host is not a plain subdomain");
  expect(!host_is_or_subdomain("evil-buy.itunes.apple.com", "buy.itunes.apple.com"),
         "lookalike host rejected");

  auto parsed = Url::parse("https://user:pass@example.com:8443/p?q=1#frag");
  expect(parsed.has_value(), "url parses");
  if (parsed) {
    expect_eq(parsed.value().host, std::string("example.com"), "host");
    expect(parsed.value().port.has_value(), "port parsed");
    expect_eq(parsed.value().path, std::string("/p"), "path");
  }
  expect_eq(percent_encode("a b/ü"), std::string("a%20b%2F%C3%BC"),
            "percent encoding");
  expect_eq(form_encode({{"a", "b c"}, {"d", "&"}}), std::string("a=b+c&d=%26"),
            "form encoding");
  // Ports are validated digit by digit; the previous implementation read a
  // pointer into a destroyed temporary (ASan: stack-use-after-scope).
  expect(Url::parse("https://example.com:65535/").has_value(),
         "maximum port accepted");
  expect(!Url::parse("https://example.com:0/").has_value(),
         "zero port rejected");
  expect(!Url::parse("https://example.com:65536/").has_value(),
         "port above 65535 rejected");
  expect(!Url::parse("https://example.com:12ab/").has_value(),
         "non-numeric port rejected");
  expect(!Url::parse("https://example.com:123456/").has_value(),
         "long port rejected");
  auto ipv6 = Url::parse("https://[::1]:8443/x");
  expect(ipv6.has_value(), "ipv6 with port parses");
  if (ipv6) {
    expect_eq(ipv6.value().host, std::string("[::1]"), "ipv6 host");
    expect(ipv6.value().port.value_or(0) == 8443, "ipv6 port");
  }
  expect(!Url::parse("https://[::1]:0/x").has_value(),
         "ipv6 zero port rejected");

  expect(valid_https_url("https://example.com/x"), "valid https url");
  expect(!valid_https_url("http://example.com/x"), "http rejected");
  expect(!valid_https_url("https://user@example.com/"), "userinfo rejected");
}

void test_bag() {
  using namespace ipashelf;
  auto bag = Bag::parse(make_bag());
  expect(bag.has_value(), "bag parses");
  if (!bag) return;
  expect_eq(bag.value().sap_version, 200, "bag SAP version");
  expect(bag.value().auth_endpoint.find("authenticate") != std::string::npos,
         "bag auth endpoint");

  auto normalised = normalize_auth_endpoint(
      "https://buy.itunes.apple.com/WebObjects/MZFinance.woa/wa/authenticate");
  expect(normalised.has_value(), "auth endpoint normalises");
  if (normalised) {
    expect(normalised.value().back() == '/', "auth endpoint gains slash");
  }

  PlistValue hostile = make_bag();
  hostile.set("authenticateAccount",
              PlistValue::string("https://evil.example.com/auth"));
  auto rejected = Bag::parse(hostile);
  expect(!rejected.has_value(), "foreign auth host rejected");

  PlistValue unsafe = make_bag();
  unsafe.set("redownloadProduct",
             PlistValue::string("https://downloaddispatch.itunes.apple.com/r/redownload?x=1"));
  expect(!Bag::parse(unsafe).has_value(), "redownload with query rejected");
}

void test_login_success() {
  using namespace ipashelf;
  LoginFixture fixture;
  ipashelf::HttpHeaders headers;
  headers.set("X-Set-Apple-Store-Front", "143441-1,29");
  headers.set("pod", "25");
  auto auth = make_auth_reply();
  fixture.transport.enqueue_plist(auth, 200, headers);

  LoginRequest request;
  request.email = "user@example.com";
  request.password = "secret";
  request.auth_code = "123456";
  request.region = "JP";
  request.hardware = fixture.hardware;

  CancellationSource source;
  auto result = fixture.client->login(request, source.token());
  expect(result.has_value(), "login succeeds");
  if (!result) {
    std::cerr << "  login error: " << result.error().message << "\n";
    return;
  }
  expect(!result.value().two_factor_required, "login is not 2FA");
  expect_eq(result.value().account.password_token, std::string("password-token"),
            "password token stored");
  expect_eq(result.value().account.directory_services_id,
            std::string("1234567890"), "DSID stored");
  expect_eq(result.value().account.storefront, std::string("143441-1,29"),
            "storefront stored");
  expect_eq(result.value().account.pod, std::string("25"), "pod stored");
  expect_eq(result.value().account.region, std::string("jp"), "region lowered");
  expect_eq(result.value().account.name, std::string("Ada Lovelace"), "name");
  expect_eq(result.value().account.device_id, fixture.hardware.guid(), "guid");

  expect_eq(fixture.transport.requests.size(), std::size_t{4},
            "four requests issued");
  if (fixture.transport.requests.size() != 4) return;
  const auto& bag_request = fixture.transport.requests[0];
  expect_eq(bag_request.method, std::string("GET"), "bag is a GET");
  expect(bag_request.url.find("init.itunes.apple.com/bag.xml?guid=") !=
             std::string::npos,
         "bag URL");
  expect_eq(fixture.transport.requests[1].method, std::string("GET"),
            "certificate is a GET");
  expect_eq(fixture.transport.requests[2].method, std::string("POST"),
            "SAP setup is a POST");

  const auto& login_request = fixture.transport.requests[3];
  expect(login_request.headers.contains("X-Apple-ActionSignature"),
         "login request is signed");
  const std::string body = text_of(login_request.body);
  expect(body.find("appleId=user%40example.com") != std::string::npos,
         "login body apple id");
  expect(body.find("password=secret123456") != std::string::npos,
         "login body appends verification code");
  expect(body.find("attempt=1") != std::string::npos, "login body attempt");
  expect(body.find("why=signIn") != std::string::npos, "login body why");
  expect_eq(fixture.sap->sign_calls, 1, "SAP signed once");
  expect(!fixture.sap->initialized_with.empty(), "SAP received hardware block");
  expect_eq(fixture.sap->teardown_calls, 1, "SAP session torn down");
}

void test_login_two_factor() {
  using namespace ipashelf;
  LoginFixture fixture;
  PlistValue challenge = PlistValue::dictionary();
  challenge.set("customerMessage",
                PlistValue::string("MZFinance.BadLogin.Configurator_message"));
  fixture.transport.enqueue_plist(challenge);

  LoginRequest request;
  request.email = "user@example.com";
  request.password = "secret";
  request.hardware = fixture.hardware;

  CancellationSource source;
  auto result = fixture.client->login(request, source.token());
  expect(result.has_value(), "2FA probe returns a result");
  if (result) {
    expect(result.value().two_factor_required, "2FA is requested");
  }
  expect_eq(fixture.sap->teardown_calls, 1, "session closed after 2FA probe");
}

void test_login_bad_password() {
  using namespace ipashelf;
  LoginFixture fixture;
  PlistValue failure = PlistValue::dictionary();
  failure.set("failureType", PlistValue::string("-5000"));
  failure.set("customerMessage",
              PlistValue::string("MZFinance.BadLogin.Configurator_message"));
  fixture.transport.enqueue_plist(failure);
  fixture.transport.enqueue_plist(failure);

  LoginRequest request;
  request.email = "user@example.com";
  request.password = "wrong";
  request.auth_code = "000000";
  request.hardware = fixture.hardware;

  CancellationSource source;
  auto result = fixture.client->login(request, source.token());
  expect(!result.has_value(), "bad password fails");
  if (!result) {
    expect(result.error().code == ErrorCode::kUnauthorized ||
               result.error().code == ErrorCode::kParse,
           "bad password is an authentication error");
  }
}

void test_search_and_lookup() {
  using namespace ipashelf;
  FakeTransport transport;
  StoreConfiguration configuration;
  configuration.sleep = [](std::chrono::milliseconds) {};
  StoreClient client(transport, configuration);
  CancellationSource source;

  const std::string body =
      "{\"resultCount\":1,\"results\":[{\"trackId\":123456789,"
      "\"bundleId\":\"com.example.sample\",\"trackName\":\"Sample\","
      "\"version\":\"3.1.4\",\"price\":0,\"sellerName\":\"Example\","
      "\"kind\":\"software\",\"minimumOsVersion\":\"18.0\","
      "\"supportedDevices\":[\"iPhone17,1\",\"iPad16,3\"]}]}";
  transport.enqueue_response(200, body);
  auto results = client.search("sample app", "JP", Platform::kIPhone, 10,
                               source.token());
  expect(results.has_value(), "search succeeds");
  if (results) {
    expect_eq(results.value().count, 1, "search count");
    expect_eq(results.value().results.size(), std::size_t{1}, "search results");
    expect_eq(results.value().results.front().name, std::string("Sample"),
              "search name");
    expect_eq(results.value().results.front().bundle_id,
              std::string("com.example.sample"), "search bundle");
    expect(std::find(results.value().results.front().platforms.begin(),
                     results.value().results.front().platforms.end(),
                     Platform::kIPad) !=
               results.value().results.front().platforms.end(),
           "iPad platform inferred");
  }
  expect(transport.requests.size() == 1, "one search request");
  if (!transport.requests.empty()) {
    const std::string& url = transport.requests.front().url;
    expect(url.find("itunes.apple.com/search?") == 0 ||
               url.find("https://itunes.apple.com/search?") == 0,
           "search URL");
    expect(url.find("country=jp") != std::string::npos, "search country");
    expect(url.find("entity=software") != std::string::npos, "search entity");
  }

  transport.enqueue_response(200, "{\"resultCount\":0,\"results\":[]}");
  auto missing = client.lookup(0, "com.example.missing", "us", Platform::kIPhone,
                               source.token());
  expect(!missing.has_value(), "lookup fails when empty");
  if (!missing) {
    expect(missing.error().code == ErrorCode::kAppNotFound, "lookup not found");
  }
}

void test_latest_external_version() {
  using namespace ipashelf;
  FakeTransport transport;
  StoreConfiguration configuration;
  configuration.sleep = [](std::chrono::milliseconds) {};
  StoreClient client(transport, configuration);
  CancellationSource source;

  App app;
  app.id = 123456789;
  app.bundle_id = "com.example.sample";

  transport.enqueue_response(200, catalog_json(app.id, "9001"));
  auto version =
      client.latest_external_version_id(app, "jp", Platform::kIPhone,
                                        source.token());
  expect(version.has_value(), "catalog version resolves");
  if (version) expect_eq(version.value(), std::string("9001"), "version id");
  expect(!transport.requests.empty() &&
             transport.requests.front().url.find(
                 "apps.apple.com/api/apps/v1/catalog/jp/apps/123456789") !=
                 std::string::npos,
         "catalog URL");

  transport.enqueue_response(200, catalog_json(app.id, "9001", false));
  auto wrong_family =
      client.latest_external_version_id(app, "jp", Platform::kIPad,
                                        source.token());
  expect(!wrong_family.has_value(), "missing device family is rejected");
  if (!wrong_family) {
    expect(wrong_family.error().code == ErrorCode::kAppNotFound,
           "missing family is not found");
  }
}

void test_download_offer() {
  using namespace ipashelf;
  FakeTransport transport;
  StoreConfiguration configuration;
  configuration.sleep = [](std::chrono::milliseconds) {};
  StoreClient client(transport, configuration);
  CancellationSource source;

  Account account;
  account.directory_services_id = "1234567890";
  account.device_id = "A1B2C3D4E5F6";
  account.region = "us";

  App app;
  app.id = 123456789;
  app.bundle_id = "com.example.sample";

  transport.enqueue_plist(make_bag());
  transport.enqueue_plist(make_download_reply());

  auto offer = client.download_offer(account, app, "9001", Platform::kIPhone,
                                     source.token());
  expect(offer.has_value(), "download offer succeeds");
  if (offer) {
    expect_eq(offer.value().url,
              std::string("https://iosapps.itunes.apple.com/package.ipa"),
              "package URL");
    expect_eq(offer.value().md5,
              std::string("d41d8cd98f00b204e9800998ecf8427e"), "md5 lowered");
    expect_eq(offer.value().sinfs.size(), std::size_t{1}, "sinf count");
    expect_eq(offer.value().display_version, std::string("3.1.4"),
              "display version");
    expect_eq(offer.value().external_version_id, std::string("9001"),
              "external version");
  }
  expect(transport.requests.size() >= 2, "bag then download");
  if (transport.requests.size() >= 2) {
    const auto& download = transport.requests[1];
    expect_eq(download.method, std::string("POST"), "download is a POST");
    expect(download.url.find("volumeStoreDownloadProduct?guid=") !=
               std::string::npos,
           "volume store URL");
    expect_eq(download.headers.get_or("iCloud-DSID"), std::string("1234567890"),
              "DSID header");
    const std::string body = text_of(download.body);
    expect(body.find("externalVersionId") != std::string::npos,
           "version pinned in payload");
  }
}

void test_download_fallback() {
  using namespace ipashelf;
  FakeTransport transport;
  StoreConfiguration configuration;
  configuration.sleep = [](std::chrono::milliseconds) {};
  StoreClient client(transport, configuration);
  CancellationSource source;

  Account account;
  account.directory_services_id = "1";
  account.device_id = "AABBCCDDEEFF";
  account.region = "us";

  App app;
  app.id = 42;
  app.bundle_id = "com.example.sample";

  PlistValue empty = PlistValue::dictionary();
  empty.set("songList", PlistValue::array());

  transport.enqueue_plist(make_bag());
  transport.enqueue_plist(empty);
  transport.enqueue_plist(make_download_reply());

  auto offer = client.download_offer(account, app, "9001", Platform::kIPhone,
                                     source.token());
  expect(offer.has_value(), "redownload fallback succeeds");
  if (offer) {
    expect_eq(offer.value().external_version_id, std::string("9001"),
              "fallback version");
  }
  expect_eq(transport.requests.size(), std::size_t{3}, "three requests");
  if (transport.requests.size() == 3) {
    expect(transport.requests[2].url.find(
               "downloaddispatch.itunes.apple.com/r/redownload") !=
               std::string::npos,
           "redownload endpoint");
  }
}

void test_purchase() {
  using namespace ipashelf;
  FakeTransport transport;
  StoreConfiguration configuration;
  configuration.sleep = [](std::chrono::milliseconds) {};
  StoreClient client(transport, configuration);
  CancellationSource source;

  Account account;
  account.directory_services_id = "1234567890";
  account.password_token = "password-token";
  account.storefront = "143441-1,29";
  account.device_id = "A1B2C3D4E5F6";
  account.pod = "25";

  App app;
  app.id = 123456789;

  PlistValue success = PlistValue::dictionary();
  success.set("jingleDocType", PlistValue::string("purchaseSuccess"));
  success.set("status", PlistValue::integer(0));
  transport.enqueue_plist(success);

  auto status = client.purchase(account, app, Platform::kIPhone, source.token());
  expect(status.ok(), "purchase succeeds");
  expect_eq(transport.requests.size(), std::size_t{1}, "one purchase request");
  if (transport.requests.empty()) return;
  const auto& request = transport.requests.front();
  expect(request.url.find("p25-buy.itunes.apple.com/WebObjects/MZFinance.woa/wa/"
                          "buyProduct") != std::string::npos,
         "purchase URL uses pod host");
  expect_eq(request.headers.get_or("X-Token"), std::string("password-token"),
            "token header");
  expect_eq(request.headers.get_or("X-Apple-Store-Front"),
            std::string("143441-1,29"), "storefront header");
  const std::string body = text_of(request.body);
  expect(body.find("salableAdamId") != std::string::npos, "purchase app id");
  expect(body.find("pricingParameters") != std::string::npos,
         "purchase pricing");
  expect(body.find("<integer>123456789</integer>") != std::string::npos,
         "purchase id value");
}

void test_purchase_license_exists() {
  using namespace ipashelf;
  FakeTransport transport;
  StoreConfiguration configuration;
  configuration.sleep = [](std::chrono::milliseconds) {};
  StoreClient client(transport, configuration);
  CancellationSource source;

  Account account;
  account.directory_services_id = "1";
  account.password_token = "token";
  account.storefront = "143441-1,29";
  account.device_id = "AABBCCDDEEFF";

  App app;
  app.id = 5;

  PlistValue failure = PlistValue::dictionary();
  failure.set("failureType", PlistValue::string("5002"));
  transport.enqueue_plist(failure);
  expect(client.purchase(account, app, Platform::kIPhone, source.token()).ok(),
         "existing license is treated as success");
}

void test_version_identifiers() {
  using namespace ipashelf;
  FakeTransport transport;
  StoreConfiguration configuration;
  configuration.sleep = [](std::chrono::milliseconds) {};
  StoreClient client(transport, configuration);
  CancellationSource source;

  Account account;
  account.directory_services_id = "1";
  account.device_id = "AABBCCDDEEFF";
  account.region = "us";
  App app;
  app.id = 7;
  app.bundle_id = "com.example.sample";

  transport.enqueue_plist(make_bag());
  transport.enqueue_plist(make_download_reply());
  auto versions =
      client.version_identifiers(account, app, Platform::kIPhone, source.token());
  expect(versions.has_value(), "version identifiers resolve");
  if (versions) {
    expect(versions.value().size() == 2, "two versions");
    expect_eq(versions.value().front(), std::string("9001"), "latest first");
  }
}

void test_download_parsing() {
  using namespace ipashelf;
  auto offer = parse_download_response(make_download_reply());
  expect(offer.has_value(), "download response parses");
  if (offer) {
    expect_eq(offer.value().bundle_id, std::string("com.example.sample"),
              "parsed bundle id");
    expect_eq(offer.value().metadata.size(), std::size_t{4}, "metadata keys");
  }

  PlistValue failure = PlistValue::dictionary();
  failure.set("failureType", PlistValue::string("9610"));
  auto missing = parse_download_response(failure);
  expect(!missing.has_value(), "license failure surfaces");
  if (!missing) {
    expect(missing.error().code == ErrorCode::kLicenseRequired,
           "license failure code");
  }
}

void test_account_json() {
  using namespace ipashelf;
  Account account;
  account.email = "user@example.com";
  account.name = "Ada Lovelace";
  account.password = "secret";
  account.password_token = "token";
  account.directory_services_id = "1234567890";
  account.storefront = "143441-1,29";
  account.pod = "25";
  account.region = "us";
  account.device_id = "A1B2C3D4E5F6";

  const std::string json = accounts_to_json({account});
  expect(json.find("user@example.com") != std::string::npos, "account JSON");
  auto parsed = accounts_from_json(json);
  expect(parsed.has_value(), "account list parses");
  if (!parsed) return;
  expect(parsed.value().size() == 1, "one account");
  if (parsed.value().empty()) return;
  const Account& round = parsed.value().front();
  expect_eq(round.email, account.email, "account email round trip");
  expect_eq(round.password_token, account.password_token, "account token");
  expect_eq(round.directory_services_id, account.directory_services_id, "account dsid");
  expect_eq(round.storefront, account.storefront, "account storefront");
  expect_eq(round.pod, account.pod, "account pod");
  expect_eq(round.region, account.region, "account region");
  expect_eq(round.device_id, account.device_id, "account device id");

  auto broken = accounts_from_json("{}");
  expect(!broken.has_value(), "non-array account list rejected");
}

void test_ipa_patch(const std::string& fixtures) {
  using namespace ipashelf;
  const std::string source = fixtures + "/sample.ipa";
  auto info = inspect_ipa(source);
  expect(info.has_value(), "IPA inspects");
  if (!info) {
    std::cerr << "  inspect error: " << info.error().message << "\n";
    return;
  }
  expect_eq(info.value().bundle_id, std::string("com.example.sample"),
            "IPA bundle id");
  expect_eq(info.value().display_version, std::string("3.1.4"),
            "IPA display version");
  expect_eq(info.value().executable, std::string("Sample"), "IPA executable");
  expect_eq(info.value().app_directory, std::string("Payload/Sample.app/"),
            "IPA app directory");
  expect(info.value().supported_platforms.size() == 1 &&
             info.value().supported_platforms.front() == "iPhoneOS",
         "IPA platforms");
  expect(info.value().file_size > 0, "IPA file size");
  expect_eq(default_sinf_path(info.value()),
            std::string("Payload/Sample.app/SC_Info/Sample.sinf"),
            "default sinf path");

  App app;
  app.id = 123456789;
  app.bundle_id = "com.example.sample";
  app.name = "Sample";

  DownloadOffer offer;
  offer.bundle_id = "com.example.sample";
  offer.display_version = "3.1.4";
  offer.external_version_id = "9001";
  PlistValue metadata = build_itunes_metadata(app, offer, [] {
    Account account;
    account.email = "user@example.com";
    return account;
  }(), default_sinf_path(info.value()));
  expect_eq(metadata.string_or("apple-id"), std::string("user@example.com"),
            "metadata apple id");
  expect_eq(metadata.string_or("userName"), std::string("user@example.com"),
            "metadata user name");
  expect_eq(metadata.integer_for("itemId").value_or(0), 123456789,
            "metadata item id");
  const PlistValue* paths = metadata.find("SinfPaths");
  expect(paths != nullptr && paths->is_array() && paths->array_items().size() == 1,
         "metadata sinf paths");

  IpaPatchOptions options;
  options.source_path = source;
  options.destination_path = fixtures + "/patched.ipa";
  options.metadata = metadata;
  options.artwork = bytes_of("PNG-ARTWORK");
  options.sinf = bytes_of("patched-sinf");
  options.sinf_path = default_sinf_path(info.value());

  CancellationSource source_token;
  const Status status = write_patched_ipa(options, source_token.token());
  expect(status.ok(), "IPA patches");
  if (!status) {
    std::cerr << "  patch error: " << status.error().message << "\n";
    return;
  }

  mz_zip_archive archive{};
  expect(mz_zip_reader_init_file(&archive, options.destination_path.c_str(), 0),
         "patched IPA opens");
  if (mz_zip_get_last_error(&archive) != MZ_ZIP_NO_ERROR &&
      mz_zip_reader_get_num_files(&archive) == 0) {
    return;
  }
  const auto find_entry = [&](std::string_view name) -> int {
    const mz_uint count = mz_zip_reader_get_num_files(&archive);
    for (mz_uint index = 0; index < count; ++index) {
      mz_zip_archive_file_stat stat{};
      if (mz_zip_reader_file_stat(&archive, index, &stat) &&
          name == stat.m_filename) {
        return static_cast<int>(index);
      }
    }
    return -1;
  };
  const auto read_entry = [&](int index) -> Bytes {
    if (index < 0) return {};
    mz_zip_archive_file_stat stat{};
    if (!mz_zip_reader_file_stat(&archive, static_cast<mz_uint>(index), &stat)) {
      return {};
    }
    Bytes buffer(static_cast<std::size_t>(stat.m_uncomp_size));
    if (!mz_zip_reader_extract_to_mem(&archive, static_cast<mz_uint>(index),
                                      buffer.data(), buffer.size(), 0)) {
      return {};
    }
    return buffer;
  };

  const int metadata_index = find_entry("iTunesMetadata.plist");
  expect(metadata_index >= 0, "metadata entry exists");
  auto written = PlistValue::parse(read_entry(metadata_index));
  expect(written.has_value(), "written metadata parses");
  if (written) {
    expect_eq(written.value().string_or("apple-id"),
              std::string("user@example.com"), "written metadata apple id");
  }
  expect_eq(text_of(read_entry(find_entry("iTunesArtwork"))),
            std::string("PNG-ARTWORK"), "artwork written");
  expect_eq(text_of(read_entry(find_entry(options.sinf_path))),
            std::string("patched-sinf"), "sinf replaced");
  expect_eq(text_of(read_entry(find_entry("Payload/Sample.app/Sample"))),
            std::string("MACH-O-PAYLOAD"), "binary preserved");
  expect(find_entry("iTunesMetadata.plist") >= 0, "metadata at root");
  mz_zip_reader_end(&archive);

  std::ofstream marker(fixtures + "/patched.ready");
  marker << "ok";
}

void test_map_store_failure() {
  using namespace ipashelf;
  expect(map_store_failure("-5000", "", false).error().code ==
             ErrorCode::kUnauthorized,
         "-5000 unauthorized");
  expect(map_store_failure("2034", "", false).error().code ==
             ErrorCode::kTokenExpired,
         "2034 token expired");
  expect(map_store_failure("9610", "", false).error().code ==
             ErrorCode::kLicenseRequired,
         "9610 license required");
  expect(map_store_failure("2059", "", false).error().code ==
             ErrorCode::kTemporarilyUnavailable,
         "2059 unavailable");
  expect(map_store_failure("5002", "", true).ok(), "5002 purchase is success");
  expect(map_store_failure("", "", false).ok(), "empty failure is success");
}

}  // namespace


// ---------------------------------------------------------------------------
// Storefronts, rich catalog data, filters and reviews
// ---------------------------------------------------------------------------

void test_storefronts() {
  using namespace ipashelf;
  expect(storefronts().size() >= 40, "storefront table is populated");
  auto us = storefront_for_region("us");
  expect(us.has_value(), "US storefront exists");
  if (us) {
    expect_eq(us->storefront_id, std::string("143441"), "US storefront id");
    expect_eq(storefront_header(*us), std::string("143441-1,29"),
              "storefront header");
    expect_eq(us->currency, std::string("USD"), "US currency");
  }
  auto jp = storefront_for_region(" JP ");
  expect(jp.has_value(), "region lookup trims and folds case");
  if (jp) expect_eq(jp->name, std::string("Japan"), "JP name");
  expect(!storefront_for_region("ZZ").has_value(), "unknown region rejected");
  Storefront empty;
  expect(storefront_header(empty).empty(), "empty storefront header");
}

const char* kRichAppJson =
    "{\"resultCount\":1,\"results\":[{"
    "\"trackId\":123456789,\"bundleId\":\"com.example.sample\","
    "\"trackName\":\"Sample\",\"version\":\"4.2.0\","
    "\"artistName\":\"Example Studio\",\"sellerName\":\"Example Inc\","
    "\"sellerUrl\":\"https://example.com\","
    "\"artworkUrl512\":\"https://example.com/a.png\","
    "\"description\":\"A sample app\",\"releaseNotes\":\"Fixes\","
    "\"releaseDate\":\"2026-09-01T00:00:00Z\","
    "\"currentVersionReleaseDate\":\"2026-09-02T00:00:00Z\","
    "\"minimumOsVersion\":\"18.0\",\"kind\":\"software\",\"price\":0,"
    "\"currency\":\"JPY\",\"primaryGenreName\":\"Utilities\","
    "\"primaryGenreId\":6002,\"contentAdvisoryRating\":\"4+\","
    "\"trackViewUrl\":\"https://apps.apple.com/jp/app/id123456789\","
    "\"averageUserRating\":4.6,\"userRatingCount\":12345,"
    "\"averageUserRatingForCurrentVersion\":4.4,"
    "\"userRatingCountForCurrentVersion\":999,"
    "\"fileSizeBytes\":\"52428800\","
    "\"languageCodesISO2A\":[\"EN\",\"JA\"],"
    "\"screenshotUrls\":[\"https://example.com/1.png\",\"https://example.com/2.png\"],"
    "\"ipadScreenshotUrls\":[\"https://example.com/ipad1.png\"],"
    "\"supportedDevices\":[\"iPhone17,1\",\"iPad16,3\"],"
    "\"features\":[\"iosUniversal\"],\"hasInAppPurchases\":true}]}";

void test_app_details() {
  using namespace ipashelf;
  FakeTransport transport;
  StoreConfiguration configuration;
  configuration.sleep = [](std::chrono::milliseconds) {};
  StoreClient client(transport, configuration);
  CancellationSource source;

  transport.enqueue_response(200, kRichAppJson);
  auto result = client.lookup(123456789, "", "jp", Platform::kIPhone,
                              source.token());
  expect(result.has_value(), "rich lookup succeeds");
  if (!result) return;
  const App& app = result.value();
  expect_eq(app.artist_name, std::string("Example Studio"), "artist name");
  expect_eq(app.seller, std::string("Example Inc"), "seller name");
  expect_eq(app.primary_genre, std::string("Utilities"), "primary genre");
  expect_eq(app.genre_id, std::int64_t{6002}, "genre id");
  expect_eq(app.content_advisory_rating, std::string("4+"), "advisory rating");
  expect_eq(app.currency, std::string("JPY"), "currency");
  expect_eq(app.track_view_url,
            std::string("https://apps.apple.com/jp/app/id123456789"),
            "track view url");
  expect_eq(app.language_codes, std::string("EN,JA"), "language codes");
  expect_eq(app.languages.size(), std::size_t{2}, "language count");
  expect_eq(app.screenshot_urls.size(), std::size_t{2}, "screenshot count");
  expect_eq(app.ipad_screenshot_urls.size(), std::size_t{1}, "iPad shots");
  expect_eq(app.supported_devices.size(), std::size_t{2}, "supported devices");
  expect_eq(app.average_rating, 4.6, "average rating");
  expect_eq(app.rating_count, std::int64_t{12345}, "rating count");
  expect_eq(app.current_version_average_rating, 4.4, "current rating");
  expect_eq(app.current_version_rating_count, std::int64_t{999},
            "current rating count");
  expect_eq(app.byte_size, std::int64_t{52428800}, "byte size");
  expect(app.is_universal, "universal app");
  expect(app.has_arcade_offer, "in-app purchases flag");
  expect_eq(app.current_version_release_date,
            std::string("2026-09-02T00:00:00Z"), "current release date");
  expect(std::find(app.platforms.begin(), app.platforms.end(),
                   Platform::kIPad) != app.platforms.end(),
         "iPad family parsed");
}

void test_search_filters() {
  using namespace ipashelf;
  FakeTransport transport;
  StoreConfiguration configuration;
  configuration.sleep = [](std::chrono::milliseconds) {};
  StoreClient client(transport, configuration);
  CancellationSource source;

  const std::string body =
      "{\"resultCount\":3,\"results\":["
      "{\"trackId\":1,\"bundleId\":\"a.one\",\"trackName\":\"Zeta\","
      "\"price\":0,\"averageUserRating\":4.1,\"userRatingCount\":10,"
      "\"supportedDevices\":[\"iPhone17,1\"]},"
      "{\"trackId\":2,\"bundleId\":\"a.two\",\"trackName\":\"Alpha\","
      "\"price\":0,\"averageUserRating\":4.9,\"userRatingCount\":90,"
      "\"supportedDevices\":[\"iPhone17,1\"]},"
      "{\"trackId\":3,\"bundleId\":\"a.three\",\"trackName\":\"Paid\","
      "\"price\":3.99,\"averageUserRating\":5,\"userRatingCount\":500,"
      "\"supportedDevices\":[\"iPhone17,1\"]}]}";
  transport.enqueue_response(200, body);

  SearchQuery query;
  query.term = " sample ";
  query.region = "JP";
  query.platform = Platform::kIPhone;
  query.limit = 15;
  query.genre_id = 6002;
  query.free_only = true;
  query.sort = "rating";

  auto results = client.search(query, source.token());
  expect(results.has_value(), "filtered search succeeds");
  if (!results) return;
  expect_eq(results.value().results.size(), std::size_t{2},
            "paid result filtered out");
  expect_eq(results.value().count, 2, "filtered count");
  expect_eq(results.value().results.front().name, std::string("Alpha"),
            "sorted by rating");
  expect(!transport.requests.empty(), "search request captured");
  if (!transport.requests.empty()) {
    const std::string& url = transport.requests.front().url;
    expect(url.find("genreId=6002") != std::string::npos, "genre filter in URL");
    expect(url.find("country=jp") != std::string::npos, "country in URL");
    expect(url.find("term=sample") != std::string::npos, "term trimmed");
    expect(url.find("limit=15") != std::string::npos, "limit in URL");
  }

  SearchQuery by_name = query;
  by_name.free_only = std::nullopt;
  by_name.sort = "name";
  transport.enqueue_response(200, body);
  auto sorted = client.search(by_name, source.token());
  expect(sorted.has_value(), "name sorted search succeeds");
  if (sorted) {
    expect_eq(sorted.value().results.front().name, std::string("Alpha"),
              "name sort first");
    expect_eq(sorted.value().results.back().name, std::string("Zeta"),
              "name sort last");
  }
}

void test_reviews() {
  using namespace ipashelf;
  const std::string feed =
      "{\"feed\":{\"link\":[{\"attributes\":{\"rel\":\"next\","
      "\"href\":\"https://itunes.apple.com/jp/rss/customerreviews/page=2/"
      "id=123456789/sortby=mostrecent/json\"}}],\"entry\":["
      "{\"id\":{\"label\":\"1\"},\"author\":{\"name\":{\"label\":\"Aki\"}},"
      "\"im:rating\":{\"label\":\"5\"},\"im:version\":{\"label\":\"4.2.0\"},"
      "\"title\":{\"label\":\"Great\"},\"content\":{\"label\":\"Works well\"},"
      "\"updated\":{\"label\":\"2026-09-10T00:00:00-07:00\"}},"
      "{\"id\":{\"label\":\"2\"},\"author\":{\"name\":{\"label\":\"Bo\"}},"
      "\"im:rating\":{\"label\":\"3\"},\"im:version\":{\"label\":\"4.1.0\"},"
      "\"title\":{\"label\":\"Okay\"},\"content\":{\"label\":\"Fine\"},"
      "\"updated\":{\"label\":\"2026-09-09T00:00:00-07:00\"}}]}}";
  auto page = parse_reviews_document(feed, 1);
  expect(page.has_value(), "review feed parses");
  if (page) {
    expect_eq(page.value().reviews.size(), std::size_t{2}, "two reviews");
    expect(page.value().has_more, "next page detected");
    expect_eq(page.value().reviews.front().author, std::string("Aki"),
              "review author");
    expect_eq(page.value().reviews.front().rating, 5, "review rating");
    expect_eq(page.value().reviews.front().version, std::string("4.2.0"),
              "review version");
    expect_eq(page.value().reviews.front().title, std::string("Great"),
              "review title");
  }

  const std::string single =
      "{\"feed\":{\"entry\":{\"id\":{\"label\":\"9\"},\"author\":{\"name\":"
      "{\"label\":\"Solo\"}},\"im:rating\":{\"label\":\"4\"},\"title\":"
      "{\"label\":\"One\"},\"content\":{\"label\":\"Only review\"}}}}";
  auto one = parse_reviews_document(single);
  expect(one.has_value(), "single review object parses");
  if (one) {
    expect_eq(one.value().reviews.size(), std::size_t{1}, "single review");
    expect(!one.value().has_more, "no next page");
  }

  expect(!parse_reviews_document("{}").has_value(), "missing feed rejected");
  expect(!parse_reviews_document("not json").has_value(), "bad JSON rejected");

  FakeTransport transport;
  StoreConfiguration configuration;
  configuration.sleep = [](std::chrono::milliseconds) {};
  StoreClient client(transport, configuration);
  CancellationSource source;
  transport.enqueue_response(200, feed);
  auto live = client.reviews(123456789, "JP", 1, source.token());
  expect(live.has_value(), "reviews request succeeds");
  if (live) expect_eq(live.value().reviews.size(), std::size_t{2}, "reviews");
  expect(!transport.requests.empty(), "review request captured");
  if (!transport.requests.empty()) {
    const std::string& url = transport.requests.front().url;
    expect(url.find("/jp/rss/customerreviews/page=1/id=123456789/") !=
               std::string::npos,
           "review URL path");
    expect(url.find("sortby=mostrecent/json") != std::string::npos,
           "review URL sort");
  }
  transport.enqueue_response(404, "");
  auto missing = client.reviews(123456789, "jp", 1, source.token());
  expect(!missing.has_value(), "missing review feed is an error");
  if (!missing) {
    expect(missing.error().code == ErrorCode::kParse, "missing feed code");
  }
}

// ---------------------------------------------------------------------------
// OTA manifest and install page
// ---------------------------------------------------------------------------

void test_ota_manifest(const std::string& fixtures) {
  using namespace ipashelf;
  OtaManifest manifest;
  manifest.bundle_id = "com.example.sample";
  manifest.display_version = "4.2.0";
  manifest.title = "Sample <&>";
  manifest.payload_url = "https://192.168.1.5:8443/payload.ipa";
  manifest.small_icon_url = "https://192.168.1.5:8443/icon57.png";
  manifest.large_icon_url = "https://192.168.1.5:8443/icon512.png";

  const PlistValue document = build_install_manifest(manifest);
  const PlistValue* items = document.find("items");
  expect(items != nullptr && items->is_array(), "manifest items array");
  if (items != nullptr && items->is_array() && !items->array_items().empty()) {
    const PlistValue& item = items->array_items().front();
    const PlistValue* assets = item.find("assets");
    expect(assets != nullptr && assets->array_items().size() == 3,
           "three assets");
    if (assets != nullptr && !assets->array_items().empty()) {
      expect_eq(assets->array_items().front().string_or("kind"),
                std::string("software-package"), "software-package kind");
      expect_eq(assets->array_items().front().string_or("url"),
                manifest.payload_url, "payload url");
    }
    const PlistValue* metadata = item.find("metadata");
    expect(metadata != nullptr, "metadata present");
    if (metadata != nullptr) {
      expect_eq(metadata->string_or("bundle-identifier"),
                std::string("com.example.sample"), "metadata bundle id");
      expect_eq(metadata->string_or("bundle-version"), std::string("4.2.0"),
                "metadata version");
      expect_eq(metadata->string_or("kind"), std::string("software"),
                "metadata kind");
      expect_eq(metadata->string_or("title"), std::string("Sample <&>"),
                "metadata title");
    }
  }

  const std::string xml = build_install_manifest_xml(manifest);
  expect(xml.find("<?xml") != std::string::npos, "manifest XML header");
  expect(xml.find("<key>bundle-identifier</key>") != std::string::npos,
         "manifest XML key");
  std::ofstream(fixtures + "/install.plist", std::ios::binary) << xml;

  const std::string install =
      build_itms_services_url("https://192.168.1.5:8443/install.plist");
  expect(install.rfind("itms-services://?action=download-manifest&url=", 0) == 0,
         "itms-services prefix");
  expect(install.find("https%3A%2F%2F192.168.1.5%3A8443%2Finstall.plist") !=
             std::string::npos,
         "manifest url is percent encoded");

  const std::string page = build_install_page(manifest, "https://x/ca.der");
  expect(page.find("Install on this device") != std::string::npos,
         "install page link");
  expect(page.find("Sample &lt;&amp;&gt;") != std::string::npos,
         "install page escapes the title");
  expect(page.find("https://x/ca.der") != std::string::npos,
         "install page certificate link");
  expect(page.find("itms-services://") != std::string::npos,
         "install page uses itms-services");
  const std::string plain = build_install_page(
      "Sample", "com.example.sample", "4.2.0",
      "itms-services://?action=download-manifest&url=https%3A%2F%2Fx%2Fm.plist",
      "http://127.0.0.1:9000/ca.der");
  expect(plain.find("http://127.0.0.1:9000/ca.der") != std::string::npos,
         "plain page certificate link");
  expect(plain.find("https%3A%2F%2Fx%2Fm.plist") != std::string::npos,
         "plain page install link");
}

// ---------------------------------------------------------------------------
// HTTP parsing, ranges and routing
// ---------------------------------------------------------------------------

void test_http_parser() {
  using namespace ipashelf;
  HttpRequestStream stream;
  stream.append(
      "GET /install.plist?x=1 HTTP/1.1\r\nHost: 192.168.1.5:8443\r\n"
      "User-Agent: curl/8\r\n\r\n");
  auto first = stream.next();
  expect(first.has_value() && first->has_value(), "request parses");
  if (first.has_value() && first->has_value()) {
    const HttpServerRequest& request = first->value();
    expect_eq(request.method, std::string("GET"), "method");
    expect_eq(request.path, std::string("/install.plist"), "path");
    expect_eq(request.query, std::string("x=1"), "query");
    expect_eq(request.header("host"), std::string("192.168.1.5:8443"),
              "case-insensitive header");
    expect(request.keep_alive, "HTTP/1.1 keep-alive");
  }

  HttpRequestStream partial;
  partial.append("GET /ping HTTP/1.1\r\nHost: h\r\n");
  expect(!partial.next().has_value(), "partial request waits for headers");
  partial.append("\r\n");
  auto completed = partial.next();
  expect(completed.has_value() && completed->has_value(),
         "request completes after headers");

  HttpRequestStream lf;
  lf.append("HEAD /ping HTTP/1.0\n\n");
  auto lf_request = lf.next();
  expect(lf_request.has_value() && lf_request->has_value(), "LF-only request");
  if (lf_request.has_value() && lf_request->has_value()) {
    expect_eq(lf_request->value().method, std::string("HEAD"), "HEAD method");
    expect(!lf_request->value().keep_alive, "HTTP/1.0 defaults to close");
  }

  HttpRequestStream pipelined;
  pipelined.append(
      "GET /a HTTP/1.1\r\nHost: h\r\n\r\nGET /b HTTP/1.1\r\nHost: h\r\n\r\n");
  auto one = pipelined.next();
  auto two = pipelined.next();
  expect(one.has_value() && one->has_value() &&
             one->value().path == std::string("/a"),
         "first pipelined request");
  expect(two.has_value() && two->has_value() &&
             two->value().path == std::string("/b"),
         "second pipelined request");

  HttpRequestStream post;
  post.append("POST / HTTP/1.1\r\nHost: h\r\n\r\n");
  auto rejected = post.next();
  expect(rejected.has_value() && !rejected->has_value(), "POST rejected");
  if (rejected.has_value() && !rejected->has_value()) {
    expect(rejected->error().code == ErrorCode::kUnsupported, "POST error code");
  }

  HttpRequestStream chunked;
  chunked.append("GET / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n");
  auto chunked_result = chunked.next();
  expect(chunked_result.has_value() && !chunked_result->has_value(),
         "chunked rejected");

  HttpRequestStream huge;
  huge.append("GET / HTTP/1.1\r\nX-Fill: ");
  huge.append(std::string(70 * 1024, 'a'));
  auto too_large = huge.next();
  expect(too_large.has_value() && !too_large->has_value(),
         "oversized headers rejected");
}

void test_ranges() {
  using namespace ipashelf;
  auto full = parse_range_header("", 100);
  expect(full.has_value() && !full.value().partial && full.value().length == 100,
         "empty range is the full body");

  auto first = parse_range_header("bytes=0-9", 100);
  expect(first.has_value() && first.value().partial, "byte range parses");
  if (first) {
    expect_eq(first.value().start, std::uint64_t{0}, "range start");
    expect_eq(first.value().length, std::uint64_t{10}, "range length");
  }

  auto open = parse_range_header("bytes=90-", 100);
  expect(open.has_value() && open.value().start == 90 &&
             open.value().length == 10,
         "open-ended range");

  auto suffix = parse_range_header("bytes=-10", 100);
  expect(suffix.has_value() && suffix.value().start == 90 &&
             suffix.value().length == 10,
         "suffix range");

  auto clamped = parse_range_header("bytes=95-1000", 100);
  expect(clamped.has_value() && clamped.value().length == 5,
         "range end is clamped");

  expect(!parse_range_header("bytes=200-", 100).has_value(),
         "range past the end rejected");
  expect(!parse_range_header("bytes=abc", 100).has_value(), "bad range rejected");
  expect(!parse_range_header("bytes=-0", 100).has_value(),
         "empty suffix rejected");

  auto multi = parse_range_header("bytes=0-1,5-6", 100);
  expect(multi.has_value() && !multi.value().partial,
         "multiple ranges fall back to the full body");
}

void test_ota_router(const std::string& fixtures) {
  using namespace ipashelf;
  const std::string payload = fixtures + "/payload.bin";
  {
    std::ofstream file(payload, std::ios::binary);
    for (int index = 0; index < 100; ++index) {
      file << static_cast<char>('0' + (index % 10));
    }
  }
  const std::string icon = fixtures + "/icon.png";
  {
    std::ofstream file(icon, std::ios::binary);
    file << "\x89PNG\r\n\x1a\nfake";
  }
  const std::string certificate = fixtures + "/ca.der";
  {
    std::ofstream file(certificate, std::ios::binary);
    file << "fake-der";
  }

  OtaPackage package;
  package.bundle_id = "com.example.sample";
  package.display_version = "4.2.0";
  package.title = "Sample";
  package.payload_path = payload;
  package.icon_small_path = icon;
  package.icon_large_path = icon;
  package.certificate_path = certificate;
  OtaRouter router("https://127.0.0.1:8443", package);

  HttpServerRequest request;
  request.method = "GET";
  request.path = "/install.plist";
  request.headers.set("Host", "127.0.0.1:8443");
  auto manifest = router.route(request);
  expect_eq(manifest.status, 200, "manifest status");
  expect(manifest.body.find("https://127.0.0.1:8443/payload.ipa") !=
             std::string::npos,
         "manifest payload url");
  expect_eq(manifest.header("Content-Type"), std::string("text/xml; charset=utf-8"),
            "manifest content type");
  const std::string head = manifest.head("Tue, 20 Oct 2026 00:00:00 GMT");
  expect(head.find("Content-Length: " + std::to_string(manifest.body.size())) !=
             std::string::npos,
         "manifest content length");
  expect(head.find("Date: Tue, 20 Oct 2026 00:00:00 GMT") != std::string::npos,
         "date header");

  HttpServerRequest range_request;
  range_request.method = "GET";
  range_request.path = "/payload.ipa";
  range_request.headers.set("Host", "127.0.0.1");
  range_request.headers.set("Range", "bytes=10-19");
  auto ranged = router.route(range_request);
  expect_eq(ranged.status, 206, "range status");
  expect_eq(ranged.file_offset, std::uint64_t{10}, "range offset");
  expect_eq(ranged.file_length, std::uint64_t{10}, "range length");
  expect_eq(ranged.header("Content-Range"),
            std::string("bytes 10-19/100"), "content range header");

  HttpServerRequest bad_range = range_request;
  bad_range.headers.set("Range", "bytes=500-");
  auto rejected = router.route(bad_range);
  expect_eq(rejected.status, 416, "unsatisfiable range status");
  expect_eq(rejected.header("Content-Range"), std::string("bytes */100"),
            "unsatisfiable content range");

  HttpServerRequest head_request = range_request;
  head_request.method = "HEAD";
  head_request.headers.remove("Range");
  auto head_only = router.route(head_request);
  expect_eq(head_only.status, 200, "HEAD status");
  expect(head_only.head_only, "HEAD flag");

  HttpServerRequest wrong_host = request;
  wrong_host.headers.set("Host", "evil.example");
  expect_eq(router.route(wrong_host).status, 421, "host mismatch");

  HttpServerRequest options;
  options.method = "OPTIONS";
  options.path = "/install.plist";
  auto options_response = router.route(options);
  expect_eq(options_response.status, 204, "OPTIONS status");
  expect_eq(options_response.header("Allow"),
            std::string("GET, HEAD, OPTIONS"), "OPTIONS allow");

  HttpServerRequest missing;
  missing.method = "GET";
  missing.path = "/nope";
  expect_eq(router.route(missing).status, 404, "unknown path");

  HttpServerRequest post;
  post.method = "POST";
  post.path = "/payload.ipa";
  expect_eq(router.route(post).status, 405, "POST status");

  expect(router.install_url().rfind("itms-services://", 0) == 0,
         "router install url");
  OtaRouter plain_router = router;
  plain_router.set_page_links("itms-services://?action=download-manifest&url=x",
                              "http://127.0.0.1:9000/ca.der");
  HttpServerRequest page_request;
  page_request.method = "GET";
  page_request.path = "/";
  page_request.headers.set("Host", "127.0.0.1");
  const auto page_response = plain_router.route(page_request);
  expect(page_response.body.find("http://127.0.0.1:9000/ca.der") !=
             std::string::npos,
         "page override certificate link");
  expect(page_response.body.find("url=x") != std::string::npos,
         "page override install link");

  HttpServerRequest certificate_request;
  certificate_request.method = "GET";
  certificate_request.path = "/ca.cer";
  certificate_request.headers.set("Host", "127.0.0.1");
  const auto certificate_response = router.route(certificate_request);
  expect_eq(certificate_response.status, 200, "certificate route");
  expect_eq(certificate_response.header("Content-Type"),
            std::string("application/x-x509-ca-cert"),
            "certificate content type");

  expect_eq(router.manifest_url(),
            std::string("https://127.0.0.1:8443/install.plist"),
            "router manifest url");
}

#if defined(__unix__) || defined(__APPLE__)
std::string read_socket_until_close(int fd) {
  std::string output;
  char buffer[4096];
  while (true) {
    const ssize_t received = ::recv(fd, buffer, sizeof(buffer), 0);
    if (received <= 0) break;
    output.append(buffer, static_cast<std::size_t>(received));
  }
  return output;
}

void test_http_server(const std::string& fixtures) {
  using namespace ipashelf;
  const std::string payload = fixtures + "/payload.bin";
  OtaPackage package;
  package.bundle_id = "com.example.sample";
  package.display_version = "4.2.0";
  package.title = "Sample";
  package.payload_path = payload;
  OtaRouter router("https://127.0.0.1", package);

  const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
  expect(listener >= 0, "socket created");
  if (listener < 0) return;
  int reuse = 1;
  ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  expect(::bind(listener, reinterpret_cast<sockaddr*>(&address),
                sizeof(address)) == 0,
         "server bound");
  expect(::listen(listener, 1) == 0, "server listening");
  socklen_t length = sizeof(address);
  ::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length);
  const int port = ntohs(address.sin_port);
  expect(port > 0, "ephemeral port assigned");

  std::thread server([listener, &router]() {
    const int client = ::accept(listener, nullptr, nullptr);
    if (client < 0) return;
    CancellationSource token;
    const Status status = serve_http_connection(client, router, token.token());
    (void)status;
    ::close(client);
  });

  const int client = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in target{};
  target.sin_family = AF_INET;
  target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  target.sin_port = htons(static_cast<std::uint16_t>(port));
  expect(::connect(client, reinterpret_cast<sockaddr*>(&target),
                   sizeof(target)) == 0,
         "client connected");
  const std::string request =
      "GET /payload.ipa HTTP/1.1\r\nHost: 127.0.0.1\r\n"
      "Range: bytes=5-9\r\nConnection: close\r\n\r\n";
  ::send(client, request.data(), request.size(), 0);
  const std::string response = read_socket_until_close(client);
  ::close(client);
  server.join();
  ::close(listener);

  expect(response.rfind("HTTP/1.1 206 Partial Content", 0) == 0,
         "socket response status");
  expect(response.find("Content-Range: bytes 5-9/100") != std::string::npos,
         "socket response range header");
  expect(response.find("Accept-Ranges: bytes") != std::string::npos,
         "socket response accept ranges");
  const std::size_t body_start = response.find("\r\n\r\n");
  expect(body_start != std::string::npos, "socket response has a body");
  if (body_start != std::string::npos) {
    expect_eq(response.substr(body_start + 4), std::string("56789"),
              "socket response body");
  }
}
#else
void test_http_server(const std::string&) {}
#endif

// ---------------------------------------------------------------------------
// X.509 construction
// ---------------------------------------------------------------------------

ipashelf::Bytes test_signature(const ipashelf::Bytes&) {
  // A structurally valid ECDSA-Sig-Value. The tests exercise DER encoding, not
  // signature verification.
  return {0x30, 0x06, 0x02, 0x01, 0x01, 0x02, 0x01, 0x01};
}

void test_x509(const std::string& fixtures) {
  using namespace ipashelf;

  // The NIST P-256 generator; a public constant used as a valid test key.
  const Bytes point = {
      0x04, 0x6b, 0x17, 0xd1, 0xf2, 0xe1, 0x2c, 0x42, 0x47, 0xf8, 0xbc,
      0xe6, 0xe5, 0x63, 0xa4, 0x40, 0xf2, 0x77, 0x03, 0x7d, 0x81, 0x2d,
      0xeb, 0x33, 0xa0, 0xf4, 0xa1, 0x39, 0x45, 0xd8, 0x98, 0xc2, 0x96,
      0x4f, 0xe3, 0x42, 0xe2, 0xfe, 0x1a, 0x7f, 0x9b, 0x8e, 0xe7, 0xeb,
      0x4a, 0x7c, 0x0f, 0x9e, 0x16, 0x2b, 0xce, 0x33, 0x57, 0x6b, 0x31,
      0x5e, 0xce, 0xcb, 0xb6, 0x40, 0x68, 0x37, 0xbf, 0x51, 0xf5};

  auto spki = ec_public_key_info(point);
  expect(spki.has_value(), "SPKI built");
  if (!spki) return;
  expect_eq(spki.value().front(), std::uint8_t{0x30}, "SPKI sequence");
  {
    std::ofstream file(fixtures + "/test-key.der", std::ios::binary);
    file.write(reinterpret_cast<const char*>(spki.value().data()),
               static_cast<std::streamsize>(spki.value().size()));
  }

  expect(!ec_public_key_info({0x04, 0x01}).has_value(),
         "short public key rejected");

  CertificateOptions ca;
  ca.subject.common_name = "IPAShelf Root CA";
  ca.subject.organization = "IPAShelf";
  ca.subject.country = "US";
  ca.is_ca = true;
  ca.serial = 1;
  ca.not_before = 1760000000;  // 2025-10-09T...
  ca.validity_seconds = 10ULL * 365 * 24 * 60 * 60;
  auto ca_der = build_ec_certificate(ca, spki.value(), test_signature);
  expect(ca_der.has_value(), "CA certificate built");
  if (!ca_der) return;
  {
    std::ofstream file(fixtures + "/test-ca.der", std::ios::binary);
    file.write(reinterpret_cast<const char*>(ca_der.value().data()),
               static_cast<std::streamsize>(ca_der.value().size()));
    std::ofstream pem(fixtures + "/test-ca.pem");
    pem << pem_encode(ca_der.value());
  }

  CertificateOptions leaf;
  leaf.subject.common_name = "IPAShelf Device";
  leaf.subject.organization = "IPAShelf";
  leaf.issuer.common_name = "IPAShelf Root CA";
  leaf.issuer.organization = "IPAShelf";
  leaf.serial = 2;
  leaf.not_before = 1760000000;
  leaf.alt_names.push_back(SubjectAltName{"localhost", std::nullopt});
  leaf.alt_names.push_back(SubjectAltName{"ipashelf.local", std::nullopt});
  leaf.alt_names.push_back(
      SubjectAltName{"", std::vector<std::uint8_t>{127, 0, 0, 1}});
  leaf.alt_names.push_back(
      SubjectAltName{"", std::vector<std::uint8_t>{192, 168, 1, 5}});
  auto leaf_der = build_ec_certificate(leaf, spki.value(), test_signature);
  expect(leaf_der.has_value(), "leaf certificate built");
  if (!leaf_der) return;
  {
    std::ofstream file(fixtures + "/test-cert.der", std::ios::binary);
    file.write(reinterpret_cast<const char*>(leaf_der.value().data()),
               static_cast<std::streamsize>(leaf_der.value().size()));
    std::ofstream pem(fixtures + "/test-cert.pem");
    pem << pem_encode(leaf_der.value());
  }

  const std::string der_text(reinterpret_cast<const char*>(leaf_der.value().data()),
                             leaf_der.value().size());
  expect(der_text.find("IPAShelf Device") != std::string::npos, "subject CN");
  expect(der_text.find("localhost") != std::string::npos, "DNS SAN");
  expect(der_text.find(std::string("\x87\x04\x7f\x00\x00\x01", 6)) !=
             std::string::npos,
         "IPv4 SAN");
  expect(der_text.find(std::string("\x30\x06\x02\x01\x01\x02\x01\x01", 8)) !=
             std::string::npos,
         "signature embedded");

  std::size_t offset = 0;
  auto root = der_read_element(leaf_der.value(), offset);
  expect(root.has_value(), "certificate DER parses");
  if (root) {
    expect_eq(root.value().tag, std::uint8_t{0x30}, "certificate sequence tag");
    expect_eq(offset, leaf_der.value().size(), "certificate length matches");
    std::size_t inner = 0;
    auto tbs = der_read_element(root.value().content, inner);
    expect(tbs.has_value(), "tbs parses");
    if (tbs) {
      expect_eq(tbs.value().tag, std::uint8_t{0x30}, "tbs sequence tag");
    }
  }

  const std::string pem = pem_encode(leaf_der.value());
  expect(pem.find("-----BEGIN CERTIFICATE-----") == 0, "PEM header");
  expect(pem.find("-----END CERTIFICATE-----") != std::string::npos,
         "PEM footer");
  expect(pem.find('\n') != std::string::npos, "PEM newlines");

  expect_bytes(der_integer(0), (Bytes{0x02, 0x01, 0x00}), "integer zero");
  expect_bytes(der_integer(127), (Bytes{0x02, 0x01, 0x7f}), "integer 127");
  expect_bytes(der_integer(128), (Bytes{0x02, 0x02, 0x00, 0x80}),
               "integer 128 needs padding");
  expect_bytes(der_integer(-1), (Bytes{0x02, 0x01, 0xff}), "integer -1");

  auto oid = der_object_identifier("1.2.840.10045.4.3.2");
  expect(oid.has_value(), "OID encodes");
  if (oid) {
    expect_bytes(oid.value(), (Bytes{0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d,
                                    0x04, 0x03, 0x02}),
                 "ecdsa-with-SHA256 OID");
  }
  expect(!der_object_identifier("1.2.3.4.5.6.7.8.9.10.11.12.13.14.15.16.17.18")
              .has_value(),
         "OID arc limit");
  expect(!der_object_identifier("abc").has_value(), "malformed OID rejected");
  expect(!build_ec_certificate(ca, spki.value(), CertificateSigner{})
              .has_value(),
         "missing signer rejected");
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: test_core <fixtures-dir>\n";
    return 2;
  }
  const std::string fixtures = argv[1];

  test_crypto();
  test_json();
  test_plist_xml();
  test_plist_binary(fixtures + "/sample.bplist");
  test_plist_binary_write(fixtures);
  test_account_json();
  test_ipa_patch(fixtures);
  test_url();
  test_bag();
  test_login_success();
  test_login_two_factor();
  test_login_bad_password();
  test_search_and_lookup();
  test_latest_external_version();
  test_download_offer();
  test_download_fallback();
  test_purchase();
  test_purchase_license_exists();
  test_version_identifiers();
  test_download_parsing();
  test_map_store_failure();
  test_storefronts();
  test_app_details();
  test_search_filters();
  test_reviews();
  test_ota_manifest(fixtures);
  test_http_parser();
  test_ranges();
  test_ota_router(fixtures);
  test_http_server(fixtures);
  test_x509(fixtures);

  std::cout << (g_failures == 0 ? "PASS" : "FAIL") << ": " << (g_checks - g_failures)
            << "/" << g_checks << " checks\n";
  return g_failures == 0 ? 0 : 1;
}
