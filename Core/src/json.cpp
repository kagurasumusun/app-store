// SPDX-License-Identifier: MIT

#include "ipashelf/json.hpp"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace ipashelf {
namespace {

constexpr std::size_t kMaxDepth = 256;

void append_utf8(std::string& output, std::uint32_t code_point) {
  if (code_point <= 0x7f) {
    output.push_back(static_cast<char>(code_point));
  } else if (code_point <= 0x7ff) {
    output.push_back(static_cast<char>(0xc0 | (code_point >> 6)));
    output.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
  } else if (code_point <= 0xffff) {
    output.push_back(static_cast<char>(0xe0 | (code_point >> 12)));
    output.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
    output.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
  } else {
    output.push_back(static_cast<char>(0xf0 | (code_point >> 18)));
    output.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3f)));
    output.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
    output.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
  }
}

void append_escaped(std::string& output, std::string_view value) {
  output.push_back('"');
  for (const char character : value) {
    const auto byte = static_cast<unsigned char>(character);
    switch (character) {
      case '"':
        output.append("\\\"");
        break;
      case '\\':
        output.append("\\\\");
        break;
      case '\b':
        output.append("\\b");
        break;
      case '\f':
        output.append("\\f");
        break;
      case '\n':
        output.append("\\n");
        break;
      case '\r':
        output.append("\\r");
        break;
      case '\t':
        output.append("\\t");
        break;
      default:
        if (byte < 0x20) {
          char buffer[7];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x", byte);
          output.append(buffer);
        } else {
          output.push_back(character);
        }
        break;
    }
  }
  output.push_back('"');
}

void dump_value(const Json& value, std::string& output, bool pretty, int depth) {
  const auto newline = [&](int level) {
    if (!pretty) return;
    output.push_back('\n');
    output.append(static_cast<std::size_t>(level) * 2, ' ');
  };

  switch (value.type()) {
    case Json::Type::kNull:
      output.append("null");
      break;
    case Json::Type::kBool:
      output.append(value.bool_value() ? "true" : "false");
      break;
    case Json::Type::kNumber: {
      if (!value.number_raw().empty()) {
        output.append(value.number_raw());
      } else {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%.17g", value.number_value());
        output.append(buffer);
      }
      break;
    }
    case Json::Type::kString:
      append_escaped(output, value.string_value());
      break;
    case Json::Type::kArray: {
      output.push_back('[');
      const auto& items = value.array_items();
      for (std::size_t index = 0; index < items.size(); ++index) {
        if (index != 0) output.push_back(',');
        newline(depth + 1);
        dump_value(items[index], output, pretty, depth + 1);
      }
      if (!items.empty()) newline(depth);
      output.push_back(']');
      break;
    }
    case Json::Type::kObject: {
      output.push_back('{');
      const auto& items = value.object_items();
      for (std::size_t index = 0; index < items.size(); ++index) {
        if (index != 0) output.push_back(',');
        newline(depth + 1);
        append_escaped(output, items[index].first);
        output.push_back(':');
        if (pretty) output.push_back(' ');
        dump_value(items[index].second, output, pretty, depth + 1);
      }
      if (!items.empty()) newline(depth);
      output.push_back('}');
      break;
    }
  }
}

class Parser {
 public:
  explicit Parser(std::string_view text) : text_(text) {}

  Result<Json> parse() {
    skip_whitespace();
    auto value = parse_value(0);
    if (!value) return value;
    skip_whitespace();
    if (position_ != text_.size()) {
      return make_error(ErrorCode::kParse, "JSON has trailing data");
    }
    return value;
  }

 private:
  void skip_whitespace() {
    while (position_ < text_.size()) {
      const char character = text_[position_];
      if (character == ' ' || character == '\n' || character == '\r' ||
          character == '\t') {
        ++position_;
      } else {
        break;
      }
    }
  }

  Result<Json> parse_value(std::size_t depth) {
    if (depth > kMaxDepth) {
      return make_error(ErrorCode::kParse, "JSON nesting is too deep");
    }
    skip_whitespace();
    if (position_ >= text_.size()) {
      return make_error(ErrorCode::kParse, "JSON ended unexpectedly");
    }
    const char character = text_[position_];
    switch (character) {
      case '{':
        return parse_object(depth);
      case '[':
        return parse_array(depth);
      case '"':
        return parse_string_value();
      case 't':
        return parse_literal("true", Json::boolean(true));
      case 'f':
        return parse_literal("false", Json::boolean(false));
      case 'n':
        return parse_literal("null", Json::null());
      default:
        if (character == '-' || (character >= '0' && character <= '9')) {
          return parse_number();
        }
        return make_error(ErrorCode::kParse, "JSON contains an invalid value");
    }
  }

  Result<Json> parse_literal(std::string_view literal, Json value) {
    if (text_.substr(position_, literal.size()) != literal) {
      return make_error(ErrorCode::kParse, "JSON literal is malformed");
    }
    position_ += literal.size();
    return value;
  }

