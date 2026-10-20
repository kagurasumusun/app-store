// SPDX-License-Identifier: MIT

#include "ipashelf/crypto.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <random>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <stdlib.h>
#endif

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

Sha1::Sha1()
    : state_{0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u, 0xc3d2e1f0u} {}

void Sha1::transform(const std::uint8_t block[64]) {
  std::array<std::uint32_t, 80> words{};
  for (std::size_t index = 0; index < 16; ++index) {
    words[index] = (static_cast<std::uint32_t>(block[index * 4]) << 24) |
                   (static_cast<std::uint32_t>(block[index * 4 + 1]) << 16) |
                   (static_cast<std::uint32_t>(block[index * 4 + 2]) << 8) |
                   static_cast<std::uint32_t>(block[index * 4 + 3]);
  }
  for (std::size_t index = 16; index < 80; ++index) {
    words[index] = rotate_left(words[index - 3] ^ words[index - 8] ^
                                   words[index - 14] ^ words[index - 16],
                               1);
  }
  std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3],
                e = state_[4];
  for (std::size_t index = 0; index < 80; ++index) {
    std::uint32_t f = 0;
    std::uint32_t k = 0;
    if (index < 20) {
      f = (b & c) | (~b & d);
      k = 0x5a827999u;
    } else if (index < 40) {
      f = b ^ c ^ d;
      k = 0x6ed9eba1u;
    } else if (index < 60) {
      f = (b & c) | (b & d) | (c & d);
      k = 0x8f1bbcdcu;
    } else {
      f = b ^ c ^ d;
      k = 0xca62c1d6u;
    }
    const std::uint32_t next =
        rotate_left(a, 5) + f + e + k + words[index];
    e = d;
    d = c;
    c = rotate_left(b, 30);
    b = a;
    a = next;
  }
  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
}

