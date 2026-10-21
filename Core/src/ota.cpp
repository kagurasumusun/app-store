// SPDX-License-Identifier: MIT

#include "ipashelf/ota.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "ipashelf/url.hpp"

#if defined(__unix__) || defined(__APPLE__) || defined(_POSIX_VERSION)
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace ipashelf {
namespace {

constexpr std::size_t kChunkSize = 64 * 1024;

std::string strip_cr(std::string_view value) {
  if (!value.empty() && value.back() == '\r') value.remove_suffix(1);
  return std::string(value);
}

bool valid_method(std::string_view method) {
  return method == "GET" || method == "HEAD" || method == "OPTIONS";
}

std::uint64_t file_size_of(const std::string& path) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error) return 0;
  return static_cast<std::uint64_t>(size);
}

std::string escape_html(std::string_view value) {
  std::string output;
  output.reserve(value.size());
  for (const char character : value) {
    switch (character) {
      case '&':
        output += "&amp;";
        break;
      case '<':
        output += "&lt;";
        break;
      case '>':
        output += "&gt;";
        break;
      case '"':
        output += "&quot;";
        break;
      case '\'':
        output += "&#39;";
        break;
      default:
        output.push_back(character);
    }
  }
  return output;
}

}  // namespace

// ---------------------------------------------------------------------------
// Manifest
// ---------------------------------------------------------------------------

PlistValue build_install_manifest(const OtaManifest& manifest) {
  PlistValue assets = PlistValue::array();
  PlistValue package = PlistValue::dictionary();
  package.set("kind", PlistValue::string("software-package"));
  package.set("url", PlistValue::string(manifest.payload_url));
  assets.append(std::move(package));

  if (!manifest.small_icon_url.empty()) {
    PlistValue icon = PlistValue::dictionary();
    icon.set("kind", PlistValue::string("display-image"));
    icon.set("url", PlistValue::string(manifest.small_icon_url));
    assets.append(std::move(icon));
  }
  if (!manifest.large_icon_url.empty()) {
    PlistValue icon = PlistValue::dictionary();
    icon.set("kind", PlistValue::string("full-size-image"));
    icon.set("url", PlistValue::string(manifest.large_icon_url));
    assets.append(std::move(icon));
  }

  PlistValue metadata = PlistValue::dictionary();
  metadata.set("bundle-identifier", PlistValue::string(manifest.bundle_id));
  metadata.set("bundle-version", PlistValue::string(manifest.display_version));
  metadata.set("kind", PlistValue::string("software"));
  metadata.set("title", PlistValue::string(manifest.title));

  PlistValue item = PlistValue::dictionary();
  item.set("assets", std::move(assets));
  item.set("metadata", std::move(metadata));

  PlistValue items = PlistValue::array();
  items.append(std::move(item));

  PlistValue root = PlistValue::dictionary();
  root.set("items", std::move(items));
  return root;
}

std::string build_install_manifest_xml(const OtaManifest& manifest) {
  return build_install_manifest(manifest).dump_xml();
}

std::string build_itms_services_url(std::string_view manifest_url) {
  return "itms-services://?action=download-manifest&url=" +
         percent_encode(manifest_url);
}

std::string build_install_page(std::string_view page_title,
                               std::string_view bundle_id,
                               std::string_view display_version,
                               std::string_view install_link,
                               std::string_view certificate_url) {
  const std::string install = escape_html(install_link);
  const std::string title = escape_html(page_title);
  std::string page;
  page.reserve(1200);
  page += "<!doctype html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">";
  page += "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">";
  page += "<title>" + title + "</title><style>";
  page += "body{font:17px -apple-system,system-ui,sans-serif;margin:0;";
  page += "padding:32px 20px;background:#f7f7fb;color:#111}";
  page += "main{max-width:520px;margin:0 auto;background:#fff;border-radius:18px;";
  page += "padding:24px;box-shadow:0 8px 30px rgba(0,0,0,.08)}";
  page += "h1{font-size:22px;margin:0 0 6px}p{color:#555;line-height:1.45}";
  page += "a.button{display:block;text-align:center;margin:22px 0 12px;padding:14px;";
  page += "border-radius:12px;background:#4b4bd9;color:#fff;text-decoration:none;";
  page += "font-weight:600}code{background:#f0f0f5;padding:2px 6px;border-radius:6px}";
  page += "</style></head><body><main>";
  page += "<h1>" + title + "</h1>";
  if (!display_version.empty() || !bundle_id.empty()) {
    page += "<p>";
    if (!display_version.empty()) {
      page += "Version " + escape_html(display_version);
    }
    if (!bundle_id.empty()) {
      if (!display_version.empty()) page += " &middot; ";
      page += "<code>" + escape_html(bundle_id) + "</code>";
    }
    page += "</p>";
  }
  page += "<a class=\"button\" href=\"" + install + "\">Install on this device</a>";
  if (!certificate_url.empty()) {
    page += "<p>First install and trust the IPAShelf root certificate, then return "
            "here: <a href=\"" +
            escape_html(certificate_url) + "\">download certificate</a>. After the "
            "profile is installed, enable it under Settings &rsaquo; General "
            "&rsaquo; About &rsaquo; Certificate Trust Settings.</p>";
  }
  page += "<p>Installation is handled by iOS. Keep IPAShelf open while the "
          "download runs.</p>";
  page += "</main></body></html>\n";
  return page;
}