  Result<Json> parse_object(std::size_t depth) {
    ++position_;  // '{'
    Json object = Json::object();
    skip_whitespace();
    if (position_ < text_.size() && text_[position_] == '}') {
      ++position_;
      return object;
    }
    while (true) {
      skip_whitespace();
      if (position_ >= text_.size() || text_[position_] != '"') {
        return make_error(ErrorCode::kParse, "JSON object key is missing");
      }
      auto key = parse_string();
      if (!key) return key.error();
      skip_whitespace();
      if (position_ >= text_.size() || text_[position_] != ':') {
        return make_error(ErrorCode::kParse, "JSON object key has no value");
      }
      ++position_;
      auto value = parse_value(depth + 1);
      if (!value) return value;
      object.set(std::move(key).value(), std::move(value).value());
      skip_whitespace();
      if (position_ >= text_.size()) {
        return make_error(ErrorCode::kParse, "JSON object is not closed");
      }
      if (text_[position_] == ',') {
        ++position_;
        continue;
      }
      if (text_[position_] == '}') {
        ++position_;
        return object;
      }
      return make_error(ErrorCode::kParse, "JSON object has an invalid separator");
    }
  }

  Result<Json> parse_array(std::size_t depth) {
    ++position_;  // '['
    std::vector<Json> items;
    skip_whitespace();
    if (position_ < text_.size() && text_[position_] == ']') {
      ++position_;
      return Json::array(std::move(items));
    }
    while (true) {
      auto value = parse_value(depth + 1);
      if (!value) return value;
      items.push_back(std::move(value).value());
      skip_whitespace();
      if (position_ >= text_.size()) {
        return make_error(ErrorCode::kParse, "JSON array is not closed");
      }
      if (text_[position_] == ',') {
        ++position_;
        continue;
      }
      if (text_[position_] == ']') {
        ++position_;
        return Json::array(std::move(items));
      }
      return make_error(ErrorCode::kParse, "JSON array has an invalid separator");
    }
  }

  Result<std::string> parse_string() {
    ++position_;  // opening quote
    std::string output;
    while (position_ < text_.size()) {
      const char character = text_[position_++];
      if (character == '"') return output;
      if (character != '\\') {
        if (static_cast<unsigned char>(character) < 0x20) {
          return make_error(ErrorCode::kParse, "JSON string has a control character");
        }
        output.push_back(character);
        continue;
      }
      if (position_ >= text_.size()) {
        return make_error(ErrorCode::kParse, "JSON escape is truncated");
      }
      const char escape = text_[position_++];
      switch (escape) {
        case '"':
          output.push_back('"');
          break;
        case '\\':
          output.push_back('\\');
          break;
        case '/':
          output.push_back('/');
          break;
        case 'b':
          output.push_back('\b');
          break;
        case 'f':
          output.push_back('\f');
          break;
        case 'n':
          output.push_back('\n');
          break;
        case 'r':
          output.push_back('\r');
          break;
        case 't':
          output.push_back('\t');
          break;
        case 'u': {
          std::uint32_t code_point = 0;
          if (!read_hex4(code_point)) {
            return make_error(ErrorCode::kParse, "JSON unicode escape is malformed");
          }
          if (code_point >= 0xd800 && code_point <= 0xdbff) {
            if (position_ + 1 >= text_.size() || text_[position_] != '\\' ||
                text_[position_ + 1] != 'u') {
              return make_error(ErrorCode::kParse,
                                "JSON surrogate pair is incomplete");
            }
            position_ += 2;
            std::uint32_t low = 0;
            if (!read_hex4(low) || low < 0xdc00 || low > 0xdfff) {
              return make_error(ErrorCode::kParse,
                                "JSON surrogate pair is invalid");
            }
            code_point = 0x10000 + ((code_point - 0xd800) << 10) + (low - 0xdc00);
          } else if (code_point >= 0xdc00 && code_point <= 0xdfff) {
            return make_error(ErrorCode::kParse, "JSON surrogate is unpaired");
          }
          append_utf8(output, code_point);
          break;
        }
        default:
          return make_error(ErrorCode::kParse, "JSON escape is unknown");
      }
    }
    return make_error(ErrorCode::kParse, "JSON string is not closed");
  }

  bool read_hex4(std::uint32_t& value) {
    if (position_ + 4 > text_.size()) return false;
    value = 0;
    for (int index = 0; index < 4; ++index) {
      const char character = text_[position_++];
      int digit = -1;
      if (character >= '0' && character <= '9') {
        digit = character - '0';
      } else if (character >= 'a' && character <= 'f') {
        digit = character - 'a' + 10;
      } else if (character >= 'A' && character <= 'F') {
        digit = character - 'A' + 10;
      }
      if (digit < 0) return false;
      value = (value << 4) | static_cast<std::uint32_t>(digit);
    }
    return true;
  }

  Result<Json> parse_string_value() {
    auto value = parse_string();
    if (!value) return value.error();
    return Json::string(std::move(value).value());
  }

