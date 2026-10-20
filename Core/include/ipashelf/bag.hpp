// SPDX-License-Identifier: MIT

#pragma once

#include <string>

#include "ipashelf/models.hpp"
#include "ipashelf/plist.hpp"

namespace ipashelf {

/// Endpoints discovered in Apple's URL bag (`bag.xml`).
///
/// Parsing only extracts fields; each endpoint is validated where it is used
/// so a compromised or unexpected bag cannot redirect credentials to a
/// non-Apple host. Note the `volumeStoreDownloadProduct` bag key no longer
/// points at the legacy MZFinance volume store: current bags return the
/// DownloadDispatch `ent/download` URL there, and the legacy volume store
/// URL is reconstructed from the account's pod instead.
struct Bag {
  std::string auth_endpoint;
  /// From the `volumeStoreDownloadProduct` key: the ent/download URL.
  std::string ent_download_endpoint;
  std::string redownload_endpoint;
  std::string update_endpoint;
  std::string sap_setup_url;
  std::string sap_setup_cert_url;
  int sap_version = 0;
  PlistValue raw = PlistValue::dictionary();

  /// Extracts the endpoint fields without validating them, mirroring the
  /// lightweight bag fetch used by the download paths.
  [[nodiscard]] static Result<Bag> parse(const PlistValue& document);

  /// Validates the shape of every non-empty endpoint (scheme, host, path).
  [[nodiscard]] Status validate() const;

  /// Everything sign-in requires: a valid authentication endpoint, HTTPS SAP
  /// setup endpoints and SAP version 200. There is deliberately no unsigned
  /// fallback; Apple answers unsigned authentication requests with 403.
  [[nodiscard]] Status validate_for_authentication() const;
};

/// Builds the URL bag request for a protocol identity.
[[nodiscard]] std::string bag_request_url(const HardwareIdentity& identity);

/// Validates an authentication endpoint exactly as the Store expects: HTTPS
/// on buy.itunes.apple.com (or a pod host), the MZFinance authenticate path,
/// no credentials, fragment or encoded path, port absent or 443.
[[nodiscard]] Status validate_auth_endpoint(std::string_view endpoint);

/// Normalises a validated authentication endpoint so the legacy MZFinance
/// path ends in a slash, retaining host, port and query.
[[nodiscard]] Result<std::string> normalize_auth_endpoint(std::string_view endpoint);

}  // namespace ipashelf
