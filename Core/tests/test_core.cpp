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
#include "ipashelf/sap_software.hpp"
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
    // A new context restarts the two-exchange handshake state machine.
    exchange_calls = 0;
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
  // Mirrors the shape of a current production bag: the
  // volumeStoreDownloadProduct key holds the ent/download URL and the SAP
  // version is a string.
  ipashelf::PlistValue bag = ipashelf::PlistValue::dictionary();
  bag.set("authenticateAccount",
          ipashelf::PlistValue::string(
              "https://buy.itunes.apple.com/WebObjects/MZFinance.woa/wa/"
              "authenticate/"));
  bag.set("volumeStoreDownloadProduct",
          ipashelf::PlistValue::string(
              "https://downloaddispatch.itunes.apple.com/WebObjects/"
              "DownloadDispatch.woa/wa/ent/download"));
  bag.set("redownloadProduct",
          ipashelf::PlistValue::string(
              "https://downloaddispatch.itunes.apple.com/r/redownload"));
  bag.set("updateProduct",
          ipashelf::PlistValue::string(
              "https://downloaddispatch.itunes.apple.com/up/updateProduct"));
  bag.set("sign-sap-setup",
          ipashelf::PlistValue::string(
              "https://fpinit.itunes.apple.com/v1/signSapSetup/legacy"));
  bag.set("sign-sap-setup-cert",
          ipashelf::PlistValue::string(
              "https://s.mzstatic.com/sap/setupCert.plist"));
  bag.set("sign-sap-version", ipashelf::PlistValue::string("200"));
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

