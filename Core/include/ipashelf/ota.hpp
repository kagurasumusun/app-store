// SPDX-License-Identifier: MIT
//
// On-demand installation support for IPAShelf.
//
// Apple's over-the-air installation flow works like this:
//
//   1. The device opens `itms-services://?action=download-manifest&url=<plist>`.
//   2. `installd` fetches the manifest over HTTPS from a trusted origin.
//   3. The manifest names the IPA, a small icon and a large icon.
//   4. `installd` downloads the IPA and installs it.
//
// The manifest itself is a property list. Everything in this header is
// portable: the iOS bridge terminates TLS with Network.framework and feeds the
// decrypted bytes to `HttpRequestStream`, then streams the response produced by
// `OtaRouter`. The same router is exercised over a real loopback socket by the
// unit tests, so the parsing and range logic are covered on any host.

#pragma once

#include <cstdint>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ipashelf/base.hpp"
#include "ipashelf/http.hpp"
#include "ipashelf/plist.hpp"

namespace ipashelf {

// ---------------------------------------------------------------------------
// Manifest
// ---------------------------------------------------------------------------

struct OtaManifest {
  std::string bundle_id;
  std::string display_version;
  std::string title;
  std::string payload_url;
  std::string small_icon_url;
  std::string large_icon_url;
};

/// Builds the `items` property list consumed by `installd`.
[[nodiscard]] PlistValue build_install_manifest(const OtaManifest& manifest);

/// The same document serialised as an XML property list (UTF-8).
[[nodiscard]] std::string build_install_manifest_xml(
    const OtaManifest& manifest);

/// Wraps a manifest URL in the `itms-services` scheme. The URL is
/// percent-encoded as a query value.
[[nodiscard]] std::string build_itms_services_url(
    std::string_view manifest_url);

/// A small HTML landing page with the install link and an optional link to the
/// root certificate the user must trust first. Written for Safari on iOS.
/// `install_link` is the full `itms-services` URL of the manifest (see
/// `build_itms_services_url`); the page is often served over plain HTTP while
/// the manifest stays on the HTTPS listener.
[[nodiscard]] std::string build_install_page(std::string_view title,
                                             std::string_view bundle_id,
                                             std::string_view display_version,
                                             std::string_view install_link,
                                             std::string_view certificate_url);

// ---------------------------------------------------------------------------
// Server-side HTTP
// ---------------------------------------------------------------------------

struct HttpServerRequest {
  std::string method;
  std::string target;
  std::string path;
  std::string query;
  int minor_version = 1;
  HttpHeaders headers;
  Bytes body;
  bool keep_alive = true;

  [[nodiscard]] std::string header(std::string_view name,
                                   std::string fallback = {}) const {
    return headers.get_or(name, std::move(fallback));
  }
};

/// Incremental HTTP/1.x request parser. Only the subset needed by the install
/// server is accepted: GET, HEAD and OPTIONS, no request bodies, no chunked
/// transfer encoding. That is exactly what `installd`, Safari and `curl` send.
class HttpRequestStream {
 public:
  explicit HttpRequestStream(std::size_t max_header_bytes = 64 * 1024,
                             std::size_t max_body_bytes = 1024 * 1024);

  void append(const void* data, std::size_t size);
  void append(std::string_view text);

  /// Returns the next complete request. `std::nullopt` means the parser needs
  /// more bytes. An error means the peer sent something we cannot serve.
  [[nodiscard]] std::optional<Result<HttpServerRequest>> next();

  [[nodiscard]] std::size_t buffered() const { return buffer_.size(); }
  void clear() { buffer_.clear(); }

 private:
  std::string buffer_;
  std::size_t max_header_bytes_ = 64 * 1024;
  std::size_t max_body_bytes_ = 1024 * 1024;
};

struct ByteRange {
  bool partial = false;
  std::uint64_t start = 0;
  std::uint64_t length = 0;
};

/// Parses a `Range` header against a representation of `size` bytes.
/// An empty header selects the whole representation. Unsatisfiable ranges are
/// reported as `kInvalidArgument` so the caller can answer 416.
[[nodiscard]] Result<ByteRange> parse_range_header(std::string_view header,
                                                   std::uint64_t size);

struct HttpServerResponse {
  int status = 200;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
  std::string file_path;
  std::uint64_t file_offset = 0;
  std::uint64_t file_length = 0;
  bool head_only = false;

