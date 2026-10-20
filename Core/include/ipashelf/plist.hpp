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

/// A property-list value. Dictionaries keep insertion order so request bodies
/// and generated metadata are stable. Both Apple XML plists and the binary
/// plist format used by bundle Info.plist files are supported.
class PlistValue {
 public:
  enum class Type {
    kNull,
    kBool,
    kInteger,
    kReal,
    kString,
    kData,
    kDate,
    kArray,
    kDictionary,
  };

  PlistValue() = default;
  static PlistValue null();
  static PlistValue boolean(bool value);
  static PlistValue integer(std::int64_t value);
  static PlistValue real(double value);
  static PlistValue string(std::string value);
  static PlistValue data(Bytes value);
  static PlistValue date(std::string value);
  static PlistValue array(std::vector<PlistValue> values = {});
  static PlistValue dictionary(
      std::vector<std::pair<std::string, PlistValue>> values = {});

  [[nodiscard]] Type type() const { return type_; }
  [[nodiscard]] bool is_bool() const { return type_ == Type::kBool; }
  [[nodiscard]] bool is_integer() const { return type_ == Type::kInteger; }
  [[nodiscard]] bool is_real() const { return type_ == Type::kReal; }
  [[nodiscard]] bool is_string() const { return type_ == Type::kString; }
  [[nodiscard]] bool is_data() const { return type_ == Type::kData; }
  [[nodiscard]] bool is_array() const { return type_ == Type::kArray; }
  [[nodiscard]] bool is_dictionary() const {
    return type_ == Type::kDictionary;
  }

  [[nodiscard]] bool bool_value() const { return bool_; }
  [[nodiscard]] std::int64_t integer_value() const { return integer_; }
  [[nodiscard]] double real_value() const { return real_; }
  [[nodiscard]] const std::string& string_value() const { return string_; }
  [[nodiscard]] const Bytes& data_value() const { return data_; }
  [[nodiscard]] const std::vector<PlistValue>& array_items() const {
    return array_;
  }
  [[nodiscard]] const std::vector<std::pair<std::string, PlistValue>>&
  dictionary_items() const {
    return dictionary_;
  }

  [[nodiscard]] const PlistValue* find(std::string_view key) const;
  [[nodiscard]] const PlistValue* at(std::size_t index) const;
  [[nodiscard]] std::size_t size() const;

  [[nodiscard]] std::optional<std::string> string_for(
      std::string_view key) const;
  [[nodiscard]] std::string string_or(std::string_view key,
                                      std::string fallback = {}) const;
  [[nodiscard]] std::optional<std::int64_t> integer_for(
      std::string_view key) const;
  [[nodiscard]] bool bool_or(std::string_view key, bool fallback = false) const;

  void set(std::string key, PlistValue value);
  void append(PlistValue value) { array_.push_back(std::move(value)); }

  [[nodiscard]] std::string dump_xml() const;
  [[nodiscard]] Bytes dump_binary() const;

  static Result<PlistValue> parse_xml(std::string_view text);
  static Result<PlistValue> parse_binary(const Bytes& data);
  static Result<PlistValue> parse(const Bytes& data);

 private:
  Type type_ = Type::kNull;
  bool bool_ = false;
  std::int64_t integer_ = 0;
  double real_ = 0;
  std::string string_;
  Bytes data_;
  std::vector<PlistValue> array_;
  std::vector<std::pair<std::string, PlistValue>> dictionary_;
};

}  // namespace ipashelf
