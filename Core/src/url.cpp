// SPDX-License-Identifier: MIT

#include "ipashelf/url.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>

namespace ipashelf {
namespace {

bool is_unreserved(unsigned char character) {
  return (character >= 'A' && character <= 'Z') ||
         (character >= 'a' && character <= 'z') ||
         (character >= '0' && character <= '9') || character == '-' ||
         character == '.' || character == '_' || character == '~';
}

char hex_digit(unsigned value) {
  return static_cast<char>(value < 10 ? '0' + value : 'A' + (value - 10));
}

// Parses a decimal port. The digits are copied into a local string before
// conversion: a string_view into a caller's buffer is not guaranteed to be
// NUL-terminated, and `strtol` would otherwise read past the view.
bool parse_port(std::string_view digits, std::uint16_t* output) {
  if (digits.empty() || digits.size() > 5) return false;
  unsigned value = 0;
  for (const char character : digits) {
    if (character < '0' || character > '9') return false;
    value = value * 10 + static_cast<unsigned>(character - '0');
  }
  if (value == 0 || value > 65535) return false;
  *output = static_cast<std::uint16_t>(value);
  return true;
}

bool valid_host_char(char character) {
  const auto value = static_cast<unsigned char>(character);
  return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
         (value >= '0' && value <= '9') || character == '.' || character == '-' ||
         character == ':' || character == '[' || character == ']';
}

}  // namespace

std::string percent_encode(std::string_view value) {
  std::string output;
  output.reserve(value.size() * 3);
  for (const char character : value) {
    const auto byte = static_cast<unsigned char>(character);
    if (is_unreserved(byte)) {
      output.push_back(character);
    } else {
      output.push_back('%');
      output.push_back(hex_digit(byte >> 4));
      output.push_back(hex_digit(byte & 0x0f));
    }
  }
  return output;
}

namespace {

// Characters Foundation's URLComponents leaves raw inside a query item
// value (the `urlQueryAllowed` set), minus the ones that would corrupt
// the surrounding query if they ever appeared in a value (`&`, `=`,
// `+`, `#`, `%` itself). For an ordinary URL — letters, digits, `:`,
// `/`, `.`, `-` — the output is byte-identical to URLComponents'.
bool is_query_value_allowed(unsigned char character) {
  if (is_unreserved(character)) return true;
  switch (character) {
    case ':':
    case '/':
    case '?':
    case '@':
    case '!':
    case '$':
    case '\'':
    case '(':
    case ')':
    case '*':
    case ',':
    case ';':
      return true;
    default:
      return false;
  }
}

}  // namespace

std::string percent_encode_query_value(std::string_view value) {
  std::string output;
  output.reserve(value.size());
  for (const char character : value) {
    const auto byte = static_cast<unsigned char>(character);
    if (is_query_value_allowed(byte)) {
      output.push_back(character);
    } else {
      output.push_back('%');
      output.push_back(hex_digit(byte >> 4));
      output.push_back(hex_digit(byte & 0x0f));
    }
  }
  return output;
}

std::string form_encode(
    const std::vector<std::pair<std::string, std::string>>& values) {
  std::string output;
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (index != 0) output.push_back('&');
    for (const char character : values[index].first) {
      const auto byte = static_cast<unsigned char>(character);
      if (is_unreserved(byte)) {
        output.push_back(character);
      } else if (character == ' ') {
        output.push_back('+');
      } else {
        output.push_back('%');
        output.push_back(hex_digit(byte >> 4));
        output.push_back(hex_digit(byte & 0x0f));
      }
    }
    output.push_back('=');
    for (const char character : values[index].second) {
      const auto byte = static_cast<unsigned char>(character);
      if (is_unreserved(byte)) {
        output.push_back(character);
      } else if (character == ' ') {
        output.push_back('+');
      } else {
        output.push_back('%');
        output.push_back(hex_digit(byte >> 4));
        output.push_back(hex_digit(byte & 0x0f));
      }
    }
  }
  return output;
}

std::string build_query(
    const std::vector<std::pair<std::string, std::string>>& values) {
  return form_encode(values);
}