  [[nodiscard]] bool has_file() const { return !file_path.empty(); }
  void set(std::string name, std::string value);
  [[nodiscard]] std::string header(std::string_view name) const;

  /// Serialises the status line and headers, including `Content-Length`.
  /// `date` is used verbatim for the `Date` header when it is not empty.
  [[nodiscard]] std::string head(std::string_view date = {}) const;
};

[[nodiscard]] std::string reason_phrase(int status);

/// Formats an IMF-fixdate (`Sun, 06 Nov 1994 08:49:37 GMT`).
[[nodiscard]] std::string format_http_date(std::time_t value);

// ---------------------------------------------------------------------------
// Router
// ---------------------------------------------------------------------------

struct OtaPackage {
  std::string bundle_id;
  std::string display_version;
  std::string title;
  /// Absolute path of the finished IPA.
  std::string payload_path;
  /// Absolute path of a PNG used for both icon endpoints when only one exists.
  std::string icon_small_path;
  std::string icon_large_path;
  /// Absolute path of the DER-encoded root certificate, when one is served.
  std::string certificate_path;
  /// In-memory `.mobileconfig` profile embedding the root certificate, when
  /// one should be served over plain HTTP for the user to install.
  std::string profile_data;
  std::uint64_t payload_size = 0;
};

struct OtaRoutes {
  std::string index = "/";
  std::string manifest = "/install.plist";
  std::string payload = "/payload.ipa";
  std::string icon_small = "/icon57.png";
  std::string icon_large = "/icon512.png";
  std::string certificate = "/ca.cer";
  std::string profile = "/profile.mobileconfig";
  std::string ping = "/ping";
};

class OtaRouter {
 public:
  OtaRouter(std::string base_url, OtaPackage package,
            OtaRoutes routes = {});

  [[nodiscard]] HttpServerResponse route(
      const HttpServerRequest& request) const;

  [[nodiscard]] std::string manifest_url() const;
  [[nodiscard]] std::string install_url() const;
  /// Liveness endpoint. The app fetches it over HTTPS with the system
  /// trust store before opening the install link, proving the listener,
  /// the TLS identity and the root trust the way `installd` will see
  /// them.
  [[nodiscard]] std::string ping_url() const;
  [[nodiscard]] const std::string& base_url() const { return base_url_; }
  [[nodiscard]] const OtaPackage& package() const { return package_; }
  [[nodiscard]] const OtaRoutes& routes() const { return routes_; }

  /// Replaces the local file used for the payload (for example after a new
  /// version finished downloading while the server was running).
  void set_package(OtaPackage package) { package_ = std::move(package); }

  /// Overrides the links shown on the landing page. The certificate server is
  /// served over plain HTTP before the root certificate is trusted, but the
  /// install link it shows must still point at the HTTPS listener.
  void set_page_links(std::string install_link, std::string certificate_link) {
    page_install_link_ = std::move(install_link);
    page_certificate_link_ = std::move(certificate_link);
  }

 private:
  [[nodiscard]] HttpServerResponse file_response(const std::string& path,
                                                 std::string_view content_type,
                                                 const HttpServerRequest& request,
                                                 std::uint64_t size) const;
  [[nodiscard]] HttpServerResponse error_response(int status,
                                                  std::string message) const;

  std::string base_url_;
  OtaPackage package_;
  OtaRoutes routes_;
  std::string page_install_link_;
  std::string page_certificate_link_;
};

// ---------------------------------------------------------------------------
// Plain HTTP connection serving
// ---------------------------------------------------------------------------

/// Serves exactly one request from `fd` using `router`, then returns. Used for
/// the certificate/help page, which must be reachable before the root
/// certificate is trusted and therefore cannot use TLS.
///
/// Declared only when POSIX sockets are available.
#if defined(__unix__) || defined(__APPLE__) || defined(_POSIX_VERSION)
[[nodiscard]] Status serve_http_connection(int fd, const OtaRouter& router,
                                           const CancellationToken& token);
#endif

}  // namespace ipashelf
