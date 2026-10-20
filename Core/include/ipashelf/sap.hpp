// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "ipashelf/base.hpp"
#include "ipashelf/http.hpp"
#include "ipashelf/models.hpp"

namespace ipashelf {

/// Low-level access to Apple's SAP primitives. The production implementation
/// resolves these entry points from the system CoreFP/AppleMediaServices
/// frameworks; tests inject a deterministic fake.
class SapBackend {
 public:
  virtual ~SapBackend() = default;

  virtual Result<std::uint64_t> initialize(const Bytes& hardware_block) = 0;
  virtual Result<std::pair<Bytes, std::int32_t>> exchange(
      std::uint32_t version, const Bytes& hardware_block,
      std::uint64_t context, const Bytes& input) = 0;
  virtual Result<Bytes> sign(std::uint64_t context, const Bytes& input) = 0;
  virtual Status teardown(std::uint64_t context) = 0;
};

struct SapConfiguration {
  std::string setup_url;
  std::string certificate_url;
  std::uint32_t version = 200;
  HardwareIdentity hardware;
};

class SapSigner {
 public:
  virtual ~SapSigner() = default;
  virtual Result<Bytes> sign(const Bytes& data) = 0;
};

/// Runs Apple's two-step SAP setup exchange and keeps the resulting context
/// alive for signing sign-in and purchase-history requests.
class SapSession final : public SapSigner {
 public:
  SapSession(std::shared_ptr<SapBackend> backend, std::uint64_t context,
             Bytes hardware_block);
  ~SapSession() override;

  SapSession(const SapSession&) = delete;
  SapSession& operator=(const SapSession&) = delete;

  Result<Bytes> sign(const Bytes& data) override;
  Status close();

 private:
  std::shared_ptr<SapBackend> backend_;
  std::uint64_t context_ = 0;
  Bytes hardware_block_;
  std::mutex mutex_;
  bool closed_ = false;
};

/// Performs the setup handshake. `user_agent` is used for both Apple requests.
Result<std::unique_ptr<SapSession>> establish_sap_session(
    HttpTransport& transport, const SapConfiguration& configuration,
    std::string_view user_agent, const CancellationToken& token,
    std::shared_ptr<SapBackend> backend);

/// Builds the plist envelope used by `sign-sap-setup`.
[[nodiscard]] Bytes sap_setup_envelope(const Bytes& payload);

/// Extracts a data value from a plist received during the SAP handshake.
[[nodiscard]] Result<Bytes> sap_plist_data(const Bytes& body,
                                           std::string_view key);

/// Platform factory. iOS returns the CoreFP-backed implementation; the
/// portable test build returns an error unless a fake is injected.
Result<std::shared_ptr<SapBackend>> create_platform_sap_backend();

}  // namespace ipashelf