// ---------------------------------------------------------------------------
// Request parsing
// ---------------------------------------------------------------------------

HttpRequestStream::HttpRequestStream(std::size_t max_header_bytes,
                                     std::size_t max_body_bytes)
    : max_header_bytes_(max_header_bytes),
      max_body_bytes_(max_body_bytes) {}

void HttpRequestStream::append(const void* data, std::size_t size) {
  buffer_.append(static_cast<const char*>(data), size);
}

void HttpRequestStream::append(std::string_view text) {
  buffer_.append(text.data(), text.size());
}

std::optional<Result<HttpServerRequest>> HttpRequestStream::next() {
  if (buffer_.empty()) return std::nullopt;

  std::size_t header_end = buffer_.find("\r\n\r\n");
  std::size_t separator = 4;
  if (header_end == std::string::npos) {
    header_end = buffer_.find("\n\n");
    separator = 2;
  }
  if (header_end == std::string::npos) {
    if (buffer_.size() > max_header_bytes_) {
      return Result<HttpServerRequest>(
          make_error(ErrorCode::kParse, "request headers are too large"));
    }
    return std::nullopt;
  }
  if (header_end > max_header_bytes_) {
    return Result<HttpServerRequest>(
        make_error(ErrorCode::kParse, "request headers are too large"));
  }

  const std::string block = buffer_.substr(0, header_end);
  std::vector<std::string> lines;
  std::size_t position = 0;
  while (position <= block.size()) {
    const std::size_t newline = block.find('\n', position);
    if (newline == std::string::npos) {
      lines.push_back(strip_cr(std::string_view(block).substr(position)));
      break;
    }
    lines.push_back(strip_cr(std::string_view(block).substr(position,
                                                            newline - position)));
    position = newline + 1;
  }
  if (lines.empty() || lines.front().empty()) {
    buffer_.erase(0, header_end + separator);
    return Result<HttpServerRequest>(
        make_error(ErrorCode::kParse, "request line is missing"));
  }

  const std::string& request_line = lines.front();
  const std::size_t first_space = request_line.find(' ');
  const std::size_t second_space =
      first_space == std::string::npos
          ? std::string::npos
          : request_line.find(' ', first_space + 1);
  if (first_space == std::string::npos || second_space == std::string::npos) {
    buffer_.erase(0, header_end + separator);
    return Result<HttpServerRequest>(
        make_error(ErrorCode::kParse, "request line is malformed"));
  }

  HttpServerRequest request;
  request.method = request_line.substr(0, first_space);
  request.target = request_line.substr(first_space + 1,
                                       second_space - first_space - 1);
  const std::string version = request_line.substr(second_space + 1);
  if (!valid_method(request.method)) {
    buffer_.erase(0, header_end + separator);
    return Result<HttpServerRequest>(Error{
        ErrorCode::kUnsupported, "only GET, HEAD and OPTIONS are supported",
        405});
  }
  if (request.target.empty() || request.target.front() != '/') {
    buffer_.erase(0, header_end + separator);
    return Result<HttpServerRequest>(
        make_error(ErrorCode::kParse, "request target must be absolute"));
  }
  if (version != "HTTP/1.1" && version != "HTTP/1.0") {
    buffer_.erase(0, header_end + separator);
    return Result<HttpServerRequest>(
        make_error(ErrorCode::kParse, "unsupported HTTP version"));
  }
  request.minor_version = version == "HTTP/1.0" ? 0 : 1;

  for (std::size_t index = 1; index < lines.size(); ++index) {
    const std::string& line = lines[index];
    if (line.empty()) continue;
    if (line.front() == ' ' || line.front() == '\t') {
      buffer_.erase(0, header_end + separator);
      return Result<HttpServerRequest>(
          make_error(ErrorCode::kParse, "folded headers are not supported"));
    }
    const std::size_t colon = line.find(':');
    if (colon == std::string::npos) {
      buffer_.erase(0, header_end + separator);
      return Result<HttpServerRequest>(
          make_error(ErrorCode::kParse, "header line has no colon"));
    }
    request.headers.add(trim(std::string_view(line).substr(0, colon)),
                        trim(std::string_view(line).substr(colon + 1)));
  }

  const std::string connection = to_lower(request.header("Connection"));
  request.keep_alive = request.minor_version == 1 ? connection != "close"
                                                  : connection == "keep-alive";

  std::size_t body_size = 0;
  const std::string length = request.header("Content-Length");
  if (!length.empty()) {
    char* end = nullptr;
    const long long parsed = std::strtoll(length.c_str(), &end, 10);
    if (end == nullptr || *end != '\0' || parsed < 0) {
      buffer_.erase(0, header_end + separator);
      return Result<HttpServerRequest>(
          make_error(ErrorCode::kParse, "invalid Content-Length"));
    }
    body_size = static_cast<std::size_t>(parsed);
    if (body_size > max_body_bytes_) {
      buffer_.erase(0, header_end + separator);
      return Result<HttpServerRequest>(
          make_error(ErrorCode::kParse, "request body is too large"));
    }
  }
  if (request.headers.contains("Transfer-Encoding")) {
    buffer_.erase(0, header_end + separator);
    return Result<HttpServerRequest>(
        make_error(ErrorCode::kUnsupported, "chunked requests are not supported"));
  }

  const std::size_t total = header_end + separator + body_size;
  if (buffer_.size() < total) return std::nullopt;

  if (body_size > 0) {
    const char* start = buffer_.data() + header_end + separator;
    request.body.assign(reinterpret_cast<const std::uint8_t*>(start),
                        reinterpret_cast<const std::uint8_t*>(start) + body_size);
  }
  buffer_.erase(0, total);

  const std::size_t question = request.target.find('?');
  if (question == std::string::npos) {
    request.path = request.target;
  } else {
    request.path = request.target.substr(0, question);
    request.query = request.target.substr(question + 1);
  }
  request.path = remove_dot_segments(request.path);
  return Result<HttpServerRequest>(std::move(request));
}

