// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ipashelf/base.hpp"

namespace ipashelf {

struct Url {
  std::string scheme;
  std::string userinfo;
  std::string host;
  std::optional<std::uint16_t> port;
  std::string path;
  std::string query;
  std::string fragment;

  [[nodiscard]] static Result<Url> parse(std::string_view text);
  [[nodiscard]] std::string serialize() const;
  [[nodiscard]] std::string host_with_port() const;
  [[nodiscard]] bool https() const { return iequals(scheme, "https"); }
};

[[nodiscard]] std::string percent_encode(std::string_view value);
[[nodiscard]] std::string form_encode(
    const std::vector<std::pair<std::string, std::string>>& values);
[[nodiscard]] std::string build_query(
    const std::vector<std::pair<std::string, std::string>>& values);
[[nodiscard]] Result<std::string> resolve_url(std::string_view base,
                                              std::string_view reference);
[[nodiscard]] bool host_is_or_subdomain(std::string_view host,
                                        std::string_view domain);
[[nodiscard]] bool valid_https_url(std::string_view value);

/// Removes dot segments from an absolute path as described by RFC 3986.
[[nodiscard]] std::string remove_dot_segments(std::string_view path);

}  // namespace ipashelf