  Result<Json> parse_number() {
    const std::size_t start = position_;
    if (text_[position_] == '-') ++position_;
    if (position_ >= text_.size()) {
      return make_error(ErrorCode::kParse, "JSON number is truncated");
    }
    if (text_[position_] == '0') {
      ++position_;
    } else if (text_[position_] >= '1' && text_[position_] <= '9') {
      while (position_ < text_.size() && text_[position_] >= '0' &&
             text_[position_] <= '9') {
        ++position_;
      }
    } else {
      return make_error(ErrorCode::kParse, "JSON number is malformed");
    }
    if (position_ < text_.size() && text_[position_] == '.') {
      ++position_;
      const std::size_t digits = position_;
      while (position_ < text_.size() && text_[position_] >= '0' &&
             text_[position_] <= '9') {
        ++position_;
      }
      if (digits == position_) {
        return make_error(ErrorCode::kParse, "JSON fraction is empty");
      }
    }
    if (position_ < text_.size() &&
        (text_[position_] == 'e' || text_[position_] == 'E')) {
      ++position_;
      if (position_ < text_.size() &&
          (text_[position_] == '+' || text_[position_] == '-')) {
        ++position_;
      }
      const std::size_t digits = position_;
      while (position_ < text_.size() && text_[position_] >= '0' &&
             text_[position_] <= '9') {
        ++position_;
      }
      if (digits == position_) {
        return make_error(ErrorCode::kParse, "JSON exponent is empty");
      }
    }

    const std::string raw(text_.substr(start, position_ - start));
    char* end = nullptr;
    const double value = std::strtod(raw.c_str(), &end);
    if (end == nullptr || *end != '\0') {
      return make_error(ErrorCode::kParse, "JSON number cannot be parsed");
    }
    Json result = Json::number(value);
    result.set_number_raw(raw);
    return result;
  }

  std::string_view text_;
  std::size_t position_ = 0;
};

}  // namespace

Json Json::null() {
  Json value;
  value.type_ = Type::kNull;
  return value;
}

Json Json::boolean(bool input) {
  Json value;
  value.type_ = Type::kBool;
  value.bool_ = input;
  return value;
}

Json Json::number(double input) {
  Json value;
  value.type_ = Type::kNumber;
  value.number_ = input;
  return value;
}

Json Json::integer(std::int64_t input) {
  Json value = number(static_cast<double>(input));
  return value;
}

Json Json::string(std::string input) {
  Json value;
  value.type_ = Type::kString;
  value.string_ = std::move(input);
  return value;
}

Json Json::array(std::vector<Json> values) {
  Json value;
  value.type_ = Type::kArray;
  value.array_ = std::move(values);
  return value;
}

Json Json::object(std::vector<std::pair<std::string, Json>> values) {
  Json value;
  value.type_ = Type::kObject;
  value.object_ = std::move(values);
  return value;
}

std::int64_t Json::int_value() const {
  if (type_ == Type::kNumber && !number_raw_.empty()) {
    std::int64_t parsed = 0;
    const char* begin = number_raw_.data();
    const char* end = begin + number_raw_.size();
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec == std::errc() && result.ptr == end) return parsed;
  }
  if (type_ == Type::kNumber) {
    return static_cast<std::int64_t>(number_);
  }
  return 0;
}

const Json* Json::find(std::string_view key) const {
  if (type_ != Type::kObject) return nullptr;
  for (const auto& item : object_) {
    if (item.first == key) return &item.second;
  }
  return nullptr;
}

const Json* Json::at(std::size_t index) const {
  if (type_ != Type::kArray || index >= array_.size()) return nullptr;
  return &array_[index];
}

std::size_t Json::size() const {
  if (type_ == Type::kArray) return array_.size();
  if (type_ == Type::kObject) return object_.size();
  if (type_ == Type::kString) return string_.size();
  return 0;
}

std::string Json::string_or(std::string fallback) const {
  return type_ == Type::kString ? string_ : std::move(fallback);
}

std::int64_t Json::int_or(std::int64_t fallback) const {
  return type_ == Type::kNumber ? int_value() : fallback;
}

double Json::number_or(double fallback) const {
  return type_ == Type::kNumber ? number_ : fallback;
}

bool Json::bool_or(bool fallback) const {
  return type_ == Type::kBool ? bool_ : fallback;
}

void Json::set(std::string key, Json value) {
  if (type_ != Type::kObject) {
    type_ = Type::kObject;
    object_.clear();
  }
  for (auto& item : object_) {
    if (item.first == key) {
      item.second = std::move(value);
      return;
    }
  }
  object_.emplace_back(std::move(key), std::move(value));
}

Result<Json> Json::parse(std::string_view text) {
  return Parser(text).parse();
}

std::string Json::dump(bool pretty) const {
  std::string output;
  dump_value(*this, output, pretty, 0);
  return output;
}

}  // namespace ipashelf
