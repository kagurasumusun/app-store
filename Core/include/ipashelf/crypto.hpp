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

}  // namespace ipashelf
