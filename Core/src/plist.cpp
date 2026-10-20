// SPDX-License-Identifier: MIT

#include "ipashelf/plist.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <limits>
#include <sstream>

#include "ipashelf/crypto.hpp"

namespace ipashelf {
namespace {

constexpr std::size_t kMaxDepth = 256;
constexpr double kAppleEpochOffset = 978307200.0;

std::string xml_escape(std::string_view text) {
  std::string result;
  result.reserve(text.size());
  for (const char character : text) {
    switch (character) {
      case '&':
        result.append("&amp;");
        break;
      case '<':
        result.append("&lt;");
        break;
      case '>':
        result.append("&gt;");
        break;
      case '"':
        result.append("&quot;");
        break;
      case '\'':
        result.append("&apos;");
        break;
      default:
        result.push_back(character);
        break;
    }
  }
  return result;
}

void append_indent(std::string& output, int depth) {
  output.append(static_cast<std::size_t>(depth) * 2, ' ');
}

void dump_xml_value(const PlistValue& value, std::string& output, int depth) {
  using Type = PlistValue::Type;
  switch (value.type()) {
    case Type::kNull:
      output.append("<string></string>");
      break;
    case Type::kBool:
      output.append(value.bool_value() ? "<true/>" : "<false/>");
      break;
    case Type::kInteger:
      output.append("<integer>");
      output.append(std::to_string(value.integer_value()));
      output.append("</integer>");
      break;
    case Type::kReal: {
      char buffer[64];
      std::snprintf(buffer, sizeof(buffer), "%.17g", value.real_value());
      output.append("<real>");
      output.append(buffer);
      output.append("</real>");
      break;
    }
    case Type::kString:
      output.append("<string>");
      output.append(xml_escape(value.string_value()));
      output.append("</string>");
      break;
    case Type::kData:
      output.append("<data>");
      output.append(base64_encode(value.data_value()));
      output.append("</data>");
      break;
    case Type::kDate:
      output.append("<date>");
      output.append(xml_escape(value.string_value()));
      output.append("</date>");
      break;
    case Type::kArray: {
      output.append("<array>");
      for (const auto& item : value.array_items()) {
        output.push_back('\n');
        append_indent(output, depth + 1);
        dump_xml_value(item, output, depth + 1);
      }
      if (!value.array_items().empty()) {
        output.push_back('\n');
        append_indent(output, depth);
      }
      output.append("</array>");
      break;
    }
    case Type::kDictionary: {
      output.append("<dict>");
      for (const auto& item : value.dictionary_items()) {
        output.push_back('\n');
        append_indent(output, depth + 1);
        output.append("<key>");
        output.append(xml_escape(item.first));
        output.append("</key>\n");
        append_indent(output, depth + 1);
        dump_xml_value(item.second, output, depth + 1);
      }
      if (!value.dictionary_items().empty()) {
        output.push_back('\n');
        append_indent(output, depth);
      }
      output.append("</dict>");
      break;
    }
  }
}

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

std::string decode_entities(std::string_view text) {
  std::string output;
  output.reserve(text.size());
  std::size_t index = 0;
  while (index < text.size()) {
    if (text[index] != '&') {
      output.push_back(text[index++]);
      continue;
    }
    const std::size_t end = text.find(';', index + 1);
    if (end == std::string_view::npos) {
      output.push_back('&');
      ++index;
      continue;
    }
    const std::string_view entity = text.substr(index + 1, end - index - 1);
    if (entity == "amp") {
      output.push_back('&');
    } else if (entity == "lt") {
      output.push_back('<');
    } else if (entity == "gt") {
      output.push_back('>');
    } else if (entity == "quot") {
      output.push_back('"');
    } else if (entity == "apos") {
      output.push_back('\'');
    } else if (!entity.empty() && entity.front() == '#') {
      std::uint32_t code_point = 0;
      if (entity.size() > 2 && (entity[1] == 'x' || entity[1] == 'X')) {
        const std::string digits(entity.substr(2));
        code_point = static_cast<std::uint32_t>(
            std::strtoul(digits.c_str(), nullptr, 16));
      } else {
        const std::string digits(entity.substr(1));
        code_point = static_cast<std::uint32_t>(
            std::strtoul(digits.c_str(), nullptr, 10));
      }
      append_utf8(output, code_point);
    } else {
      output.append(text.substr(index, end - index + 1));
    }
    index = end + 1;
  }
  return output;
}

class XmlParser {
 public:
  explicit XmlParser(std::string_view text) : text_(text) {}