void Sha1::update(const void* data, std::size_t size) {
  if (finished_ || data == nullptr || size == 0) return;
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  total_bytes_ += size;
  while (size > 0) {
    const std::size_t take = std::min(size, buffer_.size() - buffer_size_);
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

std::array<std::uint8_t, 20> Sha1::finish() {
  if (!finished_) {
    finished_ = true;
    const std::uint64_t bit_length = total_bytes_ * 8;
    buffer_[buffer_size_++] = 0x80;
    if (buffer_size_ > 56) {
      while (buffer_size_ < buffer_.size()) buffer_[buffer_size_++] = 0;
      transform(buffer_.data());
      buffer_size_ = 0;
    }
    while (buffer_size_ < 56) buffer_[buffer_size_++] = 0;
    for (int index = 7; index >= 0; --index) {
      buffer_[buffer_size_++] =
          static_cast<std::uint8_t>((bit_length >> (index * 8)) & 0xff);
    }
    transform(buffer_.data());
    buffer_size_ = 0;
  }
  std::array<std::uint8_t, 20> digest{};
  for (std::size_t index = 0; index < state_.size(); ++index) {
    digest[index * 4] = static_cast<std::uint8_t>((state_[index] >> 24) & 0xff);
    digest[index * 4 + 1] =
        static_cast<std::uint8_t>((state_[index] >> 16) & 0xff);
    digest[index * 4 + 2] =
        static_cast<std::uint8_t>((state_[index] >> 8) & 0xff);
    digest[index * 4 + 3] = static_cast<std::uint8_t>(state_[index] & 0xff);
  }
  return digest;
}

std::array<std::uint8_t, 20> sha1(const void* data, std::size_t size) {
  Sha1 hasher;
  hasher.update(data, size);
  return hasher.finish();
}

std::string sha1_hex(const void* data, std::size_t size) {
  const auto digest = sha1(data, size);
  Bytes bytes(digest.begin(), digest.end());
  return hex_encode(bytes);
}

std::array<std::uint8_t, 20> hmac_sha1(const Bytes& key, const void* data,
                                        std::size_t size) {
  std::array<std::uint8_t, 64> key_block{};
  if (key.size() > key_block.size()) {
    const auto hashed = sha1(key.data(), key.size());
    std::copy(hashed.begin(), hashed.end(), key_block.begin());
  } else {
    std::copy(key.begin(), key.end(), key_block.begin());
  }
  std::array<std::uint8_t, 64> inner_pad{};
  std::array<std::uint8_t, 64> outer_pad{};
  for (std::size_t index = 0; index < key_block.size(); ++index) {
    inner_pad[index] = static_cast<std::uint8_t>(key_block[index] ^ 0x36);
    outer_pad[index] = static_cast<std::uint8_t>(key_block[index] ^ 0x5c);
  }
  Sha1 inner;
  inner.update(inner_pad.data(), inner_pad.size());
  inner.update(data, size);
  const auto inner_digest = inner.finish();
  Sha1 outer;
  outer.update(outer_pad.data(), outer_pad.size());
  outer.update(inner_digest.data(), inner_digest.size());
  return outer.finish();
}

Result<Bytes> random_bytes(std::size_t size) {
  Bytes output(size);
  if (size == 0) return output;
#if defined(__APPLE__)
  arc4random_buf(output.data(), output.size());
  return output;
#else
  if (FILE* file = std::fopen("/dev/urandom", "rb")) {
    const std::size_t read = std::fread(output.data(), 1, output.size(), file);
    std::fclose(file);
    if (read == output.size()) return output;
  }
  try {
    std::random_device device;
    for (std::size_t index = 0; index < output.size(); index += 4) {
      const std::uint32_t word = device();
      const std::size_t take = std::min<std::size_t>(4, output.size() - index);
      std::memcpy(output.data() + index, &word, take);
    }
    return output;
  } catch (...) {
    return make_error(ErrorCode::kInternal,
                      "no operating system random source is available");
  }
#endif
}

namespace {

std::uint8_t gf_multiply(std::uint8_t a, std::uint8_t b) {
  std::uint8_t result = 0;
  while (b != 0) {
    if ((b & 1) != 0) result = static_cast<std::uint8_t>(result ^ a);
    const bool carry = (a & 0x80) != 0;
    a = static_cast<std::uint8_t>(a << 1);
    if (carry) a = static_cast<std::uint8_t>(a ^ 0x1b);
    b = static_cast<std::uint8_t>(b >> 1);
  }
  return result;
}

std::uint8_t gf_inverse(std::uint8_t value) {
  if (value == 0) return 0;
  std::uint8_t result = 1;
  std::uint8_t base = value;
  unsigned exponent = 254;
  while (exponent != 0) {
    if ((exponent & 1) != 0) result = gf_multiply(result, base);
    base = gf_multiply(base, base);
    exponent >>= 1;
  }
  return result;
}

std::uint8_t aes_sbox(std::uint8_t value) {
  const std::uint8_t inverse = gf_inverse(value);
  std::uint8_t result = inverse;
  std::uint8_t rotated = inverse;
  for (int index = 0; index < 4; ++index) {
    rotated = static_cast<std::uint8_t>((rotated << 1) | (rotated >> 7));
    result = static_cast<std::uint8_t>(result ^ rotated);
  }
  return static_cast<std::uint8_t>(result ^ 0x63);
}

std::uint8_t aes_inverse_sbox(std::uint8_t value) {
  static const std::array<std::uint8_t, 256> kInverse = [] {
    std::array<std::uint8_t, 256> table{};
    for (int index = 0; index < 256; ++index) {
      table[aes_sbox(static_cast<std::uint8_t>(index))] =
          static_cast<std::uint8_t>(index);
    }
    return table;
  }();
  return kInverse[value];
}

void aes_add_round_key(std::uint8_t state[16], const std::uint8_t* round_key) {
  for (int index = 0; index < 16; ++index) {
    state[index] = static_cast<std::uint8_t>(state[index] ^ round_key[index]);
  }
}

void aes_shift_rows(std::uint8_t state[16], bool inverse) {
  std::uint8_t copy[16];
  std::memcpy(copy, state, 16);
  for (int row = 0; row < 4; ++row) {
    for (int column = 0; column < 4; ++column) {
      const int shift = inverse ? (4 - row) % 4 : row;
      state[column * 4 + row] = copy[((column + shift) % 4) * 4 + row];
    }
  }
}

void aes_mix_columns(std::uint8_t state[16], bool inverse) {
  for (int column = 0; column < 4; ++column) {
    std::uint8_t* cell = state + column * 4;
    const std::uint8_t a0 = cell[0], a1 = cell[1], a2 = cell[2], a3 = cell[3];
    if (!inverse) {
      cell[0] = gf_multiply(a0, 2) ^ gf_multiply(a1, 3) ^ a2 ^ a3;
      cell[1] = a0 ^ gf_multiply(a1, 2) ^ gf_multiply(a2, 3) ^ a3;
      cell[2] = a0 ^ a1 ^ gf_multiply(a2, 2) ^ gf_multiply(a3, 3);
      cell[3] = gf_multiply(a0, 3) ^ a1 ^ a2 ^ gf_multiply(a3, 2);
    } else {
      cell[0] = gf_multiply(a0, 14) ^ gf_multiply(a1, 11) ^
                gf_multiply(a2, 13) ^ gf_multiply(a3, 9);
      cell[1] = gf_multiply(a0, 9) ^ gf_multiply(a1, 14) ^
                gf_multiply(a2, 11) ^ gf_multiply(a3, 13);
      cell[2] = gf_multiply(a0, 13) ^ gf_multiply(a1, 9) ^
                gf_multiply(a2, 14) ^ gf_multiply(a3, 11);
      cell[3] = gf_multiply(a0, 11) ^ gf_multiply(a1, 13) ^
                gf_multiply(a2, 9) ^ gf_multiply(a3, 14);
    }
  }
}

// Minimal big-unsigned arithmetic for the one public-key operation the SAP
// setup handshake needs. Limbs are little-endian base 2^32.
using BigUint = std::vector<std::uint32_t>;

void big_trim(BigUint& value) {
  while (value.size() > 1 && value.back() == 0) value.pop_back();
}

int big_compare(const BigUint& left, const BigUint& right) {
  std::size_t left_size = left.size();
  std::size_t right_size = right.size();
  while (left_size > 1 && left[left_size - 1] == 0) --left_size;
  while (right_size > 1 && right[right_size - 1] == 0) --right_size;
  if (left_size != right_size) return left_size < right_size ? -1 : 1;
  for (std::size_t index = left_size; index-- > 0;) {
    if (left[index] != right[index]) return left[index] < right[index] ? -1 : 1;
  }
  return 0;
}

// Requires left >= right.
void big_subtract_in_place(BigUint& left, const BigUint& right) {
  std::uint64_t borrow = 0;
  for (std::size_t index = 0; index < left.size(); ++index) {
    const std::uint64_t subtrahend =
        (index < right.size() ? right[index] : 0) + borrow;
    const std::uint64_t current = left[index];
    left[index] =
        static_cast<std::uint32_t>((current - subtrahend) & 0xffffffffu);
    borrow = current < subtrahend ? 1 : 0;
  }
  big_trim(left);
}

BigUint big_multiply(const BigUint& left, const BigUint& right) {
  BigUint result(left.size() + right.size(), 0);
  for (std::size_t i = 0; i < left.size(); ++i) {
    std::uint64_t carry = 0;
    for (std::size_t j = 0; j < right.size(); ++j) {
      const std::uint64_t total =
          static_cast<std::uint64_t>(left[i]) * right[j] + result[i + j] +
          carry;
      result[i + j] = static_cast<std::uint32_t>(total & 0xffffffffu);
      carry = total >> 32;
    }
    result[i + right.size()] += static_cast<std::uint32_t>(carry);
  }
  big_trim(result);
  return result;
}

std::size_t big_bit_length(const BigUint& value) {
  std::size_t size = value.size();
  while (size > 1 && value[size - 1] == 0) --size;
  std::uint32_t top = value[size - 1];
  std::size_t bits = (size - 1) * 32;
  while (top != 0) {
    ++bits;
    top >>= 1;
  }
  return bits;
}

bool big_bit(const BigUint& value, std::size_t bit) {
  const std::size_t limb = bit / 32;
  if (limb >= value.size()) return false;
  return ((value[limb] >> (bit % 32)) & 1u) != 0;
}

void big_shift_left_one(BigUint& value) {
  std::uint32_t carry = 0;
  for (std::uint32_t& limb : value) {
    const std::uint32_t next_carry = limb >> 31;
    limb = (limb << 1) | carry;
    carry = next_carry;
  }
  if (carry != 0) value.push_back(carry);
}

BigUint big_mod(BigUint value, const BigUint& modulus) {
  if (big_compare(value, modulus) < 0) return value;
  BigUint remainder(1, 0);
  const std::size_t bits = big_bit_length(value);
  for (std::size_t index = bits; index-- > 0;) {
    big_shift_left_one(remainder);
    if (big_bit(value, index)) remainder[0] |= 1u;
    if (big_compare(remainder, modulus) >= 0) {
      big_subtract_in_place(remainder, modulus);
    }
  }
  return remainder;
}

BigUint big_mod_multiply(const BigUint& left, const BigUint& right,
                         const BigUint& modulus) {
  return big_mod(big_multiply(left, right), modulus);
}

BigUint big_mod_power(BigUint base, const BigUint& exponent,
                      const BigUint& modulus) {
  BigUint result(1, 1);
  base = big_mod(base, modulus);
  const std::size_t bits = big_bit_length(exponent);
  for (std::size_t index = bits; index-- > 0;) {
    result = big_mod_multiply(result, result, modulus);
    if (big_bit(exponent, index)) {
      result = big_mod_multiply(result, base, modulus);
    }
  }
  return result;
}

BigUint big_from_big_endian(const Bytes& bytes) {
  BigUint value((bytes.size() + 3) / 4, 0);
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    const std::size_t from_end = bytes.size() - 1 - index;
    value[from_end / 4] |=
        static_cast<std::uint32_t>(bytes[index]) << ((from_end % 4) * 8);
  }
  if (value.empty()) value.push_back(0);
  big_trim(value);
  return value;
}

Bytes big_to_big_endian(const BigUint& value, std::size_t size) {
  Bytes output(size, 0);
  for (std::size_t index = 0; index < size; ++index) {
    const std::size_t from_end = size - 1 - index;
    const std::size_t limb = from_end / 4;
    if (limb < value.size()) {
      output[index] = static_cast<std::uint8_t>(
          (value[limb] >> ((from_end % 4) * 8)) & 0xff);
    }
  }
  return output;
}

Bytes mgf1_sha1(const Bytes& seed, std::size_t size) {
  Bytes output;
  output.reserve(size);
  for (std::uint32_t counter = 0; output.size() < size; ++counter) {
    Bytes block = seed;
    block.push_back(static_cast<std::uint8_t>((counter >> 24) & 0xff));
    block.push_back(static_cast<std::uint8_t>((counter >> 16) & 0xff));
    block.push_back(static_cast<std::uint8_t>((counter >> 8) & 0xff));
    block.push_back(static_cast<std::uint8_t>(counter & 0xff));
    const auto digest = sha1(block.data(), block.size());
    output.insert(output.end(), digest.begin(), digest.end());
  }
  output.resize(size);
  return output;
}

}  // namespace

Aes128::Aes128(const std::array<std::uint8_t, 16>& key) {
  static constexpr std::uint8_t kRoundConstants[10] = {
      0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36};
  std::copy(key.begin(), key.end(), round_keys_.begin());
  std::size_t generated = 16;
  std::size_t round = 0;
  while (generated < round_keys_.size()) {
    std::uint8_t temp[4];
    std::memcpy(temp, round_keys_.data() + generated - 4, 4);
    if (generated % 16 == 0) {
      const std::uint8_t first = temp[0];
      temp[0] = static_cast<std::uint8_t>(aes_sbox(temp[1]) ^
                                           kRoundConstants[round++]);
      temp[1] = aes_sbox(temp[2]);
      temp[2] = aes_sbox(temp[3]);
      temp[3] = aes_sbox(first);
    }
    for (std::uint8_t byte : temp) {
      round_keys_[generated] = static_cast<std::uint8_t>(
          round_keys_[generated - 16] ^ byte);
      ++generated;
    }
  }
}

void Aes128::encrypt_block(const std::uint8_t input[16],
                           std::uint8_t output[16]) const {
  std::uint8_t state[16];
  std::memcpy(state, input, 16);
  aes_add_round_key(state, round_keys_.data());
  for (int round = 1; round < 10; ++round) {
    for (std::uint8_t& byte : state) byte = aes_sbox(byte);
    aes_shift_rows(state, false);
    aes_mix_columns(state, false);
    aes_add_round_key(state, round_keys_.data() + round * 16);
  }
  for (std::uint8_t& byte : state) byte = aes_sbox(byte);
  aes_shift_rows(state, false);
  aes_add_round_key(state, round_keys_.data() + 160);
  std::memcpy(output, state, 16);
}

void Aes128::decrypt_block(const std::uint8_t input[16],
                           std::uint8_t output[16]) const {
  std::uint8_t state[16];
  std::memcpy(state, input, 16);
  aes_add_round_key(state, round_keys_.data() + 160);
  for (int round = 9; round >= 1; --round) {
    aes_shift_rows(state, true);
    for (std::uint8_t& byte : state) byte = aes_inverse_sbox(byte);
    aes_add_round_key(state, round_keys_.data() + round * 16);
    aes_mix_columns(state, true);
  }
  aes_shift_rows(state, true);
  for (std::uint8_t& byte : state) byte = aes_inverse_sbox(byte);
  aes_add_round_key(state, round_keys_.data());
  std::memcpy(output, state, 16);
}

Result<Bytes> aes128_cbc_encrypt(const Bytes& data,
                                 const std::array<std::uint8_t, 16>& key,
                                 const std::array<std::uint8_t, 16>& iv) {
  if (data.size() % 16 != 0) {
    return make_error(ErrorCode::kInvalidArgument,
                      "AES-CBC input must be a multiple of sixteen bytes");
  }
  const Aes128 cipher(key);
  Bytes output(data.size());
  std::array<std::uint8_t, 16> previous = iv;
  for (std::size_t offset = 0; offset < data.size(); offset += 16) {
    std::uint8_t block[16];
    for (int index = 0; index < 16; ++index) {
      block[index] =
          static_cast<std::uint8_t>(data[offset + index] ^ previous[index]);
    }
    cipher.encrypt_block(block, output.data() + offset);
    std::memcpy(previous.data(), output.data() + offset, 16);
  }
  return output;
}

Result<Bytes> aes128_cbc_decrypt(const Bytes& data,
                                 const std::array<std::uint8_t, 16>& key,
                                 const std::array<std::uint8_t, 16>& iv) {
  if (data.size() % 16 != 0) {
    return make_error(ErrorCode::kInvalidArgument,
                      "AES-CBC input must be a multiple of sixteen bytes");
  }
  const Aes128 cipher(key);
  Bytes output(data.size());
  std::array<std::uint8_t, 16> previous = iv;
  for (std::size_t offset = 0; offset < data.size(); offset += 16) {
    std::array<std::uint8_t, 16> current{};
    std::memcpy(current.data(), data.data() + offset, 16);
    std::uint8_t block[16];
    cipher.decrypt_block(current.data(), block);
    for (int index = 0; index < 16; ++index) {
      output[offset + index] =
          static_cast<std::uint8_t>(block[index] ^ previous[index]);
    }
    previous = current;
  }
  return output;
}

Result<Bytes> rsa_oaep_sha1_encrypt(const Bytes& message,
                                    const std::array<std::uint8_t, 20>& seed,
                                    const Bytes& modulus,
                                    const Bytes& exponent) {
  const std::size_t modulus_size = modulus.size();
  if (modulus_size <= 42 || message.size() > modulus_size - 42) {
    return make_error(ErrorCode::kInvalidArgument,
                      "message is too long for RSA-OAEP with SHA-1");
  }
  const BigUint modulus_value = big_from_big_endian(modulus);
  const BigUint exponent_value = big_from_big_endian(exponent);
  if (big_bit_length(modulus_value) < (modulus_size - 1) * 8 + 1 ||
      big_bit_length(exponent_value) < 2 ||
      (exponent_value[0] & 1u) == 0) {
    return make_error(ErrorCode::kInvalidArgument,
                      "invalid RSA public key for OAEP");
  }
  Bytes data_block;
  const auto label_hash = sha1(nullptr, 0);
  data_block.insert(data_block.end(), label_hash.begin(), label_hash.end());
  data_block.insert(data_block.end(),
                    modulus_size - message.size() - 42, 0);
  data_block.push_back(1);
  data_block.insert(data_block.end(), message.begin(), message.end());
  const Bytes seed_bytes(seed.begin(), seed.end());
  const Bytes database_mask = mgf1_sha1(seed_bytes, modulus_size - 21);
  Bytes masked_database(data_block.size());
  for (std::size_t index = 0; index < data_block.size(); ++index) {
    masked_database[index] =
        static_cast<std::uint8_t>(data_block[index] ^ database_mask[index]);
  }
  const Bytes seed_mask = mgf1_sha1(masked_database, seed.size());
  Bytes encoded;
  encoded.reserve(modulus_size);
  encoded.push_back(0);
  for (std::size_t index = 0; index < seed.size(); ++index) {
    encoded.push_back(
        static_cast<std::uint8_t>(seed[index] ^ seed_mask[index]));
  }
  encoded.insert(encoded.end(), masked_database.begin(),
                 masked_database.end());
  const BigUint encoded_value = big_from_big_endian(encoded);
  if (big_compare(encoded_value, modulus_value) >= 0) {
    return make_error(ErrorCode::kInvalidArgument,
                      "RSA-OAEP representative is out of range");
  }
  const BigUint result =
      big_mod_power(encoded_value, exponent_value, modulus_value);
  return big_to_big_endian(result, modulus_size);
}

}  // namespace ipashelf
