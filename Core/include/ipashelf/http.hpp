// SPDX-License-Identifier: MIT

#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ipashelf/base.hpp"

namespace ipashelf {

class HttpHeaders {
 public:
  void set(std::string name, std::string value);
  void add(std::string name, std::string value);
  [[nodiscard]] std::optional<std::string> get(std::string_view name) const;
  [[nodiscard]] std::string get_or(std::string_view name,
                                   std::string fallback = {}) const;
  void remove(std::string_view name);
  [[nodiscard]] bool contains(std::string_view name) const;
  [[nodiscard]] const std::vector<std::pair<std::string, std::string>>& items()
      const {
    return items_;
  }

 private:
  std::vector<std::pair<std::string, std::string>> items_;
};

struct HttpRequest {
  std::string method = "GET";
  std::string url;
  HttpHeaders headers;
  Bytes body;
  bool follow_redirects = true;
  /// Applied by platform transports where supported.
  std::optional<std::uint64_t> range_start;
  std::optional<std::uint64_t> range_end;
};

struct HttpResponse {
  int status = 0;
  HttpHeaders headers;
  Bytes body;
  std::string final_url;

  [[nodiscard]] std::string header(std::string_view name,
                                   std::string fallback = {}) const {
    return headers.get_or(name, std::move(fallback));
  }
};

using ChunkSink = std::function<bool(const std::uint8_t* data, std::size_t size)>;

/// A synchronous HTTP abstraction. The iOS implementation wraps NSURLSession
/// and reports bytes as they arrive so multi-gigabyte IPAs are never buffered.
class HttpTransport {
 public:
  virtual ~HttpTransport() = default;

  virtual Result<HttpResponse> send(const HttpRequest& request,
                                    const CancellationToken& token) = 0;

  /// Streams the body to `sink`. The returned response has an empty body.
  /// Returning false from the sink cancels the transfer.
  virtual Result<HttpResponse> send_streaming(const HttpRequest& request,
                                              const ChunkSink& sink,
                                              const CancellationToken& token) {
    auto response = send(request, token);
    if (!response) return response;
    if (!sink(response.value().body.data(), response.value().body.size())) {
      return make_error(ErrorCode::kCancelled, "transfer cancelled by sink");
    }
    response.value().body.clear();
    return response;
  }
};

/// Platform-independent helper used by the downloader. The caller owns the
/// file descriptor lifecycle.
class FileWriter {
 public:
  virtual ~FileWriter() = default;
  virtual bool write(const std::uint8_t* data, std::size_t size) = 0;
  virtual bool flush() = 0;
};

}  // namespace ipashelf