  // Locates a complete `<name...>...</name>` element anywhere in the input.
  // Apple WebObjects endpoints (most importantly the URL bag) wrap their
  // property lists in application envelopes shaped like
  // `<Document><Protocol><plist>...</plist></Protocol></Document>` or return
  // a bare `<dict>` inside such an envelope, so when the document does not
  // start with a plist element the payload is located explicitly.
  std::optional<std::string_view> find_element(std::string_view name) const {
    const std::string open = "<" + std::string(name);
    const std::string close = "</" + std::string(name) + ">";
    std::size_t search = 0;
    while (true) {
      const std::size_t start = text_.find(open, search);
      if (start == std::string_view::npos) return std::nullopt;
      const std::size_t after = start + open.size();
      if (after < text_.size()) {
        const char boundary = text_[after];
        if (boundary == '>' || boundary == '/' || boundary == ' ' ||
            boundary == '\t' || boundary == '\n' || boundary == '\r') {
          const std::size_t end = text_.rfind(close);
          if (end == std::string_view::npos || end <= start) {
            return std::nullopt;
          }
          return text_.substr(start, end + close.size() - start);
        }
      }
      search = start + 1;
    }
  }

  Result<PlistValue> parse() {
    if (starts_with(text_, "\xEF\xBB\xBF")) text_.remove_prefix(3);
    skip_misc();
    if (position_ >= text_.size()) {
      return make_error(ErrorCode::kParse, "property list is empty");
    }

    if (peek_tag_name() != "plist") {
      // Peel Apple WebObjects envelopes: prefer an embedded <plist>, then an
      // embedded <dict>, then a bare <key> fragment treated as dict content.
      if (auto embedded = find_element("plist")) {
        text_ = *embedded;
        position_ = 0;
        skip_misc();
      } else if (auto embedded = find_element("dict")) {
        text_ = *embedded;
        position_ = 0;
        return parse_value(0);
      } else if (text_.find("<key>") != std::string_view::npos) {
        owned_ = "<dict>" + std::string(text_) + "</dict>";
        text_ = owned_;
        position_ = 0;
        return parse_value(0);
      }
    }

    if (peek_tag_name() == "plist") {
      std::string name;
      if (!consume_open_tag(name) || name != "plist") {
        return make_error(ErrorCode::kParse, "plist root tag is malformed");
      }
      skip_misc();
      auto value = parse_value(0);
      if (!value) return value;
      skip_misc();
      std::string closing;
      if (!consume_close_tag(closing) || closing != "plist") {
        return make_error(ErrorCode::kParse, "plist root tag is not closed");
      }
      skip_misc();
      return value;
    }

    if (peek_tag_name() == "Document") {
      std::string name;
      if (!consume_open_tag(name)) {
        return make_error(ErrorCode::kParse, "Document tag is malformed");
      }
      skip_misc();
      auto value = parse_value(0);
      if (!value) return value;
      skip_misc();
      return value;
    }

    return parse_value(0);
  }

 private:
  void skip_misc() {
    while (position_ < text_.size()) {
      const char character = text_[position_];
      if (character == ' ' || character == '\n' || character == '\r' ||
          character == '\t') {
        ++position_;
        continue;
      }
      if (text_.substr(position_, 4) == "<!--") {
        const std::size_t end = text_.find("-->", position_ + 4);
        position_ = end == std::string_view::npos ? text_.size() : end + 3;
        continue;
      }
      if (text_.substr(position_, 2) == "<?") {
        const std::size_t end = text_.find("?>", position_ + 2);
        position_ = end == std::string_view::npos ? text_.size() : end + 2;
        continue;
      }
      if (text_.substr(position_, 2) == "<!") {
        std::size_t end = text_.find('>', position_ + 2);
        if (end == std::string_view::npos) {
          position_ = text_.size();
          return;
        }
        // The plist DOCTYPE has an internal subset with brackets in some
        // encoders. Skip to the matching bracket before looking for '>'.
        const std::size_t bracket = text_.find('[', position_ + 2);
        if (bracket != std::string_view::npos && bracket < end) {
          end = text_.find(']', bracket);
          if (end != std::string_view::npos) end = text_.find('>', end);
          if (end == std::string_view::npos) {
            position_ = text_.size();
            return;
          }
        }
        position_ = end + 1;
        continue;
      }
      break;
    }
  }

  std::string_view peek_tag_name() const {
    if (position_ >= text_.size() || text_[position_] != '<') return {};
    std::size_t index = position_ + 1;
    if (index < text_.size() && text_[index] == '/') return {};
    const std::size_t start = index;
    while (index < text_.size()) {
      const char character = text_[index];
      if (character == '>' || character == '/' || character == ' ' ||
          character == '\n' || character == '\r' || character == '\t') {
        break;
      }
      ++index;
    }
    return text_.substr(start, index - start);
  }

  bool consume_open_tag(std::string& name) {
    bool ignored = false;
    return consume_open_tag(name, ignored);
  }

  bool consume_open_tag(std::string& name, bool& self_closing) {
    if (position_ >= text_.size() || text_[position_] != '<') return false;
    std::size_t index = position_ + 1;
    if (index >= text_.size() || text_[index] == '/') return false;
    const std::size_t start = index;
    while (index < text_.size() && text_[index] != '>' && text_[index] != '/' &&
           text_[index] != ' ' && text_[index] != '\n' && text_[index] != '\r' &&
           text_[index] != '\t') {
      ++index;
    }
    name = std::string(text_.substr(start, index - start));
    self_closing = false;
    while (index < text_.size()) {
      if (text_[index] == '/') {
        self_closing = true;
        ++index;
      } else if (text_[index] == '>') {
        position_ = index + 1;
        return !name.empty();
      } else if (text_[index] == '"' || text_[index] == '\'') {
        const char quote = text_[index++];
        while (index < text_.size() && text_[index] != quote) ++index;
        if (index < text_.size()) ++index;
      } else {
        ++index;
      }
    }
    return false;
  }