// ---------------------------------------------------------------------------
// Ranges and responses
// ---------------------------------------------------------------------------

Result<ByteRange> parse_range_header(std::string_view header,
                                     std::uint64_t size) {
  ByteRange range;
  const std::string value = trim(header);
  if (value.empty()) {
    range.length = size;
    return range;
  }
  const std::string lowered = to_lower(value);
  if (!starts_with(lowered, "bytes=")) {
    range.length = size;
    return range;
  }
  const std::string spec = trim(std::string_view(value).substr(6));
  if (spec.find(',') != std::string::npos) {
    // Multiple ranges are legal but `installd` never asks for them. Answer
    // with the full representation instead of a multipart body.
    range.length = size;
    return range;
  }
  if (size == 0) {
    return make_error(ErrorCode::kInvalidArgument, "empty representation");
  }
  const std::size_t dash = spec.find('-');
  if (dash == std::string::npos) {
    return make_error(ErrorCode::kInvalidArgument, "range has no dash");
  }
  const std::string first = trim(std::string_view(spec).substr(0, dash));
  const std::string last = trim(std::string_view(spec).substr(dash + 1));
  const auto parse_number = [](const std::string& text,
                               std::uint64_t* output) -> bool {
    if (text.empty()) return false;
    std::uint64_t value = 0;
    for (const char character : text) {
      if (std::isdigit(static_cast<unsigned char>(character)) == 0) return false;
      const std::uint64_t digit = static_cast<std::uint64_t>(character - '0');
      if (value > (UINT64_MAX - digit) / 10) return false;
      value = value * 10 + digit;
    }
    *output = value;
    return true;
  };
  std::uint64_t start = 0;
  std::uint64_t end = 0;
  if (first.empty()) {
    std::uint64_t suffix = 0;
    if (!parse_number(last, &suffix) || suffix == 0) {
      return make_error(ErrorCode::kInvalidArgument, "invalid suffix range");
    }
    start = suffix >= size ? 0 : size - suffix;
    end = size - 1;
  } else {
    if (!parse_number(first, &start)) {
      return make_error(ErrorCode::kInvalidArgument, "invalid range start");
    }
    if (last.empty()) {
      end = size - 1;
    } else {
      if (!parse_number(last, &end)) {
        return make_error(ErrorCode::kInvalidArgument, "invalid range end");
      }
      if (end < start) {
        return make_error(ErrorCode::kInvalidArgument, "range end precedes start");
      }
      end = std::min(end, size - 1);
    }
    if (start >= size) {
      return make_error(ErrorCode::kInvalidArgument, "range starts past the end");
    }
  }
  range.partial = true;
  range.start = start;
  range.length = end - start + 1;
  return range;
}

