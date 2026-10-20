// SPDX-License-Identifier: MIT

#include "ipashelf/base.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>

namespace ipashelf {
namespace {

std::mutex& log_mutex() {
  static std::mutex mutex;
  return mutex;
}

LogSink& log_sink() {
  static LogSink sink;
  return sink;
}

int hex_value(char character) {
  if (character >= '0' && character <= '9') return character - '0';
  if (character >= 'a' && character <= 'f') return character - 'a' + 10;
  if (character >= 'A' && character <= 'F') return character - 'A' + 10;
  return -1;
}

}  // namespace

std::string to_string(std::string_view value) { return std::string(value); }

std::string_view trim_view(std::string_view value) {
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.front())) != 0) {
    value.remove_prefix(1);
  }
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.back())) != 0) {
    value.remove_suffix(1);
  }
  return value;
}

std::string trim(std::string_view value) { return std::string(trim_view(value)); }

std::string to_lower(std::string_view value) {
  std::string result(value);
  std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return result;
}

std::string to_upper(std::string_view value) {
  std::string result(value);
  std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
    return static_cast<char>(std::toupper(c));
  });
  return result;
}

bool iequals(std::string_view lhs, std::string_view rhs) {
  if (lhs.size() != rhs.size()) return false;
  for (std::size_t index = 0; index < lhs.size(); ++index) {
    if (std::tolower(static_cast<unsigned char>(lhs[index])) !=
        std::tolower(static_cast<unsigned char>(rhs[index]))) {
      return false;
    }
  }
  return true;
}

bool starts_with(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool ends_with(std::string_view value, std::string_view suffix) {
  return value.size() >= suffix.size() &&
         value.substr(value.size() - suffix.size()) == suffix;
}

std::vector<std::string> split(std::string_view value, char separator) {
  std::vector<std::string> parts;
  std::size_t start = 0;
  while (start <= value.size()) {
    const std::size_t end = value.find(separator, start);
    if (end == std::string_view::npos) {
      parts.emplace_back(value.substr(start));
      break;
    }
    parts.emplace_back(value.substr(start, end - start));
    start = end + 1;
  }
  return parts;
}

std::string join(const std::vector<std::string>& values,
                 std::string_view separator) {
  std::string result;
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (index != 0) result.append(separator);
    result.append(values[index]);
  }
  return result;
}

std::string hex_encode(const Bytes& data, bool upper) {
  static constexpr char kLower[] = "0123456789abcdef";
  static constexpr char kUpper[] = "0123456789ABCDEF";
  const char* table = upper ? kUpper : kLower;
  std::string result;
  result.reserve(data.size() * 2);
  for (const std::uint8_t byte : data) {
    result.push_back(table[(byte >> 4) & 0x0f]);
    result.push_back(table[byte & 0x0f]);
  }
  return result;
}

Result<Bytes> hex_decode(std::string_view text) {
  if (text.size() % 2 != 0) {
    return make_error(ErrorCode::kParse, "hex string has an odd length");
  }
  Bytes result;
  result.reserve(text.size() / 2);
  for (std::size_t index = 0; index < text.size(); index += 2) {
    const int high = hex_value(text[index]);
    const int low = hex_value(text[index + 1]);
    if (high < 0 || low < 0) {
      return make_error(ErrorCode::kParse, "hex string contains a non-hex digit");
    }
    result.push_back(static_cast<std::uint8_t>((high << 4) | low));
  }
  return result;
}

Bytes bytes_from_string(std::string_view text) {
  return Bytes(text.begin(), text.end());
}

std::string string_from_bytes(const Bytes& bytes) {
  return std::string(bytes.begin(), bytes.end());
}

std::string string_from_bytes(const std::uint8_t* data, std::size_t size) {
  return std::string(reinterpret_cast<const char*>(data), size);
}

void secure_zero(void* data, std::size_t size) {
  if (data == nullptr || size == 0) return;
  volatile auto* bytes = static_cast<volatile unsigned char*>(data);
  for (std::size_t index = 0; index < size; ++index) bytes[index] = 0;
}

void secure_zero(std::string& value) {
  if (!value.empty()) secure_zero(value.data(), value.size());
  value.clear();
}

std::string redact_email(std::string_view email) {
  const std::size_t at = email.find('@');
  if (at == std::string_view::npos || at == 0) return "<redacted>";
  const std::size_t keep = std::min<std::size_t>(2, at);
  std::string result(email.substr(0, keep));
  result.append("***");
  result.append(email.substr(at));
  return result;
}

// ---------------------------------------------------------------------------
// Cancellation
// ---------------------------------------------------------------------------

CancellationSource::CancellationSource()
    : flag_(std::make_shared<std::atomic<bool>>(false)) {}

bool CancellationToken::cancelled() const {
  return flag_ && flag_->load(std::memory_order_acquire);
}

void CancellationToken::cancel() {
  if (flag_) flag_->store(true, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

void set_log_sink(LogSink sink) {
  std::lock_guard<std::mutex> lock(log_mutex());
  log_sink() = std::move(sink);
}

void log_message(LogLevel level, std::string_view message) {
  LogSink sink;
  {
    std::lock_guard<std::mutex> lock(log_mutex());
    sink = log_sink();
  }
  if (sink) sink(level, message);
}

}  // namespace ipashelf