  bool consume_close_tag(std::string& name) {
    if (position_ + 1 >= text_.size() || text_[position_] != '<' ||
        text_[position_ + 1] != '/') {
      return false;
    }
    std::size_t index = position_ + 2;
    const std::size_t start = index;
    while (index < text_.size() && text_[index] != '>' &&
           text_[index] != ' ' && text_[index] != '\n') {
      ++index;
    }
    name = std::string(text_.substr(start, index - start));
    const std::size_t end = text_.find('>', index);
    if (end == std::string_view::npos) return false;
    position_ = end + 1;
    return true;
  }

  Result<std::string> read_text() {
    const std::size_t start = position_;
    const std::size_t end = text_.find('<', start);
    if (end == std::string_view::npos) {
      return make_error(ErrorCode::kParse, "property list value is not closed");
    }
    position_ = end;
    return decode_entities(text_.substr(start, end - start));
  }

  Result<PlistValue> parse_value(std::size_t depth) {
    if (depth > kMaxDepth) {
      return make_error(ErrorCode::kParse, "property list nesting is too deep");
    }
    skip_misc();
    if (position_ >= text_.size() || text_[position_] != '<') {
      return make_error(ErrorCode::kParse, "property list value is missing");
    }
    if (text_.substr(position_, 2) == "</") {
      return make_error(ErrorCode::kParse, "property list value is empty");
    }

    std::string name;
    bool self_closing = false;
    if (!consume_open_tag(name, self_closing)) {
      return make_error(ErrorCode::kParse, "property list tag is malformed");
    }

    if (name == "dict") {
      PlistValue result = PlistValue::dictionary();
      if (self_closing) return result;
      while (true) {
        skip_misc();
        if (position_ >= text_.size()) {
          return make_error(ErrorCode::kParse, "dictionary is not closed");
        }
        if (text_.substr(position_, 2) == "</") {
          std::string closing;
          if (!consume_close_tag(closing) || closing != "dict") {
            return make_error(ErrorCode::kParse, "dictionary close tag is invalid");
          }
          return result;
        }
        std::string key_name;
        bool key_self_closing = false;
        if (!consume_open_tag(key_name, key_self_closing) || key_name != "key") {
          return make_error(ErrorCode::kParse, "dictionary key is missing");
        }
        std::string key;
        if (!key_self_closing) {
          auto key_text = read_text();
          if (!key_text) return key_text.error();
          key = std::move(key_text).value();
          std::string closing;
          if (!consume_close_tag(closing) || closing != "key") {
            return make_error(ErrorCode::kParse, "dictionary key is not closed");
          }
        }
        auto value = parse_value(depth + 1);
        if (!value) return value;
        result.set(std::move(key), std::move(value).value());
      }
    }

    if (name == "array") {
      PlistValue result = PlistValue::array();
      if (self_closing) return result;
      while (true) {
        skip_misc();
        if (position_ >= text_.size()) {
          return make_error(ErrorCode::kParse, "array is not closed");
        }
        if (text_.substr(position_, 2) == "</") {
          std::string closing;
          if (!consume_close_tag(closing) || closing != "array") {
            return make_error(ErrorCode::kParse, "array close tag is invalid");
          }
          return result;
        }
        auto value = parse_value(depth + 1);
        if (!value) return value;
        result.append(std::move(value).value());
      }
    }

    if (name == "true" || name == "false") {
      if (!self_closing) {
        std::string closing;
        if (!consume_close_tag(closing) || closing != name) {
          return make_error(ErrorCode::kParse, "boolean tag is not closed");
        }
      }
      return PlistValue::boolean(name == "true");
    }

    auto text = self_closing ? Result<std::string>(std::string()) : read_text();
    if (!text) return text.error();
    const std::string content = trim(text.value());

    if (!self_closing) {
      std::string closing;
      if (!consume_close_tag(closing) || closing != name) {
        return make_error(ErrorCode::kParse, "property list tag is not closed");
      }
    }

    if (name == "string") return PlistValue::string(text.value());
    if (name == "key") return PlistValue::string(text.value());
    if (name == "data") {
      auto decoded = base64_decode(content);
      if (!decoded) return decoded.error();
      return PlistValue::data(std::move(decoded).value());
    }
    if (name == "date") return PlistValue::date(content);
    if (name == "integer") {
      char* end = nullptr;
      errno = 0;
      const long long parsed = std::strtoll(content.c_str(), &end, 10);
      if (errno != 0 || end == nullptr || *end != '\0') {
        return make_error(ErrorCode::kParse, "integer value is malformed");
      }
      return PlistValue::integer(static_cast<std::int64_t>(parsed));
    }
    if (name == "real") {
      char* end = nullptr;
      errno = 0;
      const double parsed = std::strtod(content.c_str(), &end);
      if (errno != 0 || end == nullptr || *end != '\0' || !std::isfinite(parsed)) {
        return make_error(ErrorCode::kParse, "real value is malformed");
      }
      return PlistValue::real(parsed);
    }
    return make_error(ErrorCode::kParse, "property list tag is unsupported");
  }