Result<Url> Url::parse(std::string_view text) {
  if (text.empty()) {
    return make_error(ErrorCode::kInvalidArgument, "URL is empty");
  }
  for (const char character : text) {
    const auto byte = static_cast<unsigned char>(character);
    if (byte <= 0x20 || byte == 0x7f) {
      return make_error(ErrorCode::kInvalidArgument,
                        "URL contains whitespace or a control character");
    }
  }

  Url url;
  std::size_t position = 0;
  const std::size_t colon = text.find(':');
  if (colon == std::string_view::npos) {
    return make_error(ErrorCode::kInvalidArgument, "URL has no scheme");
  }
  url.scheme = std::string(text.substr(0, colon));
  if (url.scheme.empty()) {
    return make_error(ErrorCode::kInvalidArgument, "URL scheme is empty");
  }
  for (const char character : url.scheme) {
    if (!(std::isalnum(static_cast<unsigned char>(character)) ||
          character == '+' || character == '-' || character == '.')) {
      return make_error(ErrorCode::kInvalidArgument, "URL scheme is invalid");
    }
  }
  position = colon + 1;
  if (text.substr(position, 2) != "//") {
    return make_error(ErrorCode::kInvalidArgument,
                      "URL is not absolute or has no authority");
  }
  position += 2;

  const std::size_t authority_end = text.find_first_of("/?#", position);
  const std::size_t authority_size =
      authority_end == std::string_view::npos ? text.size() - position
                                              : authority_end - position;
  std::string_view authority = text.substr(position, authority_size);
  const std::size_t at = authority.rfind('@');
  if (at != std::string_view::npos) {
    url.userinfo = std::string(authority.substr(0, at));
    authority.remove_prefix(at + 1);
  }

  std::string_view host_part = authority;
  if (!authority.empty() && authority.front() == '[') {
    const std::size_t closing = authority.find(']');
    if (closing == std::string_view::npos) {
      return make_error(ErrorCode::kInvalidArgument, "URL IPv6 host is invalid");
    }
    host_part = authority.substr(0, closing + 1);
    const std::string_view remainder = authority.substr(closing + 1);
    if (!remainder.empty()) {
      if (remainder.front() != ':') {
        return make_error(ErrorCode::kInvalidArgument, "URL authority is invalid");
      }
      const std::string_view port = remainder.substr(1);
      if (!port.empty()) {
        std::uint16_t parsed = 0;
        if (!parse_port(port, &parsed)) {
          return make_error(ErrorCode::kInvalidArgument, "URL port is invalid");
        }
        url.port = parsed;
      }
    }
  } else {
    const std::size_t port_separator = authority.rfind(':');
    if (port_separator != std::string_view::npos &&
        authority.find(':') == port_separator) {
      host_part = authority.substr(0, port_separator);
      const std::string_view port = authority.substr(port_separator + 1);
      if (!port.empty()) {
        std::uint16_t parsed = 0;
        if (!parse_port(port, &parsed)) {
          return make_error(ErrorCode::kInvalidArgument, "URL port is invalid");
        }
        url.port = parsed;
      }
    } else {
      host_part = authority;
    }
  }

  if (host_part.empty()) {
    return make_error(ErrorCode::kInvalidArgument, "URL host is empty");
  }
  url.host = std::string(host_part);
  for (const char character : url.host) {
    if (!valid_host_char(character)) {
      return make_error(ErrorCode::kInvalidArgument, "URL host is invalid");
    }
  }

  position += authority_size;
  const std::size_t path_end = text.find_first_of("?#", position);
  url.path = std::string(text.substr(
      position, path_end == std::string_view::npos ? text.size() - position
                                                  : path_end - position));
  if (url.path.empty() && (url.scheme == "https" || url.scheme == "http")) {
    url.path = "/";
  }
  position = path_end == std::string_view::npos ? text.size() : path_end;

  if (position < text.size() && text[position] == '?') {
    ++position;
    const std::size_t query_end = text.find('#', position);
    url.query = std::string(text.substr(
        position, query_end == std::string_view::npos ? text.size() - position
                                                      : query_end - position));
    position = query_end == std::string_view::npos ? text.size() : query_end;
  }
  if (position < text.size() && text[position] == '#') {
    url.fragment = std::string(text.substr(position + 1));
  }
  return url;
}

