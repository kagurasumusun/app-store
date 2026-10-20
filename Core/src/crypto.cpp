// SPDX-License-Identifier: MIT

#include "ipashelf/crypto.hpp"

#include <cstring>
#include <utility>

namespace ipashelf {
namespace {

constexpr char kBase64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int base64_value(char character) {
  if (character >= 'A' && character <= 'Z') return character - 'A';
  if (character >= 'a' && character <= 'z') return character - 'a' + 26;
  if (character >= '0' && character <= '9') return character - '0' + 52;
  if (character == '+') return 62;
  if (character == '/') return 63;
  return -1;
}

std::uint32_t rotate_left(std::uint32_t value, std::uint32_t count) {
  return (value << count) | (value >> (32 - count));
}

std::uint32_t rotate_right(std::uint32_t value, std::uint32_t count) {
  return (value >> count) | (value << (32 - count));
}

constexpr std::array<std::uint32_t, 64> kSha256RoundConstants = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
    0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
    0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
    0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
    0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
    0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
    0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
    0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

}  // namespace

std::string base64_encode(const Bytes& data) {
  std::string result;
  result.reserve(((data.size() + 2) / 3) * 4);
  std::size_t index = 0;
  while (index + 2 < data.size()) {
    const std::uint32_t value = (static_cast<std::uint32_t>(data[index]) << 16) |
                                (static_cast<std::uint32_t>(data[index + 1]) << 8) |
                                static_cast<std::uint32_t>(data[index + 2]);
    result.push_back(kBase64Alphabet[(value >> 18) & 0x3f]);
    result.push_back(kBase64Alphabet[(value >> 12) & 0x3f]);
    result.push_back(kBase64Alphabet[(value >> 6) & 0x3f]);
    result.push_back(kBase64Alphabet[value & 0x3f]);
    index += 3;
  }
  const std::size_t remaining = data.size() - index;
  if (remaining == 1) {
    const std::uint32_t value = static_cast<std::uint32_t>(data[index]) << 16;
    result.push_back(kBase64Alphabet[(value >> 18) & 0x3f]);
    result.push_back(kBase64Alphabet[(value >> 12) & 0x3f]);
    result.append("==");
  } else if (remaining == 2) {
    const std::uint32_t value = (static_cast<std::uint32_t>(data[index]) << 16) |
                                (static_cast<std::uint32_t>(data[index + 1]) << 8);
    result.push_back(kBase64Alphabet[(value >> 18) & 0x3f]);
    result.push_back(kBase64Alphabet[(value >> 12) & 0x3f]);
    result.push_back(kBase64Alphabet[(value >> 6) & 0x3f]);
    result.push_back('=');
  }
  return result;
}

Result<Bytes> base64_decode(std::string_view text) {
  Bytes result;
  result.reserve((text.size() / 4) * 3);
  std::uint32_t accumulator = 0;
  int bits = 0;
  std::size_t padding = 0;

  for (char character : text) {
    if (character == ' ' || character == '\n' || character == '\r' ||
        character == '\t') {
      continue;
    }
    if (character == '=') {
      ++padding;
      continue;
    }
    if (padding != 0 || padding > 2) {
      return make_error(ErrorCode::kParse, "base64 data has invalid padding");
    }
    const int value = base64_value(character);
    if (value < 0) {
      return make_error(ErrorCode::kParse, "base64 data contains an invalid character");
    }
    accumulator = (accumulator << 6) | static_cast<std::uint32_t>(value);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      result.push_back(static_cast<std::uint8_t>((accumulator >> bits) & 0xff));
    }
  }

  if (padding > 2) {
    return make_error(ErrorCode::kParse, "base64 data has too much padding");
  }
  if (bits >= 6) {
    return make_error(ErrorCode::kParse, "base64 data has a truncated quantum");
  }
  return result;
}

// ---------------------------------------------------------------------------
// MD5 (RFC 1321)
// ---------------------------------------------------------------------------

Md5::Md5() {
  state_[0] = 0x67452301u;
  state_[1] = 0xefcdab89u;
  state_[2] = 0x98badcfeu;
  state_[3] = 0x10325476u;
}