  std::string_view text_;
  // Backing storage when a bare <key> fragment is wrapped into a <dict>.
  std::string owned_;
  std::size_t position_ = 0;
};

std::uint64_t read_be(const Bytes& data, std::size_t offset, std::size_t size) {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < size; ++index) {
    value = (value << 8) | data[offset + index];
  }
  return value;
}

std::string format_apple_date(double seconds) {
  const auto unix_seconds =
      static_cast<std::time_t>(seconds + kAppleEpochOffset);
  std::tm tm{};
  gmtime_r(&unix_seconds, &tm);
  char buffer[32];
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buffer;
}

class BinaryParser {
 public:
  explicit BinaryParser(const Bytes& data) : data_(data) {}

  Result<PlistValue> parse() {
    if (data_.size() < 8 + 32 ||
        std::memcmp(data_.data(), "bplist00", 8) != 0) {
      return make_error(ErrorCode::kParse, "binary plist header is invalid");
    }
    const std::size_t trailer = data_.size() - 32;
    offset_size_ = data_[trailer + 6];
    reference_size_ = data_[trailer + 7];
    object_count_ = read_be(data_, trailer + 8, 8);
    top_object_ = read_be(data_, trailer + 16, 8);
    offset_table_ = read_be(data_, trailer + 24, 8);

    if (offset_size_ < 1 || offset_size_ > 8 || reference_size_ < 1 ||
        reference_size_ > 8) {
      return make_error(ErrorCode::kParse, "binary plist trailer is invalid");
    }
    if (object_count_ == 0 || top_object_ >= object_count_) {
      return make_error(ErrorCode::kParse, "binary plist object table is invalid");
    }
    if (offset_table_ < 8 || offset_table_ + object_count_ * offset_size_ >
                                data_.size()) {
      return make_error(ErrorCode::kParse, "binary plist offsets exceed the file");
    }
    return parse_object(top_object_, 0);
  }

 private:
  Result<std::uint64_t> object_offset(std::uint64_t reference) const {
    if (reference >= object_count_) {
      return make_error(ErrorCode::kParse, "binary plist reference is out of range");
    }
    const std::size_t position =
        static_cast<std::size_t>(offset_table_ + reference * offset_size_);
    return read_be(data_, position, offset_size_);
  }

  Result<std::uint64_t> read_length(std::size_t& position,
                                    std::uint8_t info) const {
    if (info != 0x0f) return static_cast<std::uint64_t>(info);
    if (position >= data_.size()) {
      return make_error(ErrorCode::kParse, "binary plist length is truncated");
    }
    const std::uint8_t marker = data_[position++];
    if ((marker & 0xf0) != 0x10) {
      return make_error(ErrorCode::kParse, "binary plist length is invalid");
    }
    const std::size_t count = static_cast<std::size_t>(1) << (marker & 0x0f);
    if (count > 8 || position + count > data_.size()) {
      return make_error(ErrorCode::kParse, "binary plist length is too large");
    }
    const std::uint64_t value = read_be(data_, position, count);
    position += count;
    return value;
  }

