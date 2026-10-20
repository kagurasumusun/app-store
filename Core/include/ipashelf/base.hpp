// SPDX-License-Identifier: MIT
//
// IPAShelf core foundation types.
//
// This library is an independent implementation of the App Store package
// protocol used by IPAShelf. It is designed to compile as portable C++20 so
// the protocol layer can be exercised with a fake transport on any host; the
// iOS app supplies the platform transport, signer and storage.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ipashelf {

using Bytes = std::vector<std::uint8_t>;

enum class ErrorCode {
  kNone = 0,
  kInvalidArgument,
  kCancelled,
  kNetwork,
  kHttpStatus,
  kParse,
  kUnauthorized,
  kTokenExpired,
  kTwoFactorRequired,
  kLicenseRequired,
  kTemporarilyUnavailable,
  kNoOffer,
  kAppNotFound,
  kUnsupported,
  kSignerUnavailable,
  kStorage,
  kIntegrity,
  kInternal,
};

/// A transport-independent error. Messages never contain credentials because
/// callers are expected to pass a redacted string; the optional fields carry
/// Apple's public failure metadata for diagnostics.
struct Error {
  ErrorCode code = ErrorCode::kNone;
  std::string message;
  int http_status = 0;
  std::string apple_failure_type;
  std::string customer_message;

  Error() = default;
  Error(ErrorCode code_value, std::string text)
      : code(code_value), message(std::move(text)) {}
  Error(ErrorCode code_value, std::string text, int status)
      : code(code_value), message(std::move(text)), http_status(status) {}

  [[nodiscard]] bool ok() const { return code == ErrorCode::kNone; }
};

inline Error make_error(ErrorCode code, std::string message) {
  return Error{code, std::move(message)};
}

/// Result<T> is a deliberately small expected-like type. C++20 is the minimum
/// language version for the core so the code builds with the Xcode 27 clang and
/// with the portable test toolchain.
template <typename T>
class [[nodiscard]] Result {
 public:
  Result(T value) : value_(std::move(value)) {}  // NOLINT: implicit by design
  Result(Error error) : error_(std::move(error)) {}

  [[nodiscard]] bool has_value() const { return value_.has_value(); }
  explicit operator bool() const { return has_value(); }

  T& value() & { return *value_; }
  const T& value() const& { return *value_; }
  T&& value() && { return std::move(*value_); }

  Error& error() & { return *error_; }
  const Error& error() const& { return *error_; }

  T value_or(T fallback) const {
    return value_.has_value() ? *value_ : std::move(fallback);
  }

 private:
  std::optional<T> value_;
  std::optional<Error> error_;
};

/// Status is Result without a payload.
class [[nodiscard]] Status {
 public:
  Status() = default;  // success
  Status(Error error) : error_(std::move(error)) {}  // NOLINT

  [[nodiscard]] bool ok() const { return !error_.has_value(); }
  explicit operator bool() const { return ok(); }
  const Error& error() const { return *error_; }

 private:
  std::optional<Error> error_;
};

inline Status ok_status() { return Status(); }
inline Status fail(ErrorCode code, std::string message) {
  return Status{make_error(code, std::move(message))};
}

// ---------------------------------------------------------------------------
// Text and byte helpers
// ---------------------------------------------------------------------------

[[nodiscard]] std::string to_string(std::string_view value);
[[nodiscard]] std::string_view trim_view(std::string_view value);
[[nodiscard]] std::string trim(std::string_view value);
[[nodiscard]] std::string to_lower(std::string_view value);
[[nodiscard]] std::string to_upper(std::string_view value);
[[nodiscard]] bool iequals(std::string_view lhs, std::string_view rhs);
[[nodiscard]] bool starts_with(std::string_view value, std::string_view prefix);
[[nodiscard]] bool ends_with(std::string_view value, std::string_view suffix);
[[nodiscard]] std::vector<std::string> split(std::string_view value, char separator);
[[nodiscard]] std::string join(const std::vector<std::string>& values,
                               std::string_view separator);

[[nodiscard]] std::string hex_encode(const Bytes& data, bool upper = false);
[[nodiscard]] Result<Bytes> hex_decode(std::string_view text);

[[nodiscard]] Bytes bytes_from_string(std::string_view text);
[[nodiscard]] std::string string_from_bytes(const Bytes& bytes);
[[nodiscard]] std::string string_from_bytes(const std::uint8_t* data, std::size_t size);

/// Best-effort scrubbing for heap buffers that held credentials. The compiler
/// may still keep copies elsewhere; this only shortens the lifetime of the
/// obvious ones.
void secure_zero(void* data, std::size_t size);
void secure_zero(std::string& value);

[[nodiscard]] std::string redact_email(std::string_view email);

// ---------------------------------------------------------------------------
// Cancellation
// ---------------------------------------------------------------------------

class CancellationSource;

/// Cheap copyable handle shared with a worker operation.
class CancellationToken {
 public:
  CancellationToken() = default;
  [[nodiscard]] bool cancelled() const;
  void cancel();

 private:
  friend class CancellationSource;
  explicit CancellationToken(std::shared_ptr<std::atomic<bool>> flag)
      : flag_(std::move(flag)) {}

  std::shared_ptr<std::atomic<bool>> flag_;
};

class CancellationSource {
 public:
  CancellationSource();
  [[nodiscard]] CancellationToken token() const { return CancellationToken(flag_); }
  void cancel() { flag_->store(true, std::memory_order_release); }

 private:
  std::shared_ptr<std::atomic<bool>> flag_;
};

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

enum class LogLevel { kDebug, kInfo, kWarning, kError };

using LogSink = std::function<void(LogLevel, std::string_view)>;

/// Installs a process-wide sink. Passing nullptr disables logging. The sink
/// must be thread-safe; the iOS bridge forwards it to an in-memory ring buffer.
void set_log_sink(LogSink sink);
void log_message(LogLevel level, std::string_view message);

#define IPASHELF_LOG_DEBUG(message) \
  ::ipashelf::log_message(::ipashelf::LogLevel::kDebug, (message))
#define IPASHELF_LOG_INFO(message) \
  ::ipashelf::log_message(::ipashelf::LogLevel::kInfo, (message))
#define IPASHELF_LOG_WARNING(message) \
  ::ipashelf::log_message(::ipashelf::LogLevel::kWarning, (message))
#define IPASHELF_LOG_ERROR(message) \
  ::ipashelf::log_message(::ipashelf::LogLevel::kError, (message))

}  // namespace ipashelf