void Md5::transform(const std::uint8_t block[64]) {
  static constexpr std::uint32_t kShift[64] = {
      7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
      5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
      4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
      6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
  static constexpr std::uint32_t kTable[64] = {
      0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu,
      0x4787c62au, 0xa8304613u, 0xfd469501u, 0x698098d8u, 0x8b44f7afu,
      0xffff5bb1u, 0x895cd7beu, 0x6b901122u, 0xfd987193u, 0xa679438eu,
      0x49b40821u, 0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau,
      0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u, 0x21e1cde6u,
      0xc33707d6u, 0xf4d50d87u, 0x455a14edu, 0xa9e3e905u, 0xfcefa3f8u,
      0x676f02d9u, 0x8d2a4c8au, 0xfffa3942u, 0x8771f681u, 0x6d9d6122u,
      0xfde5380cu, 0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
      0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u, 0xd9d4d039u,
      0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u, 0xf4292244u, 0x432aff97u,
      0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u, 0xffeff47du,
      0x85845dd1u, 0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
      0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u};

  std::uint32_t words[16];
  for (std::size_t index = 0; index < 16; ++index) {
    words[index] = static_cast<std::uint32_t>(block[index * 4]) |
                   (static_cast<std::uint32_t>(block[index * 4 + 1]) << 8) |
                   (static_cast<std::uint32_t>(block[index * 4 + 2]) << 16) |
                   (static_cast<std::uint32_t>(block[index * 4 + 3]) << 24);
  }

  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];

  for (std::uint32_t index = 0; index < 64; ++index) {
    std::uint32_t function = 0;
    std::uint32_t word_index = 0;
    if (index < 16) {
      function = (b & c) | (~b & d);
      word_index = index;
    } else if (index < 32) {
      function = (d & b) | (~d & c);
      word_index = (5 * index + 1) % 16;
    } else if (index < 48) {
      function = b ^ c ^ d;
      word_index = (3 * index + 5) % 16;
    } else {
      function = c ^ (b | ~d);
      word_index = (7 * index) % 16;
    }
    const std::uint32_t temp = d;
    d = c;
    c = b;
    b = b + rotate_left(a + function + kTable[index] + words[word_index],
                        kShift[index]);
    a = temp;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
}

void Md5::update(const void* data, std::size_t size) {
  if (finished_ || size == 0) return;
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  total_bytes_ += size;
  while (size > 0) {
    const std::size_t available = buffer_.size() - buffer_size_;
    const std::size_t take = size < available ? size : available;
    std::memcpy(buffer_.data() + buffer_size_, bytes, take);
    buffer_size_ += take;
    bytes += take;
    size -= take;
    if (buffer_size_ == buffer_.size()) {
      transform(buffer_.data());
      buffer_size_ = 0;
    }
  }
}

std::array<std::uint8_t, 16> Md5::finish() {
  if (!finished_) {
    const std::uint64_t bit_length = total_bytes_ * 8;
    std::uint8_t padding[72] = {0x80};
    const std::size_t padding_size =
        buffer_size_ < 56 ? 56 - buffer_size_ : 120 - buffer_size_;
    update(padding, padding_size);
    std::uint8_t length_bytes[8];
    for (std::size_t index = 0; index < 8; ++index) {
      length_bytes[index] =
          static_cast<std::uint8_t>((bit_length >> (8 * index)) & 0xff);
    }
    update(length_bytes, sizeof(length_bytes));
    finished_ = true;
  }
  std::array<std::uint8_t, 16> digest{};
  for (std::size_t index = 0; index < 4; ++index) {
    digest[index * 4] = static_cast<std::uint8_t>(state_[index] & 0xff);
    digest[index * 4 + 1] = static_cast<std::uint8_t>((state_[index] >> 8) & 0xff);
    digest[index * 4 + 2] = static_cast<std::uint8_t>((state_[index] >> 16) & 0xff);
    digest[index * 4 + 3] = static_cast<std::uint8_t>((state_[index] >> 24) & 0xff);
  }
  return digest;
}

std::array<std::uint8_t, 16> md5(const void* data, std::size_t size) {
  Md5 hasher;
  hasher.update(data, size);
  return hasher.finish();
}

std::string md5_hex(const void* data, std::size_t size) {
  const auto digest = md5(data, size);
  Bytes bytes(digest.begin(), digest.end());
  return hex_encode(bytes);
}

// ---------------------------------------------------------------------------
// SHA-256 (FIPS 180-4)
// ---------------------------------------------------------------------------

Sha256::Sha256() {
  state_ = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
            0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
}

void Sha256::transform(const std::uint8_t block[64]) {
  std::uint32_t words[64];
  for (std::size_t index = 0; index < 16; ++index) {
    words[index] = (static_cast<std::uint32_t>(block[index * 4]) << 24) |
                   (static_cast<std::uint32_t>(block[index * 4 + 1]) << 16) |
                   (static_cast<std::uint32_t>(block[index * 4 + 2]) << 8) |
                   static_cast<std::uint32_t>(block[index * 4 + 3]);
  }
  for (std::size_t index = 16; index < 64; ++index) {
    const std::uint32_t s0 = rotate_right(words[index - 15], 7) ^
                             rotate_right(words[index - 15], 18) ^
                             (words[index - 15] >> 3);
    const std::uint32_t s1 = rotate_right(words[index - 2], 17) ^
                             rotate_right(words[index - 2], 19) ^
                             (words[index - 2] >> 10);
    words[index] = words[index - 16] + s0 + words[index - 7] + s1;
  }

  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];

  for (std::size_t index = 0; index < 64; ++index) {
    const std::uint32_t s1 =
        rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
    const std::uint32_t choice = (e & f) ^ (~e & g);
    const std::uint32_t temp1 =
        h + s1 + choice + kSha256RoundConstants[index] + words[index];
    const std::uint32_t s0 =
        rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
    const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = s0 + majority;
    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256::update(const void* data, std::size_t size) {
  if (finished_ || size == 0) return;
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  total_bytes_ += size;
  while (size > 0) {
    const std::size_t available = buffer_.size() - buffer_size_;
    const std::size_t take = size < available ? size : available;
    std::memcpy(buffer_.data() + buffer_size_, bytes, take);
    buffer_size_ += take;
    bytes += take;
    size -= take;
    if (buffer_size_ == buffer_.size()) {
      transform(buffer_.data());
      buffer_size_ = 0;
    }
  }
}

std::array<std::uint8_t, 32> Sha256::finish() {
  if (!finished_) {
    const std::uint64_t bit_length = total_bytes_ * 8;
    std::uint8_t padding[72] = {0x80};
    const std::size_t padding_size =
        buffer_size_ < 56 ? 56 - buffer_size_ : 120 - buffer_size_;
    update(padding, padding_size);
    std::uint8_t length_bytes[8];
    for (std::size_t index = 0; index < 8; ++index) {
      length_bytes[7 - index] =
          static_cast<std::uint8_t>((bit_length >> (8 * index)) & 0xff);
    }
    update(length_bytes, sizeof(length_bytes));
    finished_ = true;
  }
  std::array<std::uint8_t, 32> digest{};
  for (std::size_t index = 0; index < 8; ++index) {
    digest[index * 4] = static_cast<std::uint8_t>((state_[index] >> 24) & 0xff);
    digest[index * 4 + 1] = static_cast<std::uint8_t>((state_[index] >> 16) & 0xff);
    digest[index * 4 + 2] = static_cast<std::uint8_t>((state_[index] >> 8) & 0xff);
    digest[index * 4 + 3] = static_cast<std::uint8_t>(state_[index] & 0xff);
  }
  return digest;
}

std::array<std::uint8_t, 32> sha256(const void* data, std::size_t size) {
  Sha256 hasher;
  hasher.update(data, size);
  return hasher.finish();
}

std::string sha256_hex(const void* data, std::size_t size) {
  const auto digest = sha256(data, size);
  Bytes bytes(digest.begin(), digest.end());
  return hex_encode(bytes);
}

}  // namespace ipashelf