  Result<PlistValue> parse_object(std::uint64_t reference, std::size_t depth) {
    if (depth > kMaxDepth) {
      return make_error(ErrorCode::kParse, "binary plist nesting is too deep");
    }
    auto offset = object_offset(reference);
    if (!offset) return offset.error();
    std::size_t position = static_cast<std::size_t>(offset.value());
    if (position >= data_.size()) {
      return make_error(ErrorCode::kParse, "binary plist object offset is invalid");
    }
    const std::uint8_t marker = data_[position++];
    const std::uint8_t type = marker >> 4;
    const std::uint8_t info = marker & 0x0f;

    switch (type) {
      case 0x0:
        if (info == 0x0) return PlistValue::null();
        if (info == 0x8) return PlistValue::boolean(false);
        if (info == 0x9) return PlistValue::boolean(true);
        return make_error(ErrorCode::kParse, "binary plist marker is unsupported");
      case 0x1: {
        const std::size_t count = static_cast<std::size_t>(1) << info;
        if (count > 8 || position + count > data_.size()) {
          return make_error(ErrorCode::kParse, "binary plist integer is truncated");
        }
        std::uint64_t value = read_be(data_, position, count);
        if (count < 8 && (value & (std::uint64_t{1} << (count * 8 - 1))) != 0) {
          value |= ~std::uint64_t{0} << (count * 8);
        }
        return PlistValue::integer(static_cast<std::int64_t>(value));
      }
      case 0x2: {
        const std::size_t count = static_cast<std::size_t>(1) << info;
        if (count != 4 && count != 8) {
          return make_error(ErrorCode::kParse, "binary plist real size is invalid");
        }
        if (position + count > data_.size()) {
          return make_error(ErrorCode::kParse, "binary plist real is truncated");
        }
        if (count == 4) {
          const std::uint32_t bits =
              static_cast<std::uint32_t>(read_be(data_, position, 4));
          float value = 0;
          std::memcpy(&value, &bits, sizeof(value));
          return PlistValue::real(static_cast<double>(value));
        }
        const std::uint64_t bits = read_be(data_, position, 8);
        double value = 0;
        std::memcpy(&value, &bits, sizeof(value));
        return PlistValue::real(value);
      }
      case 0x3: {
        if (position + 8 > data_.size()) {
          return make_error(ErrorCode::kParse, "binary plist date is truncated");
        }
        const std::uint64_t bits = read_be(data_, position, 8);
        double seconds = 0;
        std::memcpy(&seconds, &bits, sizeof(seconds));
        return PlistValue::date(format_apple_date(seconds));
      }
      case 0x4: {
        auto length = read_length(position, info);
        if (!length) return length.error();
        if (length.value() > data_.size() - position) {
          return make_error(ErrorCode::kParse, "binary plist data is truncated");
        }
        Bytes value(data_.begin() + static_cast<std::ptrdiff_t>(position),
                    data_.begin() +
                        static_cast<std::ptrdiff_t>(position + length.value()));
        return PlistValue::data(std::move(value));
      }
      case 0x5: {
        auto length = read_length(position, info);
        if (!length) return length.error();
        if (length.value() > data_.size() - position) {
          return make_error(ErrorCode::kParse, "binary plist string is truncated");
        }
        return PlistValue::string(std::string(
            reinterpret_cast<const char*>(data_.data() + position),
            static_cast<std::size_t>(length.value())));
      }
      case 0x6: {
        auto length = read_length(position, info);
        if (!length) return length.error();
        if (length.value() * 2 > data_.size() - position) {
          return make_error(ErrorCode::kParse, "binary plist UTF-16 string is truncated");
        }
        std::string output;
        for (std::uint64_t index = 0; index < length.value(); ++index) {
          std::uint32_t code_point = static_cast<std::uint16_t>(
              read_be(data_, position + static_cast<std::size_t>(index) * 2, 2));
          if (code_point >= 0xd800 && code_point <= 0xdbff &&
              index + 1 < length.value()) {
            const std::uint32_t low = static_cast<std::uint16_t>(
                read_be(data_,
                        position + static_cast<std::size_t>(index + 1) * 2, 2));
            if (low >= 0xdc00 && low <= 0xdfff) {
              code_point =
                  0x10000 + ((code_point - 0xd800) << 10) + (low - 0xdc00);
              ++index;
            } else {
              code_point = 0xfffd;
            }
          } else if (code_point >= 0xd800 && code_point <= 0xdfff) {
            code_point = 0xfffd;
          }
          append_utf8(output, code_point);
        }
        return PlistValue::string(std::move(output));
      }
      case 0x7: {
        auto length = read_length(position, info);
        if (!length) return length.error();
        if (length.value() > data_.size() - position) {
          return make_error(ErrorCode::kParse, "binary plist UTF-8 string is truncated");
        }
        return PlistValue::string(std::string(
            reinterpret_cast<const char*>(data_.data() + position),
            static_cast<std::size_t>(length.value())));
      }
      case 0x8: {
        const std::size_t count = info + 1;
        if (position + count > data_.size()) {
          return make_error(ErrorCode::kParse, "binary plist UID is truncated");
        }
        return PlistValue::integer(
            static_cast<std::int64_t>(read_be(data_, position, count)));
      }
      case 0xa:
      case 0xc: {
        auto length = read_length(position, info);
        if (!length) return length.error();
        if (length.value() > (data_.size() - position) / reference_size_) {
          return make_error(ErrorCode::kParse, "binary plist array is truncated");
        }
        PlistValue result = PlistValue::array();
        for (std::uint64_t index = 0; index < length.value(); ++index) {
          const std::uint64_t child =
              read_be(data_, position + static_cast<std::size_t>(index) *
                                       reference_size_,
                      reference_size_);
          auto value = parse_object(child, depth + 1);
          if (!value) return value;
          result.append(std::move(value).value());
        }
        return result;
      }
      case 0xd: {
        auto length = read_length(position, info);
        if (!length) return length.error();
        if (length.value() * 2 >
            (data_.size() - position) / reference_size_) {
          return make_error(ErrorCode::kParse, "binary plist dictionary is truncated");
        }
        PlistValue result = PlistValue::dictionary();
        for (std::uint64_t index = 0; index < length.value(); ++index) {
          const std::size_t key_position =
              position + static_cast<std::size_t>(index) * reference_size_;
          const std::size_t value_position =
              position + static_cast<std::size_t>(length.value() + index) *
                             reference_size_;
          const std::uint64_t key_reference =
              read_be(data_, key_position, reference_size_);
          const std::uint64_t value_reference =
              read_be(data_, value_position, reference_size_);
          auto key = parse_object(key_reference, depth + 1);
          if (!key) return key;
          if (!key.value().is_string()) {
            return make_error(ErrorCode::kParse,
                              "binary plist dictionary key is not a string");
          }
          auto value = parse_object(value_reference, depth + 1);
          if (!value) return value;
          result.set(key.value().string_value(), std::move(value).value());
        }
        return result;
      }
      default:
        return make_error(ErrorCode::kParse, "binary plist object type is unsupported");
    }
  }

