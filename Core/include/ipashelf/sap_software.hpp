// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <cstdint>
#include <memory>

#include "ipashelf/base.hpp"
#include "ipashelf/sap.hpp"

namespace ipashelf {

/// Pure-software reimplementation of Apple's FairPlay SAP signing protocol
/// (version 200) used to authenticate App Store requests.
///
/// The implementation performs the complete two-message setup handshake
/// (RSA-OAEP key block, AES-128-CBC payload, HMAC-SHA1) and the per-request
/// signing transform in portable C++: it needs no private Apple frameworks,
/// no emulator and no extracted system images, so it runs inside a regular
/// sandboxed iOS app. The algorithm's fixed substitution/routing constants
/// live in `sap_tables.hpp` and are generated from the documented data files
/// in `ThirdParty/sap-tables`.

/// Logical inputs of one signing session, produced by the setup handshake.
/// Treat as secret: it contains everything needed to sign requests.
struct SapSessionData {
  Bytes source;  // 576 bytes of cipher material.
  std::array<std::uint8_t, 16> f2_input1{};
  std::array<std::uint8_t, 16> f2_input2{};
  Bytes f2_tail;  // Up to sixteen bytes appended to the F2 head.
  Bytes f3;       // Server-issued signature record, carried verbatim.
  std::array<std::uint32_t, 624> random_words{};
  std::uint32_t random_cursor = 0;
  std::uint32_t seed30 = 0;
  std::uint32_t seed54 = 0;
  std::uint64_t f1_material_address = 0;
  std::uint64_t iv_address = 0x5000007FED90ULL;
};

/// The sixteen-byte block transform "E" at the heart of SAP signing,
/// keyed by the 576 bytes of session source material.
class SapSoftwareCipher {
 public:
  static Result<std::unique_ptr<SapSoftwareCipher>> create(const Bytes& source);

  [[nodiscard]] std::array<std::uint8_t, 16> encrypt(
      const std::array<std::uint8_t, 16>& block) const;

  SapSoftwareCipher(SapSoftwareCipher&&) noexcept;
  SapSoftwareCipher& operator=(SapSoftwareCipher&&) noexcept;
  ~SapSoftwareCipher();
  SapSoftwareCipher(const SapSoftwareCipher&) = delete;
  SapSoftwareCipher& operator=(const SapSoftwareCipher&) = delete;

 private:
  struct Impl;
  explicit SapSoftwareCipher(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

/// Derives the sixteen-byte F2 head from the two session inputs.
[[nodiscard]] Result<std::array<std::uint8_t, 16>> sap_software_f2_head(
    const std::array<std::uint8_t, 16>& input1,
    const std::array<std::uint8_t, 16>& input2);

/// Stateful signer over one established session. Signing advances a shared
/// pseudo-random stream, so calls are serialized internally.
class SapSoftwareSigner {
 public:
  static Result<std::unique_ptr<SapSoftwareSigner>> create(
      SapSessionData session, std::uint32_t platform = 1);

  Result<Bytes> sign(const Bytes& body);

  SapSoftwareSigner(SapSoftwareSigner&&) noexcept;
  SapSoftwareSigner& operator=(SapSoftwareSigner&&) noexcept;
  ~SapSoftwareSigner();
  SapSoftwareSigner(const SapSoftwareSigner&) = delete;
  SapSoftwareSigner& operator=(const SapSoftwareSigner&) = delete;

 private:
  struct Impl;
  explicit SapSoftwareSigner(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

/// Client side of the two-message setup handshake: builds the request sent
/// to Apple's `sign-sap-setup` endpoint and folds the reply into a session.
class SapSoftwareHandshake {
 public:
  /// Uses operating-system entropy and the current time.
  static Result<std::unique_ptr<SapSoftwareHandshake>> create(
      const Bytes& certificate_container);
  /// Deterministic variant used by known-answer tests.
  static Result<std::unique_ptr<SapSoftwareHandshake>> create_deterministic(
      const Bytes& certificate_container,
      const std::array<std::uint32_t, 3>& entropy, std::int64_t seconds,
      std::uint32_t microseconds);

  [[nodiscard]] const Bytes& request() const;
  [[nodiscard]] std::array<std::uint8_t, 16> response_key() const;
  [[nodiscard]] std::array<std::uint8_t, 16> response_echo() const;

  /// Validates and decrypts the setup reply and returns the signing session.
  Result<SapSessionData> complete(const Bytes& reply) const;

  SapSoftwareHandshake(SapSoftwareHandshake&&) noexcept;
  SapSoftwareHandshake& operator=(SapSoftwareHandshake&&) noexcept;
  ~SapSoftwareHandshake();
  SapSoftwareHandshake(const SapSoftwareHandshake&) = delete;
  SapSoftwareHandshake& operator=(const SapSoftwareHandshake&) = delete;

 private:
  struct Impl;
  explicit SapSoftwareHandshake(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

/// A `SapBackend` implementing the complete protocol in software. The
/// hardware block accepted by `initialize`/`exchange` is validated for shape
/// but does not enter the mathematics: the setup servers never see a device
/// identity, and the reconstructed algorithm derives its state solely from
/// entropy, time and the server reply.
[[nodiscard]] std::shared_ptr<SapBackend> create_software_sap_backend();

}  // namespace ipashelf
