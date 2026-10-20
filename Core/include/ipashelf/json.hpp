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

/// Minimal JSON DOM used for the public iTunes lookup API and the App Store
/// catalog API. It keeps object insertion order and the original spelling of
/// numbers so 64-bit identifiers survive a round trip.
class Json {
 public:
  enum class Type { kNull, kBool, kNumber, kString, kArray, kObject };

  Json() = default;
  static Json null();
  static Json boolean(bool value);
  static Json number(double value);
  static Json integer(std::int64_t value);
  static Json string(std::string value);
  static Json array(std::vector<Json> values = {});
  static Json object(
      std::vector<std::pair<std::string, Json>> values = {});

  [[nodiscard]] Type type() const { return type_; }
  [[nodiscard]] bool is_null() const { return type_ == Type::kNull; }
  [[nodiscard]] bool is_bool() const { return type_ == Type::kBool; }
  [[nodiscard]] bool is_number() const { return type_ == Type::kNumber; }
  [[nodiscard]] bool is_string() const { return type_ == Type::kString; }
  [[nodiscard]] bool is_array() const { return type_ == Type::kArray; }
  [[nodiscard]] bool is_object() const { return type_ == Type::kObject; }

  [[nodiscard]] bool bool_value() const { return bool_; }
  [[nodiscard]] double number_value() const { return number_; }
  [[nodiscard]] std::int64_t int_value() const;
  [[nodiscard]] const std::string& string_value() const { return string_; }
  [[nodiscard]] const std::string& number_raw() const { return number_raw_; }
  [[nodiscard]] const std::vector<Json>& array_items() const { return array_; }
  [[nodiscard]] const std::vector<std::pair<std::string, Json>>& object_items()
      const {
    return object_;
  }

  [[nodiscard]] const Json* find(std::string_view key) const;
  [[nodiscard]] const Json* at(std::size_t index) const;
  [[nodiscard]] std::size_t size() const;

  [[nodiscard]] std::string string_or(std::string fallback = {}) const;
  [[nodiscard]] std::int64_t int_or(std::int64_t fallback = 0) const;
  [[nodiscard]] double number_or(double fallback = 0) const;
  [[nodiscard]] bool bool_or(bool fallback = false) const;

  void set(std::string key, Json value);

  /// Replaces the raw spelling used when serialising a number. The JSON
  /// parser uses this to preserve 64-bit identifiers exactly.
  void set_number_raw(std::string raw) { number_raw_ = std::move(raw); }

  [[nodiscard]] std::string dump(bool pretty = false) const;

  static Result<Json> parse(std::string_view text);

 private:
  Type type_ = Type::kNull;
  bool bool_ = false;
  double number_ = 0;
  std::string number_raw_;
  std::string string_;
  std::vector<Json> array_;
  std::vector<std::pair<std::string, Json>> object_;
};

}  // namespace ipashelf