  const Bytes& data_;
  std::uint8_t offset_size_ = 0;
  std::uint8_t reference_size_ = 0;
  std::uint64_t object_count_ = 0;
  std::uint64_t top_object_ = 0;
  std::uint64_t offset_table_ = 0;
};

}  // namespace

PlistValue PlistValue::null() {
  PlistValue value;
  value.type_ = Type::kNull;
  return value;
}

PlistValue PlistValue::boolean(bool input) {
  PlistValue value;
  value.type_ = Type::kBool;
  value.bool_ = input;
  return value;
}

PlistValue PlistValue::integer(std::int64_t input) {
  PlistValue value;
  value.type_ = Type::kInteger;
  value.integer_ = input;
  return value;
}

PlistValue PlistValue::real(double input) {
  PlistValue value;
  value.type_ = Type::kReal;
  value.real_ = input;
  return value;
}

PlistValue PlistValue::string(std::string input) {
  PlistValue value;
  value.type_ = Type::kString;
  value.string_ = std::move(input);
  return value;
}

PlistValue PlistValue::data(Bytes input) {
  PlistValue value;
  value.type_ = Type::kData;
  value.data_ = std::move(input);
  return value;
}

PlistValue PlistValue::date(std::string input) {
  PlistValue value;
  value.type_ = Type::kDate;
  value.string_ = std::move(input);
  return value;
}

PlistValue PlistValue::array(std::vector<PlistValue> values) {
  PlistValue value;
  value.type_ = Type::kArray;
  value.array_ = std::move(values);
  return value;
}

PlistValue PlistValue::dictionary(
    std::vector<std::pair<std::string, PlistValue>> values) {
  PlistValue value;
  value.type_ = Type::kDictionary;
  value.dictionary_ = std::move(values);
  return value;
}

const PlistValue* PlistValue::find(std::string_view key) const {
  if (type_ != Type::kDictionary) return nullptr;
  for (const auto& item : dictionary_) {
    if (item.first == key) return &item.second;
  }
  return nullptr;
}

const PlistValue* PlistValue::at(std::size_t index) const {
  if (type_ != Type::kArray || index >= array_.size()) return nullptr;
  return &array_[index];
}

std::size_t PlistValue::size() const {
  if (type_ == Type::kArray) return array_.size();
  if (type_ == Type::kDictionary) return dictionary_.size();
  if (type_ == Type::kString) return string_.size();
  if (type_ == Type::kData) return data_.size();
  return 0;
}

std::optional<std::string> PlistValue::string_for(std::string_view key) const {
  const PlistValue* value = find(key);
  if (value == nullptr || !value->is_string()) return std::nullopt;
  return value->string_;
}

std::string PlistValue::string_or(std::string_view key,
                                  std::string fallback) const {
  auto value = string_for(key);
  return value ? std::move(*value) : std::move(fallback);
}

std::optional<std::int64_t> PlistValue::integer_for(
    std::string_view key) const {
  const PlistValue* value = find(key);
  if (value == nullptr) return std::nullopt;
  if (value->is_integer()) return value->integer_;
  if (value->is_real()) return static_cast<std::int64_t>(value->real_);
  if (value->is_string()) {
    char* end = nullptr;
    const long long parsed = std::strtoll(value->string_.c_str(), &end, 10);
    if (end != nullptr && *end == '\0') return static_cast<std::int64_t>(parsed);
  }
  return std::nullopt;
}

bool PlistValue::bool_or(std::string_view key, bool fallback) const {
  const PlistValue* value = find(key);
  return value != nullptr && value->is_bool() ? value->bool_ : fallback;
}

void PlistValue::set(std::string key, PlistValue value) {
  if (type_ != Type::kDictionary) {
    type_ = Type::kDictionary;
    dictionary_.clear();
  }
  for (auto& item : dictionary_) {
    if (item.first == key) {
      item.second = std::move(value);
      return;
    }
  }
  dictionary_.emplace_back(std::move(key), std::move(value));
}

std::string PlistValue::dump_xml() const {
  std::string output =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
      "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
      "<plist version=\"1.0\">\n";
  dump_xml_value(*this, output, 0);
  output.append("\n</plist>\n");
  return output;
}

Result<PlistValue> PlistValue::parse_xml(std::string_view text) {
  return XmlParser(text).parse();
}

Result<PlistValue> PlistValue::parse_binary(const Bytes& data) {
  return BinaryParser(data).parse();
}


