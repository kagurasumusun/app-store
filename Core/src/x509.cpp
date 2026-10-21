// SPDX-License-Identifier: MIT

#include "ipashelf/x509.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "ipashelf/crypto.hpp"

namespace ipashelf {
namespace {

constexpr std::string_view kEcdsaWithSha256 =
    "1.2.840.10045.4.3.2";
constexpr std::string_view kEcPublicKey = "1.2.840.10045.2.1";
constexpr std::string_view kPrime256v1 = "1.2.840.10045.3.1.7";
constexpr std::string_view kSubjectKeyId = "2.5.29.14";
constexpr std::string_view kKeyUsage = "2.5.29.15";
constexpr std::string_view kSubjectAltName = "2.5.29.17";
constexpr std::string_view kBasicConstraints = "2.5.29.19";
constexpr std::string_view kAuthorityKeyId = "2.5.29.35";
constexpr std::string_view kExtendedKeyUsage = "2.5.29.37";
constexpr std::string_view kCommonName = "2.5.4.3";
constexpr std::string_view kCountry = "2.5.4.6";
constexpr std::string_view kOrganization = "2.5.4.10";
constexpr std::string_view kOrganizationalUnit = "2.5.4.11";
constexpr std::string_view kServerAuth = "1.3.6.1.5.5.7.3.1";

Bytes wrap(std::uint8_t tag, const Bytes& content) {
  Bytes output;
  output.push_back(tag);
  const std::size_t size = content.size();
  if (size < 0x80) {
    output.push_back(static_cast<std::uint8_t>(size));
  } else if (size <= 0xff) {
    output.push_back(0x81);
    output.push_back(static_cast<std::uint8_t>(size));
  } else if (size <= 0xffff) {
    output.push_back(0x82);
    output.push_back(static_cast<std::uint8_t>((size >> 8) & 0xff));
    output.push_back(static_cast<std::uint8_t>(size & 0xff));
  } else {
    output.push_back(0x83);
    output.push_back(static_cast<std::uint8_t>((size >> 16) & 0xff));
    output.push_back(static_cast<std::uint8_t>((size >> 8) & 0xff));
    output.push_back(static_cast<std::uint8_t>(size & 0xff));
  }
  output.insert(output.end(), content.begin(), content.end());
  return output;
}

Bytes sequence(const std::vector<Bytes>& items) {
  Bytes content;
  for (const auto& item : items) {
    content.insert(content.end(), item.begin(), item.end());
  }
  return wrap(0x30, content);
}

Bytes set(const std::vector<Bytes>& items) {
  Bytes content;
  for (const auto& item : items) {
    content.insert(content.end(), item.begin(), item.end());
  }
  return wrap(0x31, content);
}

Bytes context_explicit(std::uint8_t index, const Bytes& value) {
  return wrap(static_cast<std::uint8_t>(0xa0 | index), value);
}

Bytes utf8_string(std::string_view value) {
  Bytes content(value.begin(), value.end());
  return wrap(0x0c, content);
}

Bytes printable_string(std::string_view value) {
  Bytes content(value.begin(), value.end());
  return wrap(0x13, content);
}

Bytes octet_string(const Bytes& value) { return wrap(0x04, value); }

Bytes bit_string(const Bytes& value) {
  Bytes content;
  content.push_back(0x00);
  content.insert(content.end(), value.begin(), value.end());
  return wrap(0x03, content);
}

Bytes boolean_value(bool value) {
  Bytes content;
  content.push_back(value ? 0xff : 0x00);
  return wrap(0x01, content);
}

Bytes time_value(std::time_t value) {
  std::tm parts{};
#if defined(_WIN32)
  gmtime_s(&parts, &value);
#else
  gmtime_r(&value, &parts);
#endif
  char buffer[32];
  const int year = parts.tm_year + 1900;
  if (year >= 1950 && year < 2050) {
    std::snprintf(buffer, sizeof(buffer), "%02d%02d%02d%02d%02d%02dZ",
                  year % 100, parts.tm_mon + 1, parts.tm_mday, parts.tm_hour,
                  parts.tm_min, parts.tm_sec);
    const std::string text(buffer);
    return wrap(0x17, Bytes(text.begin(), text.end()));
  }
  std::snprintf(buffer, sizeof(buffer), "%04d%02d%02d%02d%02d%02dZ", year,
                parts.tm_mon + 1, parts.tm_mday, parts.tm_hour, parts.tm_min,
                parts.tm_sec);
  const std::string text(buffer);
  return wrap(0x18, Bytes(text.begin(), text.end()));
}

Bytes algorithm_identifier() {
  auto oid = der_object_identifier(kEcdsaWithSha256);
  // RFC 5758 section 3.2: the parameters of ecdsa-with-SHA256 "MUST be
  // absent". An earlier version appended INTEGER 0 here, which made every
  // generated certificate unusable with strict parsers (including Apple's
  // trust evaluation) even though lenient ones still decoded it.
  return sequence({oid.value()});
}

Result<Bytes> name_der(const CertificateName& name) {
  std::vector<Bytes> rdns;
  const auto add = [&rdns](std::string_view oid, const Bytes& value) {
    auto encoded = der_object_identifier(oid);
    if (!encoded) return;
    rdns.push_back(set({sequence({encoded.value(), value})}));
  };
  if (!name.country.empty()) add(kCountry, printable_string(name.country));
  if (!name.organization.empty()) {
    add(kOrganization, utf8_string(name.organization));
  }
  if (!name.organizational_unit.empty()) {
    add(kOrganizationalUnit, utf8_string(name.organizational_unit));
  }
  if (!name.common_name.empty()) add(kCommonName, utf8_string(name.common_name));
  if (rdns.empty()) {
    return make_error(ErrorCode::kInvalidArgument,
                      "certificate name needs at least one field");
  }
  Bytes content;
  for (const auto& rdn : rdns) {
    content.insert(content.end(), rdn.begin(), rdn.end());
  }
  return wrap(0x30, content);
}

Bytes extension(std::string_view oid, bool critical, const Bytes& value) {
  std::vector<Bytes> parts;
  parts.push_back(der_object_identifier(oid).value());
  if (critical) parts.push_back(boolean_value(true));
  parts.push_back(octet_string(value));
  return sequence(parts);
}

// DER BIT STRING for KeyUsage with a correct unused-bits count (bits are
// numbered as in RFC 5280: digitalSignature=0, keyCertSign=5, cRLSign=6).
Bytes key_usage_value(std::initializer_list<int> bits) {
  int highest = 0;
  for (int bit : bits) highest = std::max(highest, bit);
  const std::size_t octets = static_cast<std::size_t>(highest / 8) + 1;
  Bytes content;
  content.push_back(static_cast<std::uint8_t>(octets * 8 - highest - 1));
  content.resize(1 + octets, 0x00);
  for (int bit : bits) {
    content[1 + static_cast<std::size_t>(bit / 8)] |=
        static_cast<std::uint8_t>(0x80 >> (bit % 8));
  }
  return wrap(0x03, content);
}

// DER INTEGER for a fixed-width unsigned magnitude (an ECDSA r/s value):
// leading zeros are stripped, and one is re-added when the value would
// otherwise look negative.
Bytes der_unsigned_integer(const std::uint8_t* data, std::size_t size) {
  if (size == 0) return wrap(0x02, Bytes{0x00});
  while (size > 1 && data[0] == 0x00) {
    ++data;
    --size;
  }
  Bytes content;
  if ((data[0] & 0x80) != 0) content.push_back(0x00);
  content.insert(content.end(), data, data + size);
  return wrap(0x02, content);
}

// Subject key identifier derived from the SPKI the caller already built.
// A parse failure simply omits SKID/AKID: both only guide path building
// and must never fail an otherwise valid certificate.
Bytes subject_key_id(const Bytes& spki) {
  std::size_t offset = 0;
  auto outer = der_read_element(spki, offset);
  if (!outer || outer.value().tag != 0x30) return {};
  std::size_t inner = 0;
  auto algorithm = der_read_element(outer.value().content, inner);
  if (!algorithm) return {};
  auto key = der_read_element(outer.value().content, inner);
  if (!key || key.value().tag != 0x03 || key.value().content.size() != 66 ||
      key.value().content.front() != 0x00) {
    return {};
  }
  const Bytes point(key.value().content.begin() + 1,
                    key.value().content.end());
  return ec_key_identifier(point);
}

}  // namespace

// ---------------------------------------------------------------------------
// Public encoding helpers
// ---------------------------------------------------------------------------

Result<Bytes> der_object_identifier(std::string_view dotted) {
  if (dotted.empty()) {
    return make_error(ErrorCode::kInvalidArgument, "empty OID");
  }
  std::uint64_t first = 0;
  const std::string text(dotted);
  std::size_t position = 0;
  std::uint64_t components[16];
  std::size_t count = 0;
  while (position <= text.size()) {
    const std::size_t dot = text.find('.', position);
    const std::string part =
        dot == std::string::npos ? text.substr(position)
                                 : text.substr(position, dot - position);
    if (part.empty()) {
      return make_error(ErrorCode::kInvalidArgument, "malformed OID");
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long long value = std::strtoull(part.c_str(), &end, 10);
    if (errno != 0 || end == nullptr || *end != '\0') {
      return make_error(ErrorCode::kInvalidArgument, "malformed OID component");
    }
    if (count >= 16) {
      return make_error(ErrorCode::kInvalidArgument, "OID has too many arcs");
    }
    components[count++] = value;
    if (dot == std::string::npos) break;
    position = dot + 1;
  }
  if (count < 2) {
    return make_error(ErrorCode::kInvalidArgument, "OID needs two arcs");
  }
  first = components[0] * 40 + components[1];
  (void)first;

  Bytes content;
  const auto push_base128 = [&content](std::uint64_t value) {
    std::uint8_t buffer[10];
    std::size_t size = 0;
    do {
      buffer[size++] = static_cast<std::uint8_t>(value & 0x7f);
      value >>= 7;
    } while (value > 0);
    while (size > 0) {
      --size;
      std::uint8_t byte = buffer[size];
      if (size > 0) byte |= 0x80;
      content.push_back(byte);
    }
  };
  push_base128(components[0] * 40 + components[1]);
  for (std::size_t index = 2; index < count; ++index) {
    push_base128(components[index]);
  }
  return wrap(0x06, content);
}

Bytes der_integer(std::int64_t value) {
  Bytes content;
  if (value == 0) {
    content.push_back(0);
  } else if (value > 0) {
    std::uint64_t magnitude = static_cast<std::uint64_t>(value);
    while (magnitude > 0) {
      content.insert(content.begin(),
                     static_cast<std::uint8_t>(magnitude & 0xff));
      magnitude >>= 8;
    }
    if ((content.front() & 0x80) != 0) content.insert(content.begin(), 0x00);
  } else {
    // Two's complement, minimal length.
    std::uint64_t magnitude = static_cast<std::uint64_t>(-(value + 1)) + 1;
    std::vector<std::uint8_t> bytes;
    while (magnitude > 0) {
      bytes.insert(bytes.begin(), static_cast<std::uint8_t>(magnitude & 0xff));
      magnitude >>= 8;
    }
    for (auto& byte : bytes) byte = static_cast<std::uint8_t>(~byte);
    std::uint64_t carry = 1;
    for (std::size_t index = bytes.size(); index > 0; --index) {
      const std::uint64_t sum = static_cast<std::uint64_t>(bytes[index - 1]) + carry;
      bytes[index - 1] = static_cast<std::uint8_t>(sum & 0xff);
      carry = sum >> 8;
    }
    while (bytes.size() > 1 && bytes.front() == 0xff &&
           (bytes[1] & 0x80) != 0) {
      bytes.erase(bytes.begin());
    }
    content = std::move(bytes);
  }
  return wrap(0x02, content);
}

Result<Bytes> ec_public_key_info(const Bytes& uncompressed_point) {
  if (uncompressed_point.size() != 65 || uncompressed_point.front() != 0x04) {
    return make_error(ErrorCode::kInvalidArgument,
                      "expected an uncompressed 65-byte P-256 point");
  }
  auto algorithm = sequence(
      {der_object_identifier(kEcPublicKey).value(),
       der_object_identifier(kPrime256v1).value()});
  auto key = bit_string(uncompressed_point);
  return sequence({algorithm, key});
}

Result<Bytes> build_ec_certificate(const CertificateOptions& options,
                                   const Bytes& subject_public_key_info,
                                   const CertificateSigner& signer) {
  if (!signer) {
    return make_error(ErrorCode::kInvalidArgument, "certificate signer is empty");
  }
  if (subject_public_key_info.empty()) {
    return make_error(ErrorCode::kInvalidArgument, "public key is empty");
  }
  auto subject = name_der(options.subject);
  if (!subject) return subject.error();
  CertificateName issuer_name = options.issuer;
  if (issuer_name.common_name.empty()) issuer_name = options.subject;
  auto issuer = name_der(issuer_name);
  if (!issuer) return issuer.error();

  const std::time_t not_before =
      options.not_before == 0 ? std::time(nullptr) : options.not_before;
  if (options.validity_seconds == 0) {
    return make_error(ErrorCode::kInvalidArgument, "certificate validity is zero");
  }
  const std::time_t not_after =
      not_before + static_cast<std::time_t>(options.validity_seconds);
  const auto signature_algorithm = algorithm_identifier();

  std::vector<Bytes> extensions;
  if (options.is_ca) {
    extensions.push_back(
        extension(kBasicConstraints, true, sequence({boolean_value(true)})));
    // digitalSignature (0) + keyCertSign (5) + cRLSign (6)
    extensions.push_back(
        extension(kKeyUsage, true, key_usage_value({0, 5, 6})));
  } else {
    // cA defaults to FALSE and DER forbids encoding a DEFAULT value, so a
    // leaf carries an empty sequence instead of BOOLEAN FALSE.
    extensions.push_back(extension(kBasicConstraints, true, sequence({})));
    extensions.push_back(extension(kKeyUsage, true, key_usage_value({0})));
    auto server_auth = der_object_identifier(kServerAuth);
    extensions.push_back(
        extension(kExtendedKeyUsage, false, sequence({server_auth.value()})));
  }
  if (!options.alt_names.empty()) {
    std::vector<Bytes> general_names;
    for (const auto& alt : options.alt_names) {
      if (!alt.dns.empty()) {
        general_names.push_back(wrap(0x82, Bytes(alt.dns.begin(), alt.dns.end())));
      }
      if (alt.ip.has_value() && (alt.ip->size() == 4 || alt.ip->size() == 16)) {
        general_names.push_back(wrap(0x87, alt.ip.value()));
      }
    }
    if (!general_names.empty()) {
      Bytes content;
      for (const auto& entry : general_names) {
        content.insert(content.end(), entry.begin(), entry.end());
      }
      extensions.push_back(
          extension(kSubjectAltName, false, wrap(0x30, content)));
    }
  }
  const Bytes skid = subject_key_id(subject_public_key_info);
  if (!skid.empty()) {
    extensions.push_back(extension(kSubjectKeyId, false, octet_string(skid)));
    Bytes akid = options.authority_key_id;
    if (akid.empty() && issuer.value() == subject.value()) akid = skid;
    if (!akid.empty()) {
      // [0] IMPLICIT KeyIdentifier
      extensions.push_back(
          extension(kAuthorityKeyId, false, sequence({wrap(0x80, akid)})));
    }
  }

  std::vector<Bytes> tbs_parts;
  tbs_parts.push_back(context_explicit(0, der_integer(2)));
  tbs_parts.push_back(der_integer(options.serial));
  tbs_parts.push_back(signature_algorithm);
  tbs_parts.push_back(issuer.value());
  tbs_parts.push_back(sequence({time_value(not_before), time_value(not_after)}));
  tbs_parts.push_back(subject.value());
  tbs_parts.push_back(subject_public_key_info);
  if (!extensions.empty()) {
    Bytes content;
    for (const auto& entry : extensions) {
      content.insert(content.end(), entry.begin(), entry.end());
    }
    tbs_parts.push_back(context_explicit(3, wrap(0x30, content)));
  }
  const Bytes tbs = sequence(tbs_parts);

  auto signature = signer(tbs);
  if (!signature) return signature.error();
  if (signature.value().empty()) {
    return make_error(ErrorCode::kInternal, "signer returned an empty signature");
  }
  return sequence({tbs, signature_algorithm, bit_string(signature.value())});
}

Bytes ec_key_identifier(const Bytes& uncompressed_point) {
  const auto digest =
      sha256(uncompressed_point.data(), uncompressed_point.size());
  return Bytes(digest.begin(), digest.begin() + 20);
}

Result<Bytes> ecdsa_signature_p1363_to_der(const Bytes& p1363) {
  if (p1363.empty() || (p1363.size() % 2) != 0) {
    return make_error(ErrorCode::kInvalidArgument,
                      "P1363 signature must be two equal-width components");
  }
  const std::size_t half = p1363.size() / 2;
  return sequence({der_unsigned_integer(p1363.data(), half),
                   der_unsigned_integer(p1363.data() + half, half)});
}

std::string pem_encode(const Bytes& der, std::string_view label) {
  const std::string encoded = base64_encode(der);
  std::string output = "-----BEGIN " + std::string(label) + "-----\n";
  for (std::size_t offset = 0; offset < encoded.size(); offset += 64) {
    output += encoded.substr(offset, 64);
    output += "\n";
  }
  output += "-----END " + std::string(label) + "-----\n";
  return output;
}

Result<DerElement> der_read_element(const Bytes& data, std::size_t& offset) {
  if (offset + 2 > data.size()) {
    return make_error(ErrorCode::kParse, "truncated DER element");
  }
  DerElement element;
  element.tag = data[offset];
  std::size_t cursor = offset + 1;
  std::size_t length = data[cursor++];
  if ((length & 0x80) != 0) {
    const std::size_t count = length & 0x7f;
    if (count == 0 || count > 4 || cursor + count > data.size()) {
      return make_error(ErrorCode::kParse, "unsupported DER length");
    }
    length = 0;
    for (std::size_t index = 0; index < count; ++index) {
      length = (length << 8) | data[cursor++];
    }
  }
  if (cursor + length > data.size()) {
    return make_error(ErrorCode::kParse, "DER element overruns the buffer");
  }
  element.header.assign(data.begin() + static_cast<std::ptrdiff_t>(offset),
                        data.begin() + static_cast<std::ptrdiff_t>(cursor));
  element.content.assign(data.begin() + static_cast<std::ptrdiff_t>(cursor),
                         data.begin() +
                             static_cast<std::ptrdiff_t>(cursor + length));
  offset = cursor + length;
  return element;
}

Result<Bytes> ecdsa_signature_to_der(const Bytes& signature) {
  // A P-256 P1363 signature is exactly 64 bytes while its DER form runs
  // 70-72 bytes for full-width components, so the 64-byte shape is
  // checked last: anything else that parses as a DER ECDSA-Sig-Value
  // (SEQUENCE of exactly two INTEGERs consuming the whole buffer) is
  // already DER and passes through untouched.
  if (signature.size() != 64 && !signature.empty()) {
    std::size_t offset = 0;
    auto outer = der_read_element(signature, offset);
    if (outer && outer.value().tag == 0x30 && offset == signature.size()) {
      const Bytes& content = outer.value().content;
      std::size_t inner = 0;
      auto first = der_read_element(content, inner);
      auto second = first ? der_read_element(content, inner) : first;
      if (second && first.value().tag == 0x02 &&
          second.value().tag == 0x02 && !first.value().content.empty() &&
          !second.value().content.empty() && inner == content.size()) {
        return signature;
      }
    }
  }
  return ecdsa_signature_p1363_to_der(signature);
}

}  // namespace ipashelf