ipashelf::PlistValue make_download_reply(std::int64_t item_id = 0) {
  ipashelf::PlistValue sinf = ipashelf::PlistValue::dictionary();
  sinf.set("id", ipashelf::PlistValue::integer(1));
  sinf.set("sinf", ipashelf::PlistValue::data(bytes_of("sinf-bytes")));
  ipashelf::PlistValue sinfs = ipashelf::PlistValue::array();
  sinfs.append(std::move(sinf));

  ipashelf::PlistValue metadata = ipashelf::PlistValue::dictionary();
  if (item_id != 0) {
    metadata.set("itemId", ipashelf::PlistValue::integer(item_id));
  }
  metadata.set("bundleShortVersionString",
               ipashelf::PlistValue::string("3.1.4"));
  metadata.set("releaseDate",
               ipashelf::PlistValue::string("2025-03-04T05:06:07Z"));
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

  explicit LoginFixture(bool wrapped_bag = false) {
    hardware = ipashelf::HardwareIdentity::random();
    ipashelf::StoreConfiguration configuration;
    configuration.sleep = [](std::chrono::milliseconds) {};
    configuration.sap_backend_factory = [this]() {
      return ipashelf::Result<std::shared_ptr<ipashelf::SapBackend>>(sap);
    };
    client = std::make_unique<ipashelf::StoreClient>(transport,
                                                    std::move(configuration));
    if (wrapped_bag) {
      // The production bag endpoint wraps the plist in a WebObjects
      // envelope instead of returning a bare plist document.
      transport.enqueue_response(
          200, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"no\"?>\n"
               "<Document>\n<Protocol>\n" +
                   make_bag().dump_xml() + "</Protocol>\n</Document>\n");
    } else {
      transport.enqueue_plist(make_bag());
    }
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

void test_plist_envelope() {
  using namespace ipashelf;
  // Apple WebObjects endpoints (notably the URL bag) wrap the property list
  // in an application envelope instead of returning a bare plist document.
  const std::string inner =
      "<plist version=\"1.0\">\n<dict>\n<key>authenticateAccount</key>\n"
      "<string>https://buy.itunes.apple.com/authenticate</string>\n"
      "</dict>\n</plist>\n";
  const std::string wrapped =
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"no\"?>\n"
      "<Document>\n<Protocol>\n" +
      inner + "</Protocol>\n</Document>\n";
  auto parsed = PlistValue::parse(bytes_of(wrapped));
  expect(parsed.has_value(), "envelope-wrapped plist parses");
  if (parsed) {
    expect_eq(parsed.value().string_or("authenticateAccount"),
              std::string("https://buy.itunes.apple.com/authenticate"),
              "envelope plist value");
  }

  // Envelope around a bare dict.
  const std::string wrapped_dict =
      "<Document><dict><key>k</key><string>v</string></dict></Document>";
  auto parsed_dict = PlistValue::parse(bytes_of(wrapped_dict));
  expect(parsed_dict.has_value() && parsed_dict.value().is_dictionary(),
         "envelope-wrapped dict parses");
  if (parsed_dict) {
    expect_eq(parsed_dict.value().string_or("k"), std::string("v"),
              "envelope dict value");
  }

  // Bare key/value fragment without any container element.
  const std::string fragment = "<key>a</key><string>1</string>";
  auto parsed_fragment = PlistValue::parse(bytes_of(fragment));
  expect(parsed_fragment.has_value() &&
             parsed_fragment.value().is_dictionary(),
         "key fragment parses as dict");
  if (parsed_fragment) {
    expect_eq(parsed_fragment.value().string_or("a"), std::string("1"),
              "key fragment value");
  }

  // Non-plist payloads still fail cleanly.
  expect(!PlistValue::parse(bytes_of("<html><body>oops</body></html>"))
              .has_value(),
         "HTML page is not a plist");
  expect(!PlistValue::parse(bytes_of("plain text")).has_value(),
         "plain text is not a plist");
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
  // The volumeStoreDownloadProduct key carries the ent/download URL in
  // current bags; the legacy volume store URL is rebuilt from the pod.
  expect_eq(bag.value().ent_download_endpoint,
            std::string("https://downloaddispatch.itunes.apple.com/"
                        "WebObjects/DownloadDispatch.woa/wa/ent/download"),
            "bag ent/download endpoint");
  expect(bag.value().validate().ok(), "production-shaped bag validates");
  expect(bag.value().validate_for_authentication().ok(),
         "bag is usable for sign-in");

  auto normalised = normalize_auth_endpoint(
      "https://buy.itunes.apple.com/WebObjects/MZFinance.woa/wa/authenticate");
  expect(normalised.has_value(), "auth endpoint normalises");
  if (normalised) {
    expect(normalised.value().back() == '/', "auth endpoint gains slash");
  }
  auto with_query = normalize_auth_endpoint(
      "https://p7-buy.itunes.apple.com:443/WebObjects/MZFinance.woa/wa/"
      "authenticate?Pod=7&PRH=7");
  expect(with_query.has_value(), "pod auth endpoint normalises");
  if (with_query) {
    expect(with_query.value().find("authenticate/?Pod=7&PRH=7") !=
               std::string::npos,
           "normalisation retains host port and query");
  }
  expect(!normalize_auth_endpoint(
              "https://buy.itunes.apple.com:8443/WebObjects/MZFinance.woa/"
              "wa/authenticate"),
         "non-default port rejected");
  expect(!normalize_auth_endpoint(
              "https://buy.itunes.apple.com/WebObjects/MZFinance.woa/wa/"
              "authenticate//"),
         "double slash rejected");
  expect(!normalize_auth_endpoint(
              "https://buy.itunes.apple.com/WebObjects/MZFinance.woa/wa/"
              "authenticate%2f"),
         "encoded path rejected");
  expect(!normalize_auth_endpoint(
              "https://user@buy.itunes.apple.com/WebObjects/MZFinance.woa/"
              "wa/authenticate"),
         "userinfo rejected");

  // Parsing itself is deliberately lenient (download paths fetch the bag
  // without authentication requirements); validation happens at use sites.
  PlistValue hostile = make_bag();
  hostile.set("authenticateAccount",
              PlistValue::string("https://evil.example.com/auth"));
  auto rejected = Bag::parse(hostile);
  expect(rejected.has_value(), "bag parse does not validate endpoints");
  if (rejected) {
    expect(!rejected.value().validate().ok(), "foreign auth host rejected");
    expect(!rejected.value().validate_for_authentication().ok(),
           "foreign auth host rejected for sign-in");
  }

  PlistValue unsafe = make_bag();
  unsafe.set("redownloadProduct",
             PlistValue::string("https://downloaddispatch.itunes.apple.com/r/redownload?x=1"));
  auto unsafe_bag = Bag::parse(unsafe);
  expect(unsafe_bag.has_value() && !unsafe_bag.value().validate().ok(),
         "redownload with query rejected");

  PlistValue no_sap = make_bag();
  no_sap.set("sign-sap-version", PlistValue::string("199"));
  auto old_sap = Bag::parse(no_sap);
  expect(old_sap.has_value() &&
             !old_sap.value().validate_for_authentication().ok(),
         "unsupported SAP version rejected for sign-in");
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
  // The wire body is an XML plist with string values, never form fields.
  auto body_plist = PlistValue::parse(login_request.body);
  expect(body_plist.has_value() && body_plist.value().is_dictionary(),
         "login body is a plist");
  if (body_plist) {
    const PlistValue& fields = body_plist.value();
    expect_eq(fields.string_or("appleId"), std::string("user@example.com"),
              "login body apple id");
    expect_eq(fields.string_or("password"), std::string("secret123456"),
              "login body appends verification code");
    expect_eq(fields.string_or("attempt"), std::string("1"),
              "login body attempt is a string");
    expect_eq(fields.string_or("why"), std::string("signIn"), "login why");
    expect_eq(fields.string_or("rmp"), std::string("0"), "login rmp");
    expect_eq(fields.string_or("guid"), fixture.hardware.guid(),
              "login guid");
  }
  expect(text_of(login_request.body).find("appleId=") == std::string::npos,
         "login body is not form-encoded");
  expect_eq(fixture.sap->sign_calls, 1, "SAP signed once");
  expect(!fixture.sap->initialized_with.empty(), "SAP received hardware block");
  expect_eq(fixture.sap->teardown_calls, 1, "SAP session torn down");
}

void test_login_bag_envelope() {
  using namespace ipashelf;
  // Regression: the production bag endpoint wraps its plist in a
  // <Document><Protocol> envelope; login must still succeed.
  LoginFixture fixture(/*wrapped_bag=*/true);
  ipashelf::HttpHeaders headers;
  headers.set("X-Set-Apple-Store-Front", "143441-1,29");
  fixture.transport.enqueue_plist(make_auth_reply(), 200, headers);

  LoginRequest request;
  request.email = "user@example.com";
  request.password = "secret";
  request.hardware = fixture.hardware;

  CancellationSource source;
  auto result = fixture.client->login(request, source.token());
  expect(result.has_value(), "login with envelope-wrapped bag succeeds");
  if (!result) {
    std::cerr << "  login error: " << result.error().message << "\n";
    return;
  }
  expect_eq(result.value().account.password_token,
            std::string("password-token"), "envelope bag login token");
  expect_eq(fixture.transport.requests.size(), std::size_t{4},
            "envelope bag login request count");
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

void test_catalog_throttling() {
  using namespace ipashelf;
  FakeTransport transport;
  StoreConfiguration configuration;
  std::vector<long long> waits;
  configuration.sleep = [&waits](std::chrono::milliseconds duration) {
    waits.push_back(static_cast<long long>(duration.count()));
  };
  StoreClient client(transport, configuration);
  CancellationSource source;

  // Distinct app ids keep the process-wide catalog cache from leaking
  // between tests.
  App app;
  app.id = 987654321;
  app.bundle_id = "com.example.sample";

  // Throttled twice, then through: Retry-After is honoured first, the
  // default backoff second, and the third attempt succeeds.
  HttpHeaders retry_headers;
  retry_headers.set("Retry-After", "2");
  transport.enqueue_response(429, "", retry_headers);
  transport.enqueue_response(429, "");
  transport.enqueue_response(200, catalog_json(app.id, "9100"));
  auto version = client.latest_external_version_id(app, "jp",
                                                  Platform::kIPhone,
                                                  source.token());
  expect(version.has_value(), "catalog succeeds after throttling");
  if (version) {
    expect_eq(version.value(), std::string("9100"), "throttled version id");
  }
  expect_eq(waits.size(), std::size_t{2}, "two throttled waits");
  if (waits.size() == 2) {
    expect_eq(waits[0], 2000LL, "Retry-After honoured");
    expect_eq(waits[1], 8000LL, "default backoff");
  }

  // A repeat lookup for the same key is served from the cache without a
  // new request.
  const std::size_t requests_so_far = transport.requests.size();
  auto cached = client.latest_external_version_id(app, "jp",
                                                  Platform::kIPhone,
                                                  source.token());
  expect(cached.has_value(), "cached catalog lookup succeeds");
  if (cached) {
    expect_eq(cached.value(), std::string("9100"), "cached version id");
  }
  expect_eq(transport.requests.size(), requests_so_far,
            "cache avoids a request");

  // Persistent throttling is reported as temporarily unavailable, with the
  // raw status preserved for diagnostics.
  app.id = 987654322;
  transport.enqueue_response(429, "");
  transport.enqueue_response(429, "");
  transport.enqueue_response(429, "");
  auto limited = client.latest_external_version_id(app, "jp",
                                                   Platform::kIPhone,
                                                   source.token());
  expect(!limited.has_value(), "persistent 429 fails");
  if (!limited) {
    expect(limited.error().code == ErrorCode::kTemporarilyUnavailable,
           "429 maps to temporarily unavailable");
    expect_eq(limited.error().http_status, 429, "429 status kept");
  }
}

void test_download_unpinned_when_catalog_throttled() {
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

  // macOS: the pin lookup is throttled away, so the volume-store request
  // goes out unpinned instead of failing the download.
  App app;
  app.id = 987654323;
  app.bundle_id = "com.example.sample";
  transport.enqueue_response(429, "");
  transport.enqueue_response(429, "");
  transport.enqueue_response(429, "");
  transport.enqueue_plist(make_download_reply());
  auto offer = client.download_offer(account, app, "", Platform::kMacOS,
                                     source.token());
  expect(offer.has_value(), "macOS download survives a throttled catalog");
  if (offer) {
    expect_eq(offer.value().external_version_id, std::string("9001"),
              "throttled macOS version");
  }

  // iPhone redownload: the volume store is empty, the catalog is
  // throttled, and the redownload still proceeds unpinned.
  app.id = 987654324;
  transport.requests.clear();
  PlistValue empty = PlistValue::dictionary();
  empty.set("songList", PlistValue::array());
  transport.enqueue_plist(empty);
  transport.enqueue_plist(make_bag());
  transport.enqueue_response(429, "");
  transport.enqueue_response(429, "");
  transport.enqueue_response(429, "");
  transport.enqueue_plist(make_download_reply(app.id));
  auto redownload = client.download_offer(account, app, "",
                                          Platform::kIPhone, source.token());
  expect(redownload.has_value(),
         "redownload survives a throttled catalog");
  expect_eq(transport.requests.size(), std::size_t{6},
            "volume, bag, throttled catalog, redownload");
  if (transport.requests.size() == 6) {
    expect(transport.requests[5].url.find("/r/redownload") !=
               std::string::npos,
           "unpinned redownload sent");
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
  account.pod = "42";

  App app;
  app.id = 123456789;
  app.bundle_id = "com.example.sample";

  // Without a kbsync generator the volume store is tried first and no bag
  // fetch is needed; the URL is rebuilt from the account pod.
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
  expect_eq(transport.requests.size(), std::size_t{1},
            "download goes straight to the volume store");
  if (transport.requests.size() >= 1) {
    const auto& download = transport.requests[0];
    expect(download.url.find("p42-buy.itunes.apple.com") != std::string::npos,
           "volume store URL uses the account pod");
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

  // Volume store (empty) -> bag fetch -> redownload.
  transport.enqueue_plist(empty);
  transport.enqueue_plist(make_bag());
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

void test_download_redirect_replay() {
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

  // The volume store answers with a pod redirect. Apple expects the
  // identical POST to be replayed at the new location; a generic redirect
  // follow would turn it into a bodyless GET and the request would fail.
  HttpHeaders redirect_headers;
  redirect_headers.set(
      "Location",
      "https://p25-buy.itunes.apple.com/WebObjects/MZFinance.woa/wa/"
      "volumeStoreDownloadProduct?guid=AABBCCDDEEFF");
  transport.enqueue_response(302, "", redirect_headers);
  transport.enqueue_plist(make_download_reply());

  auto offer = client.download_offer(account, app, "9001", Platform::kIPhone,
                                     source.token());
  expect(offer.has_value(), "download survives a pod redirect");
  if (!offer) {
    std::cerr << "  redirect replay error: " << offer.error().message << "\n";
  }
  expect_eq(transport.requests.size(), std::size_t{2}, "two requests");
  if (transport.requests.size() == 2) {
    const auto& replay = transport.requests[1];
    expect_eq(replay.method, std::string("POST"), "redirect replays POST");
    expect(replay.url.find("p25-buy.itunes.apple.com") != std::string::npos,
           "redirect target pod");
    expect(!replay.body.empty(), "redirect keeps the plist body");
    expect(!replay.follow_redirects, "store posts never auto-follow");
    expect(transport.requests[0].body == replay.body,
           "redirect replays the identical body");
  }

  // A redirect to an unexpected host must be refused.
  FakeTransport hostile;
  StoreClient hostile_client(hostile, configuration);
  HttpHeaders hostile_headers;
  hostile_headers.set("Location", "https://example.com/steal");
  hostile.enqueue_response(302, "", hostile_headers);
  auto refused = hostile_client.download_offer(account, app, "9001",
                                               Platform::kIPhone,
                                               source.token());
  expect(!refused.has_value(), "unexpected redirect host is refused");
  expect_eq(hostile.requests.size(), std::size_t{1},
            "hostile redirect is not followed");
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

  transport.enqueue_plist(make_download_reply());
  auto versions =
      client.version_list(account, app, Platform::kIPhone, source.token());
  expect(versions.has_value(), "version list resolves");
  if (versions) {
    expect_eq(versions.value().identifiers.size(), std::size_t{2},
              "two versions");
    expect_eq(versions.value().identifiers.front(), std::string("9001"),
              "server order preserved");
    expect_eq(versions.value().identifiers.back(), std::string("8800"),
              "older version listed");
    expect_eq(versions.value().latest, std::string("9001"),
              "latest identifier reported separately");
  }
}

void test_download_parsing() {
  using namespace ipashelf;
  auto offer = parse_download_response(make_download_reply());
  expect(offer.has_value(), "download response parses");
  if (offer) {
    expect_eq(offer.value().bundle_id, std::string("com.example.sample"),
              "parsed bundle id");
    expect_eq(offer.value().metadata.size(), std::size_t{5}, "metadata keys");
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

void test_phone_storefront() {
  using namespace ipashelf;
  expect_eq(phone_storefront_header("+8613800000000"), std::string("143465"),
            "international China mobile");
  expect_eq(phone_storefront_header("13800000000"), std::string("143465"),
            "national China mobile");
  expect_eq(phone_storefront_header("+86 138-0000-0000"),
            std::string("143465"), "formatted China mobile");
  expect_eq(phone_storefront_header("+919876543210"), std::string("143467"),
            "international India mobile");
  expect_eq(phone_storefront_header("919876543210"), std::string("143467"),
            "India mobile with country code");
  expect_eq(phone_storefront_header("09876543210"), std::string("143467"),
            "India mobile with trunk prefix");
  expect_eq(phone_storefront_header("9123456789"), std::string("143467"),
            "local India number starting with 91");
  expect_eq(phone_storefront_header("user@example.com"), std::string{},
            "email has no phone storefront");
  expect_eq(phone_storefront_header("+14155552671"), std::string{},
            "unsupported international number");
  expect_eq(phone_storefront_header("+442071234567"), std::string{},
            "unsupported country code does not fall back to local rules");
}

void test_login_phone_storefront_header() {
  using namespace ipashelf;
  LoginFixture fixture;
  HttpHeaders headers;
  headers.set("X-Set-Apple-Store-Front", "143465-2,29");
  fixture.transport.enqueue_plist(make_auth_reply(), 200, headers);

  LoginRequest request;
  request.email = "+8613800000000";
  request.password = "secret";
  request.hardware = fixture.hardware;
  CancellationSource source;
  auto result = fixture.client->login(request, source.token());
  expect(result.has_value(), "phone-number sign-in succeeds");
  expect_eq(fixture.transport.requests.size(), std::size_t{4},
            "four requests issued");
  if (fixture.transport.requests.size() == 4) {
    expect_eq(fixture.transport.requests[3].headers.get_or(
                  "X-Apple-Store-Front"),
              std::string("143465"), "phone storefront routing hint sent");
  }
}

void test_login_redirects() {
  using namespace ipashelf;
  {
    LoginFixture fixture;
    HttpHeaders headers;
    headers.set("Location", "https://evil.example.com/authenticate");
    fixture.transport.enqueue_response(302, "", headers);
    LoginRequest request;
    request.email = "user@example.com";
    request.password = "secret";
    request.hardware = fixture.hardware;
    CancellationSource source;
    auto result = fixture.client->login(request, source.token());
    expect(!result.has_value(), "foreign redirect destination rejected");
  }
  {
    LoginFixture fixture;
    HttpHeaders headers;
    headers.set("Location",
                "https://buy.itunes.apple.com/WebObjects/MZFinance.woa/wa/"
                "authenticate");
    fixture.transport.enqueue_response(303, "", headers);
    LoginRequest request;
    request.email = "user@example.com";
    request.password = "secret";
    request.hardware = fixture.hardware;
    CancellationSource source;
    auto result = fixture.client->login(request, source.token());
    expect(!result.has_value(), "303 is not an authentication redirect");
  }
  {
    LoginFixture fixture;
    HttpHeaders redirect;
    redirect.set("Location",
                 "https://p9-buy.itunes.apple.com/WebObjects/MZFinance.woa/"
                 "wa/authenticate");
    fixture.transport.enqueue_response(302, "", redirect);
    HttpHeaders headers;
    headers.set("X-Set-Apple-Store-Front", "143441-1,29");
    fixture.transport.enqueue_plist(make_auth_reply(), 200, headers);
    LoginRequest request;
    request.email = "user@example.com";
    request.password = "secret";
    request.hardware = fixture.hardware;
    CancellationSource source;
    auto result = fixture.client->login(request, source.token());
    expect(result.has_value(), "pod redirect is followed");
    expect_eq(fixture.sap->sign_calls, 2,
              "redirected request is signed again");
    if (fixture.transport.requests.size() == 5) {
      expect(fixture.transport.requests[4].url.find("p9-buy") !=
                 std::string::npos,
             "second attempt went to the pod");
    }
  }
}

void test_ent_download() {
  using namespace ipashelf;
  FakeTransport transport;
  StoreConfiguration configuration;
  configuration.sleep = [](std::chrono::milliseconds) {};
  int generator_calls = 0;
  configuration.kbsync_generator =
      [&generator_calls](const Bytes& hardware_id, std::uint64_t dsid)
      -> Result<Bytes> {
    ++generator_calls;
    expect_eq(hardware_id.size(), std::size_t{6}, "kbsync hardware id size");
    expect_eq(dsid, std::uint64_t{1234567890}, "kbsync dsid");
    return Bytes{1, 2, 3};
  };
  std::optional<std::string> stored_entry;
  configuration.kbsync_cache_load = [&stored_entry]() { return stored_entry; };
  configuration.kbsync_cache_store = [&stored_entry](std::string_view entry) {
    stored_entry = std::string(entry);
  };
  bool cache_removed = false;
  configuration.kbsync_cache_remove = [&cache_removed]() {
    cache_removed = true;
  };
  StoreClient client(transport, configuration);
  CancellationSource source;

  Account account;
  account.directory_services_id = "1234567890";
  account.password_token = "password-token";
  account.storefront = "143441-1,29";
  account.device_id = "A1B2C3D4E5F6";
  account.region = "us";

  App app;
  app.id = 123456789;
  app.bundle_id = "com.example.sample";

  transport.enqueue_plist(make_bag());
  transport.enqueue_plist(make_download_reply(123456789));
  auto offer = client.download_offer(account, app, "9001", Platform::kIPhone,
                                     source.token());
  expect(offer.has_value(), "ent/download offer succeeds");
  expect_eq(transport.requests.size(), std::size_t{2}, "bag then ent/download");
  if (transport.requests.size() == 2) {
    const auto& request = transport.requests[1];
    expect(request.url.find("ent/download?guid=A1B2C3D4E5F6") !=
               std::string::npos,
           "ent/download URL");
    expect_eq(request.headers.get_or("X-Token"),
              std::string("password-token"), "ent token header");
    expect(!request.follow_redirects,
           "ent/download must not forward X-Token on redirects");
    auto body = PlistValue::parse(request.body);
    expect(body.has_value(), "ent body is a plist");
    if (body) {
      expect_eq(body.value().string_or("kbsync"), std::string("AQID"),
                "kbsync blob is base64 in the payload");
      expect_eq(body.value().string_or("salableAdamId"),
                std::string("123456789"), "ent app id is a string");
      const Bytes serial = {0x54, 0xc8, 0xb0, 0xa9, 0x88,
                            0xc3, 0xd4, 0xe5, 0xf6};
      expect_eq(body.value().string_or("serialNumber"), base64_encode(serial),
                "ent serial number derivation");
      expect_eq(body.value().string_or("externalVersionId"),
                std::string("9001"), "ent version pin");
    }
  }
  expect(stored_entry.has_value(), "validated kbsync blob is cached");
  expect(!cache_removed, "cache entry retained");

  // A second download reuses the cached blob without regenerating it.
  transport.enqueue_plist(make_bag());
  transport.enqueue_plist(make_download_reply(123456789));
  auto second = client.download_offer(account, app, "9001", Platform::kIPhone,
                                        source.token());
  expect(second.has_value(), "cached ent/download succeeds");
  expect_eq(generator_calls, 1, "kbsync generated only once");
}

void test_ent_download_fallbacks() {
  using namespace ipashelf;
  Account account;
  account.directory_services_id = "1234567890";
  account.password_token = "password-token";
  account.storefront = "143441-1,29";
  account.device_id = "A1B2C3D4E5F6";
  account.region = "us";
  App app;
  app.id = 123456789;
  app.bundle_id = "com.example.sample";

  {
    // Structured ent/download failure falls back to the legacy chain and
    // does not populate the cache.
    FakeTransport transport;
    StoreConfiguration configuration;
    configuration.sleep = [](std::chrono::milliseconds) {};
    configuration.kbsync_generator =
        [](const Bytes&, std::uint64_t) -> Result<Bytes> {
      return Bytes{7};
    };
    bool stored = false;
    configuration.kbsync_cache_store = [&stored](std::string_view) {
      stored = true;
    };
    StoreClient client(transport, configuration);
    CancellationSource source;

    PlistValue unavailable = PlistValue::dictionary();
    unavailable.set("failureType", PlistValue::string("2059"));
    transport.enqueue_plist(make_bag());
    transport.enqueue_plist(unavailable);
    transport.enqueue_plist(make_download_reply());
    auto offer = client.download_offer(account, app, "9001",
                                       Platform::kIPhone, source.token());
    expect(offer.has_value(), "legacy chain serves after ent failure");
    expect(!stored, "failed kbsync blob is not cached");
    expect_eq(transport.requests.size(), std::size_t{3},
              "bag, ent, then volume store");
    if (transport.requests.size() == 3) {
      expect(transport.requests[2].url.find("volumeStoreDownloadProduct") !=
                 std::string::npos,
             "fallback hits the volume store");
    }
  }

  {
    // A rejected cached blob is invalidated and regenerated exactly once.
    FakeTransport transport;
    StoreConfiguration configuration;
    configuration.sleep = [](std::chrono::milliseconds) {};
    int generator_calls = 0;
    configuration.kbsync_generator =
        [&generator_calls](const Bytes&, std::uint64_t) -> Result<Bytes> {
      ++generator_calls;
      return Bytes{9, 9};
    };
    std::optional<std::string> stored_entry =
        std::string("{\"dsid\":1234567890,\"guid\":\"A1B2C3D4E5F6\","
                    "\"data\":\"AQID\"}");
    bool removed = false;
    configuration.kbsync_cache_load = [&stored_entry]() {
      return stored_entry;
    };
    configuration.kbsync_cache_store =
        [&stored_entry](std::string_view entry) {
          stored_entry = std::string(entry);
        };
    configuration.kbsync_cache_remove = [&]() { removed = true; };
    StoreClient client(transport, configuration);
    CancellationSource source;

    transport.enqueue_plist(make_bag());
    transport.enqueue_response(500, "");
    transport.enqueue_plist(make_download_reply(123456789));
    auto offer = client.download_offer(account, app, "9001",
                                       Platform::kIPhone, source.token());
    expect(offer.has_value(), "fresh kbsync blob succeeds after rejection");
    expect(removed, "rejected cached entry removed");
    expect_eq(generator_calls, 1, "one regeneration");
    expect(stored_entry.has_value() &&
               stored_entry->find("CQk=") != std::string::npos,
           "fresh blob cached");
  }
}

ipashelf::Bytes t_dmap(std::string_view name, const ipashelf::Bytes& payload) {
  ipashelf::Bytes out(name.begin(), name.end());
  const std::uint32_t size = static_cast<std::uint32_t>(payload.size());
  out.push_back(static_cast<std::uint8_t>((size >> 24) & 0xff));
  out.push_back(static_cast<std::uint8_t>((size >> 16) & 0xff));
  out.push_back(static_cast<std::uint8_t>((size >> 8) & 0xff));
  out.push_back(static_cast<std::uint8_t>(size & 0xff));
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

ipashelf::Bytes t_dmap_u32(std::string_view name, std::uint32_t value) {
  ipashelf::Bytes payload = {
      static_cast<std::uint8_t>((value >> 24) & 0xff),
      static_cast<std::uint8_t>((value >> 16) & 0xff),
      static_cast<std::uint8_t>((value >> 8) & 0xff),
      static_cast<std::uint8_t>(value & 0xff)};
  return t_dmap(name, payload);
}

ipashelf::Bytes t_dmap_str(std::string_view name, std::string_view value) {
  return t_dmap(name, ipashelf::Bytes(value.begin(), value.end()));
}

ipashelf::Bytes t_owned_item(std::uint32_t id, std::uint32_t media_kind,
                             std::uint32_t products, std::string_view bundle,
                             std::string_view name, std::string_view version,
                             std::uint32_t purchase_date, bool use_minm) {
  ipashelf::Bytes payload;
  const auto add = [&](ipashelf::Bytes part) {
    payload.insert(payload.end(), part.begin(), part.end());
  };
  add(t_dmap_u32("aeSI", id));
  add(t_dmap_u32("aeMk", media_kind));
  add(t_dmap_u32("aeSS", products));
  add(t_dmap_str("aeBI", bundle));
  add(t_dmap_str(use_minm ? "minm" : "aeLN", name));
  add(t_dmap_str("aePd", version));
  add(t_dmap_u32("asdp", purchase_date));
  return t_dmap("mlit", payload);
}

void enqueue_owned_flow(FakeTransport& transport) {
  using namespace ipashelf;
  const auto respond = [&transport](Bytes body) {
    transport.enqueue([body](const HttpRequest&) {
      HttpResponse response;
      response.status = 200;
      response.body = body;
      return Result<HttpResponse>(response);
    });
  };
  Bytes login_payload;
  const auto add = [](Bytes& into, Bytes part) {
    into.insert(into.end(), part.begin(), part.end());
  };
  add(login_payload, t_dmap_u32("mstt", 200));
  add(login_payload, t_dmap_u32("mlid", 4242));
  Bytes update_payload;
  add(update_payload, t_dmap_u32("mstt", 200));
  add(update_payload, t_dmap_u32("musr", 7));

  // Storefront ,34: a mobile app plus a Mac app discovered by media kind.
  Bytes items_34_list;
  add(items_34_list,
      t_owned_item(111, 131072, 3, "com.example.one", "One", "1.0",
                   1700000000, false));
  add(items_34_list,
      t_owned_item(222, 67108864, 0, "com.example.mac", "Mac App", "2.0",
                   1690000000, true));
  // Storefront ,13: the same mobile app with Mac availability and a newer
  // purchase date, plus a visionOS app.
  Bytes items_13_list;
  add(items_13_list,
      t_owned_item(111, 131072, 9, "com.example.one", "One", "1.0",
                   1700001000, false));
  add(items_13_list,
      t_owned_item(333, 262144, 16, "com.example.vision", "Vision", "3.0",
                   1710000000, false));

  respond(t_dmap("mlog", login_payload));
  respond(t_dmap("mupd", update_payload));
  respond(t_dmap("avdb", t_dmap("mlcl", items_34_list)));
  respond(t_dmap("mlog", login_payload));
  respond(t_dmap("mupd", update_payload));
  respond(t_dmap("avdb", t_dmap("mlcl", items_13_list)));
}

void test_owned_apps() {
  using namespace ipashelf;
  FakeTransport transport;
  auto sap = std::make_shared<FakeSapBackend>();
  StoreConfiguration configuration;
  configuration.sleep = [](std::chrono::milliseconds) {};
  configuration.sap_backend_factory = [sap]() {
    return Result<std::shared_ptr<SapBackend>>(sap);
  };
  configuration.now_unix = []() { return std::int64_t{1700000000}; };
  configuration.time_zone_name = "Asia/Tokyo";
  configuration.utc_offset_seconds = 32400;
  StoreClient client(transport, configuration);
  CancellationSource source;

  const HardwareIdentity hardware = HardwareIdentity::random();
  Account account;
  account.email = "user@example.com";
  account.password_token = "password-token";
  account.directory_services_id = "1234567890";
  account.storefront = "143441-1,29";
  account.device_id = hardware.guid();

  transport.enqueue_plist(make_bag());
  transport.enqueue_plist(make_certificate());
  transport.enqueue_plist(make_setup_reply());
  enqueue_owned_flow(transport);

  auto page = client.owned_apps(account, std::nullopt, 2, 2, source.token());
  expect(page.has_value(), "owned apps resolve");
  if (!page) {
    std::cerr << "  owned apps error: " << page.error().message << "\n";
    return;
  }
  expect_eq(page.value().total, 3, "merged total across storefronts");
  // Sorted by purchase date descending: 333, 111, 222 -> page 2 holds 222.
  expect_eq(page.value().apps.size(), std::size_t{1}, "second page size");
  if (page.value().apps.size() == 1) {
    const App& mac = page.value().apps[0];
    expect_eq(mac.id, std::int64_t{222}, "oldest purchase last");
    expect_eq(mac.name, std::string("Mac App"), "minm name fallback");
    expect(std::find(mac.platforms.begin(), mac.platforms.end(),
                     Platform::kMacOS) != mac.platforms.end(),
           "media kind marks the Mac app");
  }

  expect_eq(transport.requests.size(), std::size_t{9},
            "bag + SAP setup + two DAAP flows");
  if (transport.requests.size() == 9) {
    const auto& login = transport.requests[3];
    expect(login.url.ends_with("/login"), "DAAP login URL");
    expect(!login.headers.contains("X-Apple-ActionSignature"),
           "DAAP login is unsigned");
    expect_eq(login.headers.get_or("X-Apple-Store-Front"),
              std::string("143441-1,34"), "first storefront suffix");
    expect_eq(login.headers.get_or("X-Apple-TZ"), std::string("540"),
              "timezone offset minutes");
    expect(login.headers.get_or("Date").find("GMT") != std::string::npos,
           "DAAP date header");
    const auto& update = transport.requests[4];
    expect(update.headers.contains("X-Apple-ActionSignature"),
           "DAAP update is SAP-signed");
    expect_eq(text_of(update.body),
              std::string("session-id=4242&revision-number=(null)&query="
                          "('com.apple.itunes.extended\\-media\\-kind:131072'"
                          ",'com.apple.itunes.extended\\-media\\-kind:262144'"
                          ",'com.apple.itunes.extended\\-media\\-kind:"
                          "67108864')"),
              "DAAP update body");
    const auto& items = transport.requests[5];
    expect(items.url.find("/databases/7/items") != std::string::npos,
           "DAAP items URL carries the revision");
    expect(items.headers.contains("X-Apple-ActionSignature"),
           "DAAP items are SAP-signed");
    expect_eq(transport.requests[6].headers.get_or("X-Apple-Store-Front"),
              std::string("143441-1,13"), "second storefront suffix");
  }
  expect_eq(sap->sign_calls, 4, "two signed DAAP calls per storefront");
  expect_eq(sap->teardown_calls, 1, "SAP session torn down");

  // Platform filtering happens after the merge.
  transport.enqueue_plist(make_bag());
  transport.enqueue_plist(make_certificate());
  transport.enqueue_plist(make_setup_reply());
  enqueue_owned_flow(transport);
  auto mac_page =
      client.owned_apps(account, Platform::kMacOS, 1, 10, source.token());
  expect(mac_page.has_value(), "filtered owned apps resolve");
  if (!mac_page) {
    std::cerr << "  filtered owned apps error: " << mac_page.error().message
              << "\n";
  }
  if (mac_page) {
    expect_eq(mac_page.value().total, 2, "Mac-filtered total");
    bool merged_platforms = false;
    std::int64_t merged_date = 0;
    for (const App& app : mac_page.value().apps) {
      if (app.id == 111) {
        merged_platforms =
            app.platforms.size() == 3 &&
            std::find(app.platforms.begin(), app.platforms.end(),
                      Platform::kIPhone) != app.platforms.end() &&
            std::find(app.platforms.begin(), app.platforms.end(),
                      Platform::kIPad) != app.platforms.end() &&
            std::find(app.platforms.begin(), app.platforms.end(),
                      Platform::kMacOS) != app.platforms.end();
        merged_date = app.purchase_date_unix;
      }
    }
    expect(merged_platforms, "platforms merged across storefronts");
    expect_eq(merged_date, std::int64_t{1700001000},
              "newest purchase date wins the merge");
  }

  auto invalid =
      client.owned_apps(account, Platform::kWatchOS, 1, 10, source.token());
  expect(!invalid.has_value(), "watchOS purchase-history filter rejected");
  auto too_big =
      client.owned_apps(account, std::nullopt, 1, 101, source.token());
  expect(!too_big.has_value(), "limit above the DAAP maximum rejected");
}

void test_version_metadata(const std::string&) {
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

  transport.enqueue_plist(make_download_reply());

  auto metadata =
      client.version_metadata(account, app, "9001", Platform::kIPhone,
                              source.token());
  expect(metadata.has_value(), "version metadata resolves");
  if (!metadata) {
    std::cerr << "  version metadata error: " << metadata.error().message
              << "\n";
    return;
  }
  expect_eq(metadata.value().display_version, std::string("3.1.4"),
            "display version from the download metadata");
  // 2025-03-04T05:06:07Z
  expect_eq(metadata.value().release_date_unix, std::int64_t{1741064767},
            "release date from the download metadata");
  expect_eq(transport.requests.size(), std::size_t{1},
            "version metadata needs only the pinned download response");
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
  expect(info.value().declares_supported_platforms,
         "platform declaration present");
  expect_eq(info.value().sinf_paths.size(), std::size_t{2},
            "manifest sinf paths");
  if (info.value().sinf_paths.size() == 2) {
    expect_eq(info.value().sinf_paths[0],
              std::string("Payload/Sample.app/SC_Info/Sample.sinf"),
              "first manifest sinf path is bundle-relative resolved");
    expect_eq(info.value().sinf_paths[1],
              std::string("Payload/Sample.app/Frameworks/Kit.framework/"
                          "SC_Info/Kit.sinf"),
              "framework sinf path");
  }

  // Platform validation mirrors the reference rules: declared support is
  // required, except that a missing declaration is tolerated for iPhoneOS.
  expect(validate_ipa_platform(info.value(), Platform::kIPhone).ok(),
         "iPhoneOS package validates for iPhone");
  expect(validate_ipa_platform(info.value(), Platform::kIPad).ok(),
         "iPhoneOS package validates for iPad");
  expect(!validate_ipa_platform(info.value(), Platform::kAppleTV).ok(),
         "iPhoneOS package rejected for Apple TV");
  expect(validate_ipa_platform(info.value(), Platform::kMacOS).ok(),
         "macOS requests are not platform-checked");
  expect(validate_ipa_platform(info.value(), Platform::kUnspecified).ok(),
         "unspecified requests are not platform-checked");
  IpaInfo legacy;
  legacy.declares_supported_platforms = false;
  expect(validate_ipa_platform(legacy, Platform::kIPhone).ok(),
         "missing declaration tolerated for iPhoneOS");
  expect(!validate_ipa_platform(legacy, Platform::kVisionOS).ok(),
         "missing declaration rejected for visionOS");
  legacy.declares_supported_platforms = true;
  expect(!validate_ipa_platform(legacy, Platform::kIPhone).ok(),
         "empty declaration rejected for iPhoneOS");

  DownloadOffer offer;
  offer.bundle_id = "com.example.sample";
  offer.display_version = "3.1.4";
  offer.external_version_id = "9001";
  PlistValue server_metadata = PlistValue::dictionary();
  server_metadata.set("itemId", PlistValue::integer(123456789));
  server_metadata.set("softwareVersionBundleId",
                      PlistValue::string("com.example.sample"));
  offer.metadata = server_metadata;
  Account account;
  account.email = "user@example.com";
  PlistValue metadata = build_itunes_metadata(offer, account);
  expect_eq(metadata.string_or("apple-id"), std::string("user@example.com"),
            "metadata apple id");
  expect_eq(metadata.string_or("userName"), std::string("user@example.com"),
            "metadata user name");
  expect_eq(metadata.integer_for("itemId").value_or(0), 123456789,
            "server metadata preserved");

  // Sinf blobs pair one-for-one with the manifest paths.
  std::vector<Sinf> sinfs;
  sinfs.push_back(Sinf{0, bytes_of("patched-sinf-1")});
  sinfs.push_back(Sinf{1, bytes_of("patched-sinf-2")});
  auto sinf_entries = resolve_sinf_entries(info.value(), sinfs);
  expect(sinf_entries.has_value(), "sinf entries resolve");
  if (!sinf_entries) return;
  expect_eq(sinf_entries.value().size(), std::size_t{2}, "two sinf entries");
  std::vector<Sinf> short_sinfs;
  short_sinfs.push_back(Sinf{0, bytes_of("only-one")});
  expect(!resolve_sinf_entries(info.value(), short_sinfs).has_value(),
         "sinf count mismatch with manifest rejected");
  IpaInfo no_manifest = info.value();
  no_manifest.sinf_paths.clear();
  auto fallback_entries = resolve_sinf_entries(no_manifest, sinfs);
  expect(fallback_entries.has_value(), "fallback sinf entry resolves");
  if (fallback_entries) {
    expect_eq(fallback_entries.value().size(), std::size_t{1},
              "fallback uses a single sinf");
    expect_eq(fallback_entries.value().front().first,
              default_sinf_path(info.value()), "fallback sinf path");
  }

  IpaPatchOptions options;
  options.source_path = source;
  options.destination_path = fixtures + "/patched.ipa";
  options.metadata = metadata;
  options.artwork = bytes_of("PNG-ARTWORK");
  options.sinf_entries = sinf_entries.value();
  const std::string first_sinf_path = options.sinf_entries[0].first;
  const std::string second_sinf_path = options.sinf_entries[1].first;

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
  const Bytes metadata_bytes = read_entry(metadata_index);
  expect(metadata_bytes.size() >= 8 &&
             std::string(metadata_bytes.begin(), metadata_bytes.begin() + 8) ==
                 "bplist00",
         "metadata is written as a binary plist");
  auto written = PlistValue::parse(metadata_bytes);
  expect(written.has_value(), "written metadata parses");
  if (written) {
    expect_eq(written.value().string_or("apple-id"),
              std::string("user@example.com"), "written metadata apple id");
  }
  expect_eq(text_of(read_entry(find_entry("iTunesArtwork"))),
            std::string("PNG-ARTWORK"), "artwork written");
  expect_eq(text_of(read_entry(find_entry(first_sinf_path))),
            std::string("patched-sinf-1"), "first sinf replaced");
  expect_eq(text_of(read_entry(find_entry(second_sinf_path))),
            std::string("patched-sinf-2"), "framework sinf replaced");
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
// Storefronts, rich catalog data and filters
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

  const std::string page = build_install_page(
      manifest.title, manifest.bundle_id, manifest.display_version, install,
      "https://x/ca.der");
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

  // Attribute injection: the certificate URL comes from the network address
  // and the title comes from Apple, so both must be escaped in the page.
  const std::string hostile = build_install_page(
      "<script>alert(1)</script>\"", "com.example.sample",
      "1.0\"><script>alert(2)</script>",
      "itms-services://?action=download-manifest&url=https%3A%2F%2Fx%2Fm.plist",
      "https://x/\"><script>alert(3)</script>");
  expect(hostile.find("<script>") == std::string::npos,
         "install page escapes injected markup");
  expect(hostile.find("&lt;script&gt;") != std::string::npos,
         "install page keeps escaped markup readable");
  expect(hostile.find("&quot;") != std::string::npos,
         "install page escapes quotes");
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

  // The router advertises 127.0.0.1 here, so use another LAN address for the
  // reverse case: a listener that advertises a LAN address must still answer
  // on loopback.
  OtaRouter lan_router("https://192.168.1.5:8443", package);
  HttpServerRequest loopback;
  loopback.method = "GET";
  loopback.path = "/ping";
  loopback.headers.set("Host", "127.0.0.1:8443");
  expect_eq(lan_router.route(loopback).status, 200, "loopback host allowed");
  HttpServerRequest foreign = loopback;
  foreign.headers.set("Host", "192.168.1.6:8443");
  expect_eq(lan_router.route(foreign).status, 421,
            "other LAN host rejected");

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
  const std::string profile =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\">\n"
      "<dict><key>PayloadContent</key><array/></dict>\n</plist>\n";
  OtaPackage package;
  package.bundle_id = "com.example.sample";
  package.display_version = "4.2.0";
  package.title = "Sample";
  package.payload_path = payload;
  package.profile_data = profile;
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
    for (int served = 0; served < 2; ++served) {
      const int client = ::accept(listener, nullptr, nullptr);
      if (client < 0) return;
      CancellationSource token;
      const Status status =
          serve_http_connection(client, router, token.token());
      (void)status;
      ::close(client);
    }
  });

  sockaddr_in target{};
  target.sin_family = AF_INET;
  target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  target.sin_port = htons(static_cast<std::uint16_t>(port));
  const auto fetch = [&target](const std::string& request) {
    const int client = ::socket(AF_INET, SOCK_STREAM, 0);
    if (::connect(client, reinterpret_cast<sockaddr*>(&target),
                  sizeof(target)) != 0) {
      ::close(client);
      return std::string();
    }
    ::send(client, request.data(), request.size(), 0);
    const std::string response = read_socket_until_close(client);
    ::close(client);
    return response;
  };

  const std::string request =
      "GET /payload.ipa HTTP/1.1\r\nHost: 127.0.0.1\r\n"
      "Range: bytes=5-9\r\nConnection: close\r\n\r\n";
  const std::string response = fetch(request);
  expect(!response.empty(), "client connected");

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

  // The certificate profile used to come back mangled in the field, so the
  // exact bytes and the profile MIME type are asserted over a real socket.
  const std::string profile_response = fetch(
      "GET /profile.mobileconfig HTTP/1.1\r\nHost: 127.0.0.1\r\n"
      "Connection: close\r\n\r\n");
  expect(profile_response.rfind("HTTP/1.1 200 OK", 0) == 0,
         "profile response status");
  expect(profile_response.find(
             "Content-Type: application/x-apple-aspen-config") !=
             std::string::npos,
         "profile content type");
  const std::size_t profile_start = profile_response.find("\r\n\r\n");
  expect(profile_start != std::string::npos, "profile response has a body");
  if (profile_start != std::string::npos) {
    expect_eq(profile_response.substr(profile_start + 4), profile,
              "profile bytes served verbatim");
  }

  server.join();
  ::close(listener);
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
  leaf.authority_key_id = ec_key_identifier(point);
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

  const std::string ca_text(
      reinterpret_cast<const char*>(ca_der.value().data()),
      ca_der.value().size());
  const auto count_occurrences = [](const std::string& haystack,
                                    const std::string& needle) {
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
      ++count;
      ++pos;
    }
    return count;
  };

  // ecdsa-with-SHA256 with parameters ABSENT (RFC 5758 section 3.2) appears
  // exactly twice (TBS + outer). A previous form appended INTEGER 0 as the
  // parameters, which strict parsers reject; make sure it never comes back.
  const std::string algid("\x30\x0a\x06\x08\x2a\x86\x48\xce\x3d\x04\x03\x02", 12);
  expect_eq(count_occurrences(ca_text, algid), std::size_t{2},
            "CA algId has no parameters");
  expect_eq(count_occurrences(der_text, algid), std::size_t{2},
            "leaf algId has no parameters");
  const std::string bad_algid(
      "\x30\x0d\x06\x08\x2a\x86\x48\xce\x3d\x04\x03\x02\x02\x01\x00", 15);
  expect(ca_text.find(bad_algid) == std::string::npos,
         "CA algId is not the broken parameterized form");

  // Basic constraints: the CA carries BOOLEAN TRUE, a leaf carries the
  // empty sequence (DER forbids encoding DEFAULT FALSE explicitly).
  expect(ca_text.find(std::string(
             "\x06\x03\x55\x1d\x13\x01\x01\xff\x04\x05\x30\x03\x01\x01\xff",
             15)) != std::string::npos,
         "CA basic constraints CA:TRUE");
  expect(der_text.find(std::string(
             "\x06\x03\x55\x1d\x13\x01\x01\xff\x04\x02\x30\x00", 12)) !=
             std::string::npos,
         "leaf basic constraints is the empty sequence");

  // Key usage as minimized BIT STRINGs (unused-bits count included).
  expect(ca_text.find(std::string(
             "\x06\x03\x55\x1d\x0f\x01\x01\xff\x04\x04\x03\x02\x01\x86",
             14)) != std::string::npos,
         "CA key usage digitalSignature+keyCertSign+cRLSign");
  expect(der_text.find(std::string(
             "\x06\x03\x55\x1d\x0f\x01\x01\xff\x04\x04\x03\x02\x07\x80",
             14)) != std::string::npos,
         "leaf key usage digitalSignature");

  // Subject/authority key identifiers derived from the public point.
  const Bytes key_id = ec_key_identifier(point);
  expect_eq(key_id.size(), std::size_t{20}, "key identifier length");
  const std::string key_id_text(
      reinterpret_cast<const char*>(key_id.data()), key_id.size());
  expect(ca_text.find(std::string("\x06\x03\x55\x1d\x0e", 5)) !=
             std::string::npos,
         "CA carries SKID");
  expect(ca_text.find(std::string("\x06\x03\x55\x1d\x23", 5)) !=
             std::string::npos,
         "self-signed CA carries AKID");
  expect(count_occurrences(ca_text, key_id_text) >= 2,
         "CA key id appears as SKID and AKID");
  expect(count_occurrences(der_text, key_id_text) >= 2,
         "leaf embeds the authority key id");

  // P1363 to DER: r = 1, s = all ones (the high bit forces a sign pad).
  Bytes p1363(64, 0x00);
  p1363[31] = 0x01;
  for (std::size_t i = 32; i < 64; ++i) p1363[i] = 0xff;
  auto der_sig = ecdsa_signature_p1363_to_der(p1363);
  expect(der_sig.has_value(), "P1363 signature converts");
  if (der_sig) {
    Bytes expected = {0x30, 0x26, 0x02, 0x01, 0x01, 0x02, 0x21, 0x00};
    expected.insert(expected.end(), 32, 0xff);
    expect_bytes(der_sig.value(), expected, "P1363 to DER known answer");
  }
  auto zero_sig = ecdsa_signature_p1363_to_der(Bytes(64, 0x00));
  expect(zero_sig.has_value(), "zero P1363 converts");
  if (zero_sig) {
    expect_bytes(zero_sig.value(),
                 (Bytes{0x30, 0x06, 0x02, 0x01, 0x00, 0x02, 0x01, 0x00}),
                 "zero P1363 known answer");
  }
  expect(!ecdsa_signature_p1363_to_der({}).has_value(),
         "empty P1363 rejected");
  expect(!ecdsa_signature_p1363_to_der({0x01, 0x02, 0x03}).has_value(),
         "odd-length P1363 rejected");

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

// ---------------------------------------------------------------------------
// Software SAP (known-answer tests)
//
// The vectors below are synthetic inputs whose expected outputs are fixed by
// the protocol algorithm; none of them was issued by a server. They pin the
// SHA-1/HMAC/AES primitives, the cipher transform, the F2 derivation, and the
// complete setup handshake byte for byte.
// ---------------------------------------------------------------------------

ipashelf::Bytes hex_bytes(std::string_view text) {
  auto decoded = ipashelf::hex_decode(text);
  if (!decoded) return {};
  return decoded.value();
}

std::string hex_of(const ipashelf::Bytes& bytes) {
  return ipashelf::hex_encode(bytes);
}

template <std::size_t Size>
std::string hex_of(const std::array<std::uint8_t, Size>& bytes) {
  return hex_of(ipashelf::Bytes(bytes.begin(), bytes.end()));
}

void test_crypto_primitives() {
  expect_eq(ipashelf::sha1_hex("abc", 3),
            std::string("a9993e364706816aba3e25717850c26c9cd0d89d"),
            "SHA-1 of abc");
  expect_eq(ipashelf::sha1_hex("", 0),
            std::string("da39a3ee5e6b4b0d3255bfef95601890afd80709"),
            "SHA-1 of empty input");
  const std::string long_text(1000000, 'a');
  expect_eq(ipashelf::sha1_hex(long_text.data(), long_text.size()),
            std::string("34aa973cd4c4daa4f61eeb2bdbad27316534016f"),
            "SHA-1 of one million a characters");

  const ipashelf::Bytes hmac_key(20, 0x0b);
  expect_eq(hex_of(ipashelf::hmac_sha1(hmac_key, "Hi There", 8)),
            std::string("b617318655057264e28bc0b6fb378c8ef146be00"),
            "HMAC-SHA1 RFC 2202 case 1");
  expect_eq(hex_of(ipashelf::hmac_sha1(bytes_of("Jefe"),
                                       "what do ya want for nothing?", 28)),
            std::string("effcdf6ae5eb2fa2d27416d5f184df9c259a7c79"),
            "HMAC-SHA1 RFC 2202 case 2");

  const ipashelf::Bytes key = hex_bytes("000102030405060708090a0b0c0d0e0f");
  const ipashelf::Bytes plain = hex_bytes("00112233445566778899aabbccddeeff");
  std::array<std::uint8_t, 16> aes_key{};
  std::copy(key.begin(), key.end(), aes_key.begin());
  const ipashelf::Aes128 aes(aes_key);
  std::uint8_t cipher_block[16];
  aes.encrypt_block(plain.data(), cipher_block);
  expect_eq(hex_of(ipashelf::Bytes(cipher_block, cipher_block + 16)),
            std::string("69c4e0d86a7b0430d8cdb78070b4c55a"),
            "AES-128 FIPS-197 encryption");
  std::uint8_t round_trip[16];
  aes.decrypt_block(cipher_block, round_trip);
  expect_bytes(ipashelf::Bytes(round_trip, round_trip + 16), plain,
               "AES-128 decryption inverts encryption");

  const ipashelf::Bytes cbc_key = hex_bytes("2b7e151628aed2a6abf7158809cf4f3c");
  const ipashelf::Bytes cbc_iv = hex_bytes("000102030405060708090a0b0c0d0e0f");
  const ipashelf::Bytes cbc_plain =
      hex_bytes("6bc1bee22e409f96e93d7e117393172a"
                "ae2d8a571e03ac9c9eb76fac45af8e51");
  std::array<std::uint8_t, 16> cbc_key_array{};
  std::array<std::uint8_t, 16> cbc_iv_array{};
  std::copy(cbc_key.begin(), cbc_key.end(), cbc_key_array.begin());
  std::copy(cbc_iv.begin(), cbc_iv.end(), cbc_iv_array.begin());
  auto cbc_encrypted =
      ipashelf::aes128_cbc_encrypt(cbc_plain, cbc_key_array, cbc_iv_array);
  expect(cbc_encrypted.has_value(), "AES-CBC encryption succeeds");
  if (cbc_encrypted) {
    expect_eq(hex_of(cbc_encrypted.value()),
              std::string("7649abac8119b246cee98e9b12e9197d"
                          "5086cb9b507219ee95db113a917678b2"),
              "AES-CBC SP 800-38A vector");
    auto cbc_decrypted = ipashelf::aes128_cbc_decrypt(
        cbc_encrypted.value(), cbc_key_array, cbc_iv_array);
    expect(cbc_decrypted.has_value(), "AES-CBC decryption succeeds");
    if (cbc_decrypted) {
      expect_bytes(cbc_decrypted.value(), cbc_plain,
                   "AES-CBC decryption round trip");
    }
  }
  expect(!ipashelf::aes128_cbc_encrypt(ipashelf::Bytes(3, 0), cbc_key_array,
                                       cbc_iv_array),
         "AES-CBC rejects unaligned input");

  auto random = ipashelf::random_bytes(32);
  expect(random.has_value() && random.value().size() == 32,
         "OS random bytes are available");
}

struct XorShift32 {
  std::uint32_t state;
  std::uint32_t word() {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
  }
  ipashelf::Bytes bytes(std::size_t count) {
    ipashelf::Bytes output(count);
    for (std::uint8_t& byte : output) {
      byte = static_cast<std::uint8_t>(word() & 0xff);
    }
    return output;
  }
};

ipashelf::SapSessionData synthetic_session() {
  XorShift32 random{0x53595031};
  ipashelf::SapSessionData session;
  session.source = random.bytes(576);
  session.source[0xfa] = static_cast<std::uint8_t>(session.source[0xca] ^ 10);
  session.source[0xf3] =
      static_cast<std::uint8_t>(session.source[0xc3] ^ 147);
  const ipashelf::Bytes input1 = random.bytes(16);
  const ipashelf::Bytes input2 = random.bytes(16);
  std::copy(input1.begin(), input1.end(), session.f2_input1.begin());
  std::copy(input2.begin(), input2.end(), session.f2_input2.begin());
  session.f2_tail = random.bytes(7);
  const std::string label = "SYNTHETIC TEST SESSION / NEVER SERVER-ISSUED / ";
  session.f3 = bytes_of(label);
  const ipashelf::Bytes rest = random.bytes(159 - label.size());
  session.f3.insert(session.f3.end(), rest.begin(), rest.end());
  for (std::uint32_t& word : session.random_words) word = random.word();
  session.seed30 = random.word();
  session.seed54 = random.word();
  session.random_cursor = 624;
  session.f1_material_address = 0x1234500044ULL;
  session.iv_address = 0x0FEED00050ULL;
  return session;
}

void test_sap_software_synthetic() {
  const ipashelf::SapSessionData session = synthetic_session();
  expect_eq(ipashelf::sha256_hex(session.source.data(), session.source.size()),
            std::string("bc89fa33012fa9d8639c174bc5fca3b9e037da8b91a1d42b0c82"
                        "992963fd315a"),
            "synthetic session source hash");

  auto head =
      ipashelf::sap_software_f2_head(session.f2_input1, session.f2_input2);
  expect(head.has_value(), "F2 derivation succeeds");
  if (head) {
    expect_eq(hex_of(head.value()),
              std::string("ad1adda4bad5ada37fd54543681d001c"),
              "F2 head known answer");
  }

  auto cipher = ipashelf::SapSoftwareCipher::create(session.source);
  expect(cipher.has_value(), "SAP cipher preparation succeeds");
  if (cipher) {
    std::array<std::uint8_t, 16> zeros{};
    std::array<std::uint8_t, 16> sequence{};
    std::array<std::uint8_t, 16> ones{};
    for (int index = 0; index < 16; ++index) {
      sequence[index] = static_cast<std::uint8_t>(index);
      ones[index] = 0xff;
    }
    expect_eq(hex_of(cipher.value()->encrypt(zeros)),
              std::string("8fcdc9e6d7c5009c7f489288231582de"),
              "E transform of the zero block");
    expect_eq(hex_of(cipher.value()->encrypt(sequence)),
              std::string("65810b6c7555809e4b87529110631a4c"),
              "E transform of the sequence block");
    expect_eq(hex_of(cipher.value()->encrypt(ones)),
              std::string("73d2bd4f671e9abe8b105aac1883036d"),
              "E transform of the 0xff block");
  }

  auto signer = ipashelf::SapSoftwareSigner::create(session, 7);
  expect(signer.has_value(), "SAP signer creation succeeds");
  if (!signer) return;
  ipashelf::Bytes second_body = bytes_of("synthetic SAP\nbody-2");
  second_body.push_back(0);
  second_body.push_back(0xff);
  ipashelf::Bytes third_body(256);
  for (int index = 0; index < 256; ++index) {
    third_body[index] = static_cast<std::uint8_t>(index);
  }
  const ipashelf::Bytes bodies[3] = {ipashelf::Bytes{}, second_body,
                                     third_body};
  const char* expected_hashes[3] = {
      "ba76e674c73d9db75c2986233016b94b22e062556fd029d71733726519756517",
      "4acc6269de932c753260fecfa375a6405b5b63cf621a8adace3bd7b5f0305287",
      "853914149c9de004852fffa75a4489c3cdec3f179c92903e3d94b8b2d494e4e4"};
  ipashelf::Bytes signatures[3];
  for (int index = 0; index < 3; ++index) {
    auto signature = signer.value()->sign(bodies[index]);
    expect(signature.has_value(), "SAP signing succeeds");
    if (!signature) return;
    signatures[index] = signature.value();
    expect_eq(signatures[index].size(), std::size_t{501},
              "SAP signature length");
    expect_eq(ipashelf::sha256_hex(signatures[index].data(),
                                   signatures[index].size()),
              std::string(expected_hashes[index]),
              "SAP signature known answer");
  }
  expect(std::equal(signatures[0].end() - 464, signatures[0].end(),
                    signatures[1].end() - 464),
         "signing blob is cached across calls");
  expect(!std::equal(signatures[0].begin() + 1, signatures[0].begin() + 17,
                     signatures[1].begin() + 1),
         "each signature consumes a fresh IV");

  auto replay = ipashelf::SapSoftwareSigner::create(session, 7);
  expect(replay.has_value(), "signer replay creation succeeds");
  if (replay) {
    auto again = replay.value()->sign(bodies[0]);
    expect(again.has_value(), "signer replay succeeds");
    if (again) {
      expect_bytes(again.value(), signatures[0],
                   "signing is deterministic for a fresh session copy");
    }
  }
}

void test_sap_software_handshake(const std::string& fixtures) {
  const ipashelf::Bytes raw =
      read_file(fixtures + "/sap_handshake_fixture.json");
  expect(!raw.empty(), "SAP handshake fixture is present");
  if (raw.empty()) return;
  auto parsed = ipashelf::Json::parse(text_of(raw));
  expect(parsed.has_value(), "SAP handshake fixture parses");
  if (!parsed) return;
  const ipashelf::Json& root = parsed.value();
  const ipashelf::Json* certificate = root.find("certificate");
  const ipashelf::Json* cases = root.find("cases");
  expect(certificate != nullptr && certificate->is_string() &&
             cases != nullptr && cases->is_array(),
         "SAP handshake fixture shape");
  if (certificate == nullptr || cases == nullptr) return;
  const ipashelf::Bytes certificate_bytes =
      hex_bytes(certificate->string_value());
  expect_eq(certificate_bytes.size(), std::size_t{783},
            "fixture certificate length");

  const auto u32_field = [](const ipashelf::Json& object,
                            std::string_view key) {
    const ipashelf::Json* value = object.find(key);
    if (value == nullptr) return std::uint32_t{0};
    return static_cast<std::uint32_t>(value->int_value());
  };
  const auto hex_field = [](const ipashelf::Json& object,
                            std::string_view key) {
    const ipashelf::Json* value = object.find(key);
    if (value == nullptr || !value->is_string()) return ipashelf::Bytes{};
    return hex_bytes(value->string_value());
  };

  std::optional<std::size_t> first_case;
  for (std::size_t index = 0; index < cases->array_items().size(); ++index) {
    const ipashelf::Json& vector = cases->array_items()[index];
    const ipashelf::Json* entropy = vector.find("entropy");
    expect(entropy != nullptr && entropy->is_array() &&
               entropy->array_items().size() == 3,
           "fixture entropy shape");
    if (entropy == nullptr || entropy->array_items().size() != 3) continue;
    std::array<std::uint32_t, 3> entropy_words{};
    for (int word = 0; word < 3; ++word) {
      entropy_words[word] = static_cast<std::uint32_t>(
          entropy->array_items()[word].int_value());
    }
    const ipashelf::Json* seconds = vector.find("seconds");
    auto handshake = ipashelf::SapSoftwareHandshake::create_deterministic(
        certificate_bytes, entropy_words,
        seconds != nullptr ? seconds->int_value() : 0,
        u32_field(vector, "microseconds"));
    expect(handshake.has_value(), "handshake construction succeeds");
    if (!handshake) continue;
    expect_bytes(handshake.value()->request(), hex_field(vector, "request"),
                 "handshake request known answer");
    const std::array<std::uint8_t, 16> response_key =
        handshake.value()->response_key();
    const std::array<std::uint8_t, 16> response_echo =
        handshake.value()->response_echo();
    expect_bytes(ipashelf::Bytes(response_key.begin(), response_key.end()),
                 hex_field(vector, "responseKey"),
                 "handshake response key known answer");
    expect_bytes(ipashelf::Bytes(response_echo.begin(), response_echo.end()),
                 hex_field(vector, "responseEcho"),
                 "handshake response echo known answer");

    auto session = handshake.value()->complete(hex_field(vector, "reply"));
    expect(session.has_value(), "handshake completion succeeds");
    if (!session) continue;
    ipashelf::Bytes words_bytes;
    for (std::uint32_t word : session.value().random_words) {
      for (int byte = 0; byte < 4; ++byte) {
        words_bytes.push_back(
            static_cast<std::uint8_t>((word >> (8 * byte)) & 0xff));
      }
    }
    const ipashelf::Json* random_hash = vector.find("randomHash");
    expect_eq(ipashelf::sha256_hex(words_bytes.data(), words_bytes.size()),
              random_hash != nullptr ? random_hash->string_value()
                                     : std::string{},
              "handshake random state hash");
    expect_eq(session.value().random_cursor, u32_field(vector, "cursor"),
              "handshake random cursor");
    expect_eq(session.value().seed30, u32_field(vector, "seed30"),
              "handshake seed30");
    expect_eq(session.value().seed54, u32_field(vector, "seed54"),
              "handshake seed54");
    expect_bytes(ipashelf::Bytes(session.value().f2_input2.begin(),
                                 session.value().f2_input2.end()),
                 hex_field(vector, "f2Input2"), "handshake F2 input 2");
    expect_bytes(session.value().f2_tail, hex_field(vector, "f2Tail"),
                 "handshake F2 tail");
    ipashelf::Bytes expected_source(576);
    for (int i = 0; i < 576; ++i) {
      expected_source[i] = static_cast<std::uint8_t>((i * 17 + 3) & 0xff);
    }
    expect_bytes(session.value().source, expected_source,
                 "handshake source material");
    ipashelf::Bytes expected_input1(16);
    for (int i = 0; i < 16; ++i) {
      expected_input1[i] = static_cast<std::uint8_t>(32 + i + 0x2d);
    }
    expect_bytes(ipashelf::Bytes(session.value().f2_input1.begin(),
                                 session.value().f2_input1.end()),
                 expected_input1, "handshake F2 input 1");
    expect_eq(session.value().f3.size(), std::size_t{159},
              "handshake signature record length");
    if (!first_case) first_case = index;

    // A truncated or foreign reply must be rejected, never signed with.
    const ipashelf::Bytes reply = hex_field(vector, "reply");
    expect(!handshake.value()->complete(ipashelf::Bytes{}),
           "empty reply is rejected");
    expect(!handshake.value()->complete(
               ipashelf::Bytes(reply.begin(), reply.begin() + 21)),
           "truncated reply is rejected");
    expect(!handshake.value()->complete(
               ipashelf::Bytes(reply.begin(), reply.end() - 1)),
           "short reply is rejected");
  }
}

void test_sap_software_backend() {
  auto backend = ipashelf::create_platform_sap_backend();
  expect(backend.has_value(), "platform SAP backend is the software backend");
  if (!backend) return;
  const ipashelf::Bytes hardware = {0xde, 0xad, 0xbe, 0xef, 0x00, 0x11};
  auto context = backend.value()->initialize(hardware);
  expect(context.has_value() && context.value() != 0,
         "software backend initializes");
  if (!context) return;
  auto exchange = backend.value()->exchange(200, hardware, context.value(),
                                            ipashelf::Bytes{});
  expect(!exchange, "exchange rejects a missing certificate container");
  auto sign = backend.value()->sign(context.value(), ipashelf::Bytes{1, 2, 3});
  expect(!sign, "signing before setup completion fails");
  expect(backend.value()->teardown(context.value()).ok(),
         "software backend tears down");
  expect(backend.value()->teardown(context.value()).ok(),
         "teardown is idempotent");
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: test_core <fixtures-dir>\n";
    return 2;
  }
  const std::string fixtures = argv[1];

  test_crypto();
  test_crypto_primitives();
  test_sap_software_synthetic();
  test_sap_software_handshake(fixtures);
  test_sap_software_backend();
  test_json();
  test_plist_xml();
  test_plist_envelope();
  test_plist_binary(fixtures + "/sample.bplist");
  test_plist_binary_write(fixtures);
  test_account_json();
  test_ipa_patch(fixtures);
  test_url();
  test_bag();
  test_login_success();
  test_login_bag_envelope();
  test_login_two_factor();
  test_login_bad_password();
  test_search_and_lookup();
  test_latest_external_version();
  test_catalog_throttling();
  test_download_unpinned_when_catalog_throttled();
  test_download_offer();
  test_download_fallback();
  test_download_redirect_replay();
  test_purchase();
  test_purchase_license_exists();
  test_version_identifiers();
  test_download_parsing();
  test_phone_storefront();
  test_login_phone_storefront_header();
  test_login_redirects();
  test_ent_download();
  test_ent_download_fallbacks();
  test_owned_apps();
  test_version_metadata(fixtures);
  test_map_store_failure();
  test_storefronts();
  test_app_details();
  test_search_filters();
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