namespace {

class BinaryWriter {
 public:
  explicit BinaryWriter(const PlistValue& root) { flatten(root, 0); }

  Bytes finish() const {
    Bytes body;
    std::vector<std::uint64_t> offsets(objects_.size(), 0);
    constexpr std::size_t kHeaderSize = 8;
    for (std::size_t index = 0; index < objects_.size(); ++index) {
      offsets[index] = body.size() + kHeaderSize;
      append_object(body, index);
    }
    const std::size_t ref_size = byte_width(objects_.size());
    const std::size_t offset_size = byte_width(body.size() + kHeaderSize);
    const std::uint64_t table_offset = body.size() + kHeaderSize;
    for (const std::uint64_t offset : offsets) {
      append_be(body, offset, offset_size);
    }
    Bytes output;
    output.insert(output.end(), {'b', 'p', 'l', 'i', 's', 't', '0', '0'});
    output.insert(output.end(), body.begin(), body.end());
    output.insert(output.end(), 6, 0);
    output.push_back(static_cast<std::uint8_t>(offset_size));
    output.push_back(static_cast<std::uint8_t>(ref_size));
    append_be(output, objects_.size(), 8);
    append_be(output, 0, 8);
    append_be(output, table_offset, 8);
    return output;
  }

 private:
  struct Entry {
    PlistValue value;
    std::vector<std::size_t> refs;
  };

  std::size_t flatten(const PlistValue& value, std::size_t depth) {
    if (depth > 64) return 0;
    const std::size_t index = objects_.size();
    objects_.push_back(Entry{value, {}});
    if (value.is_array()) {
      for (const auto& item : value.array_items()) {
        const std::size_t child = flatten(item, depth + 1);
        objects_[index].refs.push_back(child);
      }
    } else if (value.is_dictionary()) {
      std::vector<std::size_t> keys;
      std::vector<std::size_t> values;
      keys.reserve(value.dictionary_items().size());
      values.reserve(value.dictionary_items().size());
      for (const auto& item : value.dictionary_items()) {
        keys.push_back(flatten(PlistValue::string(item.first), depth + 1));
        values.push_back(flatten(item.second, depth + 1));
      }
      objects_[index].refs = std::move(keys);
      objects_[index].refs.insert(objects_[index].refs.end(), values.begin(),
                                  values.end());
    }
    return index;
  }

  static std::size_t byte_width(std::uint64_t value) {
    std::size_t width = 1;
    while (width < 8 && (value >> (width * 8)) != 0) ++width;
    return width;
  }

  static void append_be(Bytes& output, std::uint64_t value, std::size_t width) {
    for (std::size_t index = 0; index < width; ++index) {
      output.push_back(static_cast<std::uint8_t>(
          value >> ((width - index - 1) * 8)));
    }
  }

  static void append_scalar(Bytes& output, std::uint8_t marker,
                            std::uint64_t value, std::size_t width) {
    output.push_back(marker);
    append_be(output, value, width);
  }

  static void append_length(Bytes& output, std::uint8_t base,
                            std::size_t length) {
    if (length < 15) {
      output.push_back(static_cast<std::uint8_t>(base | length));
      return;
    }
    output.push_back(static_cast<std::uint8_t>(base | 0x0f));
    if (length <= 0xff) {
      append_scalar(output, 0x10, length, 1);
    } else if (length <= 0xffff) {
      append_scalar(output, 0x11, length, 2);
    } else if (length <= 0xffffffffULL) {
      append_scalar(output, 0x12, length, 4);
    } else {
      append_scalar(output, 0x13, length, 8);
    }
  }

  static void append_utf16(Bytes& output, std::string_view text) {
    for (std::size_t index = 0; index < text.size();) {
      const auto lead = static_cast<unsigned char>(text[index]);
      std::uint32_t code_point = lead;
      std::size_t consumed = 1;
      if ((lead & 0xe0) == 0xc0 && index + 1 < text.size()) {
        code_point = ((lead & 0x1f) << 6) |
                     (static_cast<unsigned char>(text[index + 1]) & 0x3f);
        consumed = 2;
      } else if ((lead & 0xf0) == 0xe0 && index + 2 < text.size()) {
        code_point = ((lead & 0x0f) << 12) |
                     ((static_cast<unsigned char>(text[index + 1]) & 0x3f) << 6) |
                     (static_cast<unsigned char>(text[index + 2]) & 0x3f);
        consumed = 3;
      } else if ((lead & 0xf8) == 0xf0 && index + 3 < text.size()) {
        code_point = ((lead & 0x07) << 18) |
                     ((static_cast<unsigned char>(text[index + 1]) & 0x3f) << 12) |
                     ((static_cast<unsigned char>(text[index + 2]) & 0x3f) << 6) |
                     (static_cast<unsigned char>(text[index + 3]) & 0x3f);
        consumed = 4;
      }
      index += consumed;
      if (code_point >= 0x10000) {
        code_point -= 0x10000;
        const std::uint16_t high =
            static_cast<std::uint16_t>(0xd800 + (code_point >> 10));
        const std::uint16_t low =
            static_cast<std::uint16_t>(0xdc00 + (code_point & 0x3ff));
        output.push_back(static_cast<std::uint8_t>(high >> 8));
        output.push_back(static_cast<std::uint8_t>(high & 0xff));
        output.push_back(static_cast<std::uint8_t>(low >> 8));
        output.push_back(static_cast<std::uint8_t>(low & 0xff));
      } else {
        output.push_back(static_cast<std::uint8_t>(code_point >> 8));
        output.push_back(static_cast<std::uint8_t>(code_point & 0xff));
      }
    }
  }