std::string Url::serialize() const {
  std::string output;
  output.append(scheme).append("://");
  if (!userinfo.empty()) output.append(userinfo).append("@");
  output.append(host);
  if (port.has_value()) output.append(":").append(std::to_string(*port));
  output.append(path.empty() ? "/" : path);
  if (!query.empty()) output.append("?").append(query);
  if (!fragment.empty()) output.append("#").append(fragment);
  return output;
}

std::string Url::host_with_port() const {
  std::string result = host;
  if (port.has_value()) result.append(":").append(std::to_string(*port));
  return result;
}

bool host_is_or_subdomain(std::string_view host, std::string_view domain) {
  const std::string lower_host = to_lower(host);
  const std::string lower_domain = to_lower(domain);
  return lower_host == lower_domain ||
         ends_with(lower_host, "." + lower_domain);
}

bool valid_https_url(std::string_view value) {
  auto parsed = Url::parse(value);
  return parsed && parsed.value().https() && parsed.value().userinfo.empty() &&
         !parsed.value().host.empty();
}

std::string remove_dot_segments(std::string_view path) {
  std::string output;
  std::string input(path);
  while (!input.empty()) {
    if (starts_with(input, "../")) {
      input.erase(0, 3);
    } else if (starts_with(input, "./")) {
      input.erase(0, 2);
    } else if (starts_with(input, "/./")) {
      input.replace(0, 3, "/");
    } else if (input == "/.") {
      input = "/";
    } else if (starts_with(input, "/../")) {
      input.replace(0, 4, "/");
      const std::size_t slash = output.rfind('/');
      output.erase(slash == std::string::npos ? 0 : slash);
    } else if (input == "/..") {
      input = "/";
      const std::size_t slash = output.rfind('/');
      output.erase(slash == std::string::npos ? 0 : slash);
    } else if (input == "." || input == "..") {
      input.clear();
    } else {
      std::size_t next = input.find('/', input.front() == '/' ? 1 : 0);
      if (next == std::string::npos) {
        output.append(input);
        input.clear();
      } else {
        output.append(input.substr(0, next));
        input.erase(0, next);
      }
    }
  }
  return output;
}

Result<std::string> resolve_url(std::string_view base_text,
                                std::string_view reference_text) {
  auto base = Url::parse(base_text);
  if (!base) return base.error();
  const std::string reference = trim(reference_text);
  if (reference.empty()) {
    return make_error(ErrorCode::kInvalidArgument, "redirect location is empty");
  }
  if (reference.find("://") != std::string::npos ||
      (reference.size() > 1 && reference[0] != '/' &&
       reference.find(':') != std::string::npos &&
       reference.find('/') > reference.find(':'))) {
    auto absolute = Url::parse(reference);
    if (!absolute) return absolute.error();
    return absolute.value().serialize();
  }

  Url result = base.value();
  result.fragment.clear();
  if (starts_with(reference, "//")) {
    result = Url::parse(result.scheme + ":" + reference).value();
    return result.serialize();
  }
  if (reference.front() == '#') {
    result.fragment = reference.substr(1);
    return result.serialize();
  }
  if (reference.front() == '?') {
    result.query = reference.substr(1);
    return result.serialize();
  }

  std::string reference_path;
  std::string reference_query;
  const std::size_t hash = reference.find('#');
  const std::size_t question = reference.find('?');
  const std::size_t path_end = std::min(
      hash == std::string::npos ? reference.size() : hash,
      question == std::string::npos ? reference.size() : question);
  reference_path = reference.substr(0, path_end);
  if (question != std::string::npos && (hash == std::string::npos || question < hash)) {
    reference_query = reference.substr(
        question + 1,
        hash == std::string::npos ? std::string::npos : hash - question - 1);
  }

  if (reference_path.empty()) {
    if (!reference_query.empty()) result.query = reference_query;
  } else {
    if (reference_path.front() == '/') {
      result.path = remove_dot_segments(reference_path);
    } else {
      std::string merged;
      const std::size_t slash = result.path.rfind('/');
      if (slash == std::string::npos) {
        merged = "/" + reference_path;
      } else {
        merged = result.path.substr(0, slash + 1) + reference_path;
      }
      result.path = remove_dot_segments(merged);
    }
    result.query = reference_query;
  }
  return result.serialize();
}

}  // namespace ipashelf