void HttpServerResponse::set(std::string name, std::string value) {
  for (auto& entry : headers) {
    if (iequals(entry.first, name)) {
      entry.second = std::move(value);
      return;
    }
  }
  headers.emplace_back(std::move(name), std::move(value));
}

std::string HttpServerResponse::header(std::string_view name) const {
  for (const auto& entry : headers) {
    if (iequals(entry.first, name)) return entry.second;
  }
  return {};
}

std::string HttpServerResponse::head(std::string_view date) const {
  std::string output = "HTTP/1.1 " + std::to_string(status) + " " +
                       reason_phrase(status) + "\r\n";
  for (const auto& entry : headers) {
    if (iequals(entry.first, "Content-Length") ||
        iequals(entry.first, "Connection")) {
      continue;
    }
    output += entry.first + ": " + entry.second + "\r\n";
  }
  if (!date.empty()) output += "Date: " + std::string(date) + "\r\n";
  const std::uint64_t length = has_file() ? file_length : body.size();
  output += "Content-Length: " + std::to_string(length) + "\r\n";
  output += "Connection: close\r\n\r\n";
  return output;
}

std::string reason_phrase(int status) {
  switch (status) {
    case 200:
      return "OK";
    case 204:
      return "No Content";
    case 206:
      return "Partial Content";
    case 304:
      return "Not Modified";
    case 400:
      return "Bad Request";
    case 403:
      return "Forbidden";
    case 404:
      return "Not Found";
    case 405:
      return "Method Not Allowed";
    case 416:
      return "Range Not Satisfiable";
    case 421:
      return "Misdirected Request";
    case 500:
      return "Internal Server Error";
    case 503:
      return "Service Unavailable";
    default:
      return "Unknown";
  }
}

std::string format_http_date(std::time_t value) {
  static const char* kDays[] = {"Sun", "Mon", "Tue", "Wed",
                                "Thu", "Fri", "Sat"};
  static const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  std::tm parts{};
#if defined(_WIN32)
  gmtime_s(&parts, &value);
#else
  gmtime_r(&value, &parts);
#endif
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%s, %02d %s %04d %02d:%02d:%02d GMT",
                kDays[parts.tm_wday % 7], parts.tm_mday,
                kMonths[parts.tm_mon % 12], parts.tm_year + 1900, parts.tm_hour,
                parts.tm_min, parts.tm_sec);
  return buffer;
}

// ---------------------------------------------------------------------------
// Router
// ---------------------------------------------------------------------------

OtaRouter::OtaRouter(std::string base_url, OtaPackage package,
                     OtaRoutes routes)
    : base_url_(std::move(base_url)),
      package_(std::move(package)),
      routes_(std::move(routes)) {}

std::string OtaRouter::manifest_url() const {
  return base_url_ + routes_.manifest;
}

std::string OtaRouter::install_url() const {
  return build_itms_services_url(manifest_url());
}

std::string OtaRouter::ping_url() const {
  return base_url_ + routes_.ping;
}

HttpServerResponse OtaRouter::error_response(int status,
                                             std::string message) const {
  HttpServerResponse response;
  response.status = status;
  response.body = std::move(message);
  if (!response.body.empty() && response.body.back() != '\n') {
    response.body.push_back('\n');
  }
  response.set("Content-Type", "text/plain; charset=utf-8");
  response.set("Cache-Control", "no-store");
  return response;
}