  static std::size_t utf16_length(std::string_view text) {
    std::size_t units = 0;
    for (std::size_t index = 0; index < text.size();) {
      const auto lead = static_cast<unsigned char>(text[index]);
      std::size_t consumed = 1;
      std::uint32_t code_point = lead;
      if ((lead & 0xe0) == 0xc0 && index + 1 < text.size()) {
        consumed = 2;
        code_point = 0x80;
      } else if ((lead & 0xf0) == 0xe0 && index + 2 < text.size()) {
        consumed = 3;
        code_point = 0x800;
      } else if ((lead & 0xf8) == 0xf0 && index + 3 < text.size()) {
        consumed = 4;
        code_point = 0x10000;
      }
      units += code_point >= 0x10000 ? 2 : 1;
      index += consumed;
    }
    return units;
  }

  void append_object(Bytes& output, std::size_t index) const {
    const PlistValue& value = objects_[index].value;
    switch (value.type()) {
      case PlistValue::Type::kNull:
        output.push_back(0x00);
        return;
      case PlistValue::Type::kBool:
        output.push_back(value.bool_value() ? 0x09 : 0x08);
        return;
      case PlistValue::Type::kInteger: {
        const std::int64_t integer = value.integer_value();
        if (integer >= 0) {
          if (integer <= 0xff) {
            append_scalar(output, 0x10, static_cast<std::uint64_t>(integer), 1);
          } else if (integer <= 0xffff) {
            append_scalar(output, 0x11, static_cast<std::uint64_t>(integer), 2);
          } else if (integer <= 0xffffffffLL) {
            append_scalar(output, 0x12, static_cast<std::uint64_t>(integer), 4);
          } else {
            append_scalar(output, 0x13, static_cast<std::uint64_t>(integer), 8);
          }
        } else {
          append_scalar(output, 0x13, static_cast<std::uint64_t>(integer), 8);
        }
        return;
      }
      case PlistValue::Type::kReal: {
        output.push_back(0x23);
        const double value_real = value.real_value();
        std::uint64_t bits = 0;
        std::memcpy(&bits, &value_real, sizeof(bits));
        append_be(output, bits, 8);
        return;
      }
      case PlistValue::Type::kDate: {
        std::tm tm{};
        std::istringstream input(value.string_value());
        input >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
        double seconds = 0;
        if (!input.fail()) {
          const std::time_t unix_seconds = timegm(&tm);
          seconds = static_cast<double>(unix_seconds) - kAppleEpochOffset;
        }
        output.push_back(0x33);
        std::uint64_t bits = 0;
        std::memcpy(&bits, &seconds, sizeof(bits));
        append_be(output, bits, 8);
        return;
      }
      case PlistValue::Type::kData:
        append_length(output, 0x40, value.data_value().size());
        output.insert(output.end(), value.data_value().begin(),
                      value.data_value().end());
        return;
      case PlistValue::Type::kString: {
        const std::string& text = value.string_value();
        const bool ascii = std::all_of(
            text.begin(), text.end(),
            [](char character) {
              return static_cast<unsigned char>(character) < 0x80;
            });
        if (ascii) {
          append_length(output, 0x50, text.size());
          output.insert(output.end(), text.begin(), text.end());
        } else {
          append_length(output, 0x60, utf16_length(text));
          append_utf16(output, text);
        }
        return;
      }
      case PlistValue::Type::kArray: {
        append_length(output, 0xa0, value.array_items().size());
        const std::size_t ref_size = byte_width(objects_.size());
        for (const std::size_t ref : objects_[index].refs) {
          append_be(output, ref, ref_size);
        }
        return;
      }
      case PlistValue::Type::kDictionary: {
        append_length(output, 0xd0, value.dictionary_items().size());
        const std::size_t ref_size = byte_width(objects_.size());
        for (const std::size_t ref : objects_[index].refs) {
          append_be(output, ref, ref_size);
        }
        return;
      }
    }
  }

  std::vector<Entry> objects_;
};

}  // namespace

Bytes PlistValue::dump_binary() const {
  return BinaryWriter(*this).finish();
}

Result<PlistValue> PlistValue::parse(const Bytes& data) {
  if (data.size() >= 8 &&
      std::memcmp(data.data(), "bplist00", 8) == 0) {
    return parse_binary(data);
  }
  return parse_xml(std::string_view(
      reinterpret_cast<const char*>(data.data()), data.size()));
}

}  // namespace ipashelf
