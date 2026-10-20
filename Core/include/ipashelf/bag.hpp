// SPDX-License-Identifier: MIT

#pragma once

#include <string>

#include "ipashelf/models.hpp"
#include "ipashelf/plist.hpp"

namespace ipashelf {

/// Endpoints discovered in Apple's URL bag. Every endpoint is validated before
/// it is used so a compromised or unexpected bag cannot redirect credentials
/// to a non-Apple host.
struct Bag {
  std::string auth_endpoint;
  std::string volume_store_endpoint;
  std::string redownload_endpoint;
  std::string update_endpoint;
  std::string ent_download_endpoint;
  std::string sap_setup_url;
  std::string sap_setup_cert_url;
  int sap_version = 0;
  PlistValue raw = PlistValue::dictionary();

  [[nodiscard]] static Result<Bag> parse(const PlistValue& document);
  [[nodiscard]] Status validate() const;
};

/// Builds the URL bag request for a protocol identity.
[[nodiscard]] std::string bag_request_url(const HardwareIdentity& identity);

/// Normalises a validated authentication endpoint so the legacy MZFinance path
/// ends in a slash, matching Apple's Store behaviour.
[[nodiscard]] Result<std::string> normalize_auth_endpoint(std::string_view endpoint);

}  // namespace ipashelf