HttpServerResponse OtaRouter::file_response(const std::string& path,
                                            std::string_view content_type,
                                            const HttpServerRequest& request,
                                            std::uint64_t size) const {
  std::error_code error;
  if (path.empty() || !std::filesystem::exists(path, error)) {
    return error_response(404, "not found");
  }
  if (size == 0) size = file_size_of(path);
  HttpServerResponse response;
  response.file_path = path;
  response.set("Content-Type", std::string(content_type));
  response.set("Accept-Ranges", "bytes");
  response.set("Cache-Control", "no-store");

  auto range = parse_range_header(request.header("Range"), size);
  if (!range) {
    response.status = 416;
    response.file_path.clear();
    response.body.clear();
    response.set("Content-Range", "bytes */" + std::to_string(size));
    response.set("Content-Type", "text/plain; charset=utf-8");
    return response;
  }
  response.file_offset = range.value().start;
  response.file_length = range.value().length;
  if (range.value().partial) {
    response.status = 206;
    response.set("Content-Range",
                 "bytes " + std::to_string(response.file_offset) + "-" +
                     std::to_string(response.file_offset +
                                    response.file_length - 1) +
                     "/" + std::to_string(size));
  }
  return response;
}

HttpServerResponse OtaRouter::route(const HttpServerRequest& request) const {
  if (request.method == "OPTIONS") {
    HttpServerResponse response;
    response.status = 204;
    response.set("Allow", "GET, HEAD, OPTIONS");
    return response;
  }
  if (request.method != "GET" && request.method != "HEAD") {
    HttpServerResponse response = error_response(405, "method not allowed");
    response.set("Allow", "GET, HEAD, OPTIONS");
    return response;
  }
  const bool head_only = request.method == "HEAD";

  // The manifest embeds absolute URLs. Reaching it through a different host
  // would produce an installable package pointing somewhere else, so refuse
  // mismatched Host headers.
  const std::string host_header = to_lower(request.header("Host"));
  if (!host_header.empty() && !base_url_.empty()) {
    auto parsed = Url::parse(base_url_);
    if (parsed) {
      std::string expected = to_lower(parsed.value().host);
      const std::size_t colon = host_header.find(':');
      const std::string actual =
          colon == std::string::npos ? host_header : host_header.substr(0, colon);
      // Loopback is how the app itself probes the listener and how a user
      // can check the server from the device; the manifest keeps pointing at
      // the advertised address, so accepting it cannot redirect anything.
      const bool loopback = actual == "127.0.0.1" || actual == "localhost" ||
                            actual == "::1" || actual == "[::1]";
      if (actual != expected && !loopback) {
        return error_response(421, "unexpected host");
      }
    }
  }

  HttpServerResponse response;
  if (request.path == routes_.ping) {
    response.body = "pong\n";
    response.set("Content-Type", "text/plain; charset=utf-8");
    response.set("Cache-Control", "no-store");
  } else if (request.path == routes_.index ||
             request.path == "/index.html") {
    const std::string install_link =
        page_install_link_.empty() ? install_url() : page_install_link_;
    const std::string certificate =
        page_certificate_link_.empty()
            ? (package_.certificate_path.empty()
                   ? std::string()
                   : base_url_ + routes_.certificate)
            : page_certificate_link_;
    response.body = build_install_page(package_.title, package_.bundle_id,
                                       package_.display_version, install_link,
                                       certificate);
    response.set("Content-Type", "text/html; charset=utf-8");
    response.set("Cache-Control", "no-store");
  } else if (request.path == routes_.manifest) {
    OtaManifest manifest;
    manifest.bundle_id = package_.bundle_id;
    manifest.display_version = package_.display_version;
    manifest.title = package_.title;
    manifest.payload_url = base_url_ + routes_.payload;
    manifest.small_icon_url = package_.icon_small_path.empty()
                                  ? std::string()
                                  : base_url_ + routes_.icon_small;
    manifest.large_icon_url = package_.icon_large_path.empty()
                                  ? std::string()
                                  : base_url_ + routes_.icon_large;
    response.body = build_install_manifest_xml(manifest);
    response.set("Content-Type", "text/xml; charset=utf-8");
    response.set("Cache-Control", "no-store");
  } else if (request.path == routes_.payload) {
    response = file_response(package_.payload_path, "application/octet-stream",
                             request, package_.payload_size);
  } else if (request.path == routes_.icon_small && !routes_.icon_small.empty()) {
    const std::string path = package_.icon_small_path.empty()
                                 ? package_.icon_large_path
                                 : package_.icon_small_path;
    response = file_response(path, "image/png", request, 0);
  } else if (request.path == routes_.icon_large && !routes_.icon_large.empty()) {
    const std::string path = package_.icon_large_path.empty()
                                 ? package_.icon_small_path
                                 : package_.icon_large_path;
    response = file_response(path, "image/png", request, 0);
  } else if (request.path == routes_.certificate &&
             !package_.certificate_path.empty()) {
    response = file_response(package_.certificate_path,
                             "application/x-x509-ca-cert", request, 0);
  } else if (request.path == routes_.profile && !package_.profile_data.empty()) {
    response.body = package_.profile_data;
    response.set("Content-Type",
                 "application/x-apple-aspen-config; charset=utf-8");
    response.set("Content-Disposition",
                 "attachment; filename=\"IPAShelf Root CA.mobileconfig\"");
    response.set("Cache-Control", "no-store");
  } else {
    return error_response(404, "not found");
  }
  response.head_only = head_only;
  return response;
}

