// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "ipashelf/base.hpp"

namespace ipashelf {

[[nodiscard]] std::string base64_encode(const Bytes& data);
[[nodiscard]] Result<Bytes> base64_decode(std::string_view text);

class Md5 {
 public:
  Md5();
  void update(const void* data, std::size_t size);
  [[nodiscard]] std::array<std::uint8_t, 16> finish();

 private:
  void transform(const std::uint8_t block[64]);

  std::array<std::uint32_t, 4> state_{};
  std::array<std::uint8_t, 64> buffer_{};
  std::uint64_t total_bytes_ = 0;
  std::size_t buffer_size_ = 0;
  bool finished_ = false;
};

[[nodiscard]] std::array<std::uint8_t, 16> md5(const void* data, std::size_t size);
[[nodiscard]] std::string md5_hex(const void* data, std::size_t size);

class Sha256 {
 public:
  Sha256();
  void update(const void* data, std::size_t size);
  [[nodiscard]] std::array<std::uint8_t, 32> finish();

 private:
  void transform(const std::uint8_t block[64]);

  std::array<std::uint32_t, 8> state_{};
  std::array<std::uint8_t, 64> buffer_{};
  std::uint64_t total_bytes_ = 0;
  std::size_t buffer_size_ = 0;
  bool finished_ = false;
};

[[nodiscard]] std::array<std::uint8_t, 32> sha256(const void* data, std::size_t size);
[[nodiscard]] std::string sha256_hex(const void* data, std::size_t size);

class Sha1 {
 public:
  Sha1();
  void update(const void* data, std::size_t size);
  [[nodiscard]] std::array<std::uint8_t, 20> finish();

 private:
  void transform(const std::uint8_t block[64]);

  std::array<std::uint32_t, 5> state_{};
  std::array<std::uint8_t, 64> buffer_{};
  std::uint64_t total_bytes_ = 0;
  std::size_t buffer_size_ = 0;
  bool finished_ = false;
};

[[nodiscard]] std::array<std::uint8_t, 20> sha1(const void* data, std::size_t size);
[[nodiscard]] std::string sha1_hex(const void* data, std::size_t size);

/// HMAC-SHA1 (RFC 2104) over a single contiguous message.
[[nodiscard]] std::array<std::uint8_t, 20> hmac_sha1(const Bytes& key,
                                                     const void* data,
                                                     std::size_t size);

/// Fills `size` bytes from the operating system CSPRNG.
[[nodiscard]] Result<Bytes> random_bytes(std::size_t size);

/// AES-128 (FIPS-197). The CBC helpers apply no padding and therefore require
/// input lengths that are multiples of sixteen bytes.
class Aes128 {
 public:
  explicit Aes128(const std::array<std::uint8_t, 16>& key);
  void encrypt_block(const std::uint8_t input[16], std::uint8_t output[16]) const;
  void decrypt_block(const std::uint8_t input[16], std::uint8_t output[16]) const;

 private:
  std::array<std::uint8_t, 176> round_keys_{};
};

[[nodiscard]] Result<Bytes> aes128_cbc_encrypt(
    const Bytes& data, const std::array<std::uint8_t, 16>& key,
    const std::array<std::uint8_t, 16>& iv);
[[nodiscard]] Result<Bytes> aes128_cbc_decrypt(
    const Bytes& data, const std::array<std::uint8_t, 16>& key,
    const std::array<std::uint8_t, 16>& iv);

/// RSAES-OAEP encryption with SHA-1 / MGF1-SHA1 and an empty label
/// (RFC 8017 section 7.1.1), using a caller-supplied twenty-byte seed so the
/// operation is deterministic for protocols that derive the seed themselves.
/// `modulus` and `exponent` are big-endian magnitudes.
[[nodiscard]] Result<Bytes> rsa_oaep_sha1_encrypt(
    const Bytes& message, const std::array<std::uint8_t, 20>& seed,
    const Bytes& modulus, const Bytes& exponent);

}  // namespace ipashelf