// ---------------------------------------------------------------------------
// Plain HTTP serving
// ---------------------------------------------------------------------------

#if defined(__unix__) || defined(__APPLE__) || defined(_POSIX_VERSION)

namespace {

bool send_all(int fd, const char* data, std::size_t size) {
  std::size_t sent = 0;
  while (sent < size) {
    const ssize_t result = ::send(fd, data + sent, size - sent, 0);
    if (result <= 0) return false;
    sent += static_cast<std::size_t>(result);
  }
  return true;
}

}  // namespace

Status serve_http_connection(int fd, const OtaRouter& router,
                             const CancellationToken& token) {
  // Peers include speculative browser preconnects that never send a request
  // and clients that stall mid-transfer. Without timeouts, one such socket
  // parks a serving slot forever and the loopback server looks unreachable —
  // "could not connect to localhost" — even though it is listening. The
  // timeouts are best-effort; failure to set them only loses that hardening.
  timeval receive_timeout{};
  receive_timeout.tv_sec = 10;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout,
               sizeof(receive_timeout));
  timeval send_timeout{};
  send_timeout.tv_sec = 30;
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout,
               sizeof(send_timeout));

  char buffer[16384];
  HttpRequestStream stream;
  std::optional<HttpServerRequest> request;
  while (!request.has_value()) {
    if (token.cancelled()) {
      return fail(ErrorCode::kCancelled, "connection cancelled");
    }
    const ssize_t received = ::recv(fd, buffer, sizeof(buffer), 0);
    if (received <= 0) return fail(ErrorCode::kNetwork, "connection closed");
    stream.append(buffer, static_cast<std::size_t>(received));
    auto parsed = stream.next();
    if (!parsed.has_value()) {
      if (stream.buffered() > 256 * 1024) {
        return fail(ErrorCode::kParse, "request is too large");
      }
      continue;
    }
    if (!parsed.value()) {
      const std::string response =
          "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n"
          "Connection: close\r\n\r\n";
      send_all(fd, response.data(), response.size());
      return Status{parsed.value().error()};
    }
    request = std::move(parsed.value()).value();
  }

  HttpServerResponse response = router.route(*request);
  const std::string head = response.head(format_http_date(std::time(nullptr)));
  if (!send_all(fd, head.data(), head.size())) {
    return fail(ErrorCode::kNetwork, "could not send response headers");
  }
  if (response.head_only) return ok_status();

  if (response.has_file()) {
    std::ifstream file(response.file_path, std::ios::binary);
    if (!file) return fail(ErrorCode::kStorage, "could not open payload");
    file.seekg(static_cast<std::streamoff>(response.file_offset));
    std::uint64_t remaining = response.file_length;
    std::vector<char> chunk(kChunkSize);
    while (remaining > 0) {
      if (token.cancelled()) {
        return fail(ErrorCode::kCancelled, "transfer cancelled");
      }
      const std::size_t wanted = static_cast<std::size_t>(
          std::min<std::uint64_t>(remaining, chunk.size()));
      file.read(chunk.data(), static_cast<std::streamsize>(wanted));
      const std::streamsize read = file.gcount();
      if (read <= 0) break;
      if (!send_all(fd, chunk.data(), static_cast<std::size_t>(read))) {
        return fail(ErrorCode::kNetwork, "could not send payload");
      }
      remaining -= static_cast<std::uint64_t>(read);
    }
    if (remaining != 0) {
      return fail(ErrorCode::kStorage, "payload ended early");
    }
    return ok_status();
  }

  if (!response.body.empty() &&
      !send_all(fd, response.body.data(), response.body.size())) {
    return fail(ErrorCode::kNetwork, "could not send body");
  }
  return ok_status();
}

#endif

}  // namespace ipashelf
