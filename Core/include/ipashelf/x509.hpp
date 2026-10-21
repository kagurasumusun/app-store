// SPDX-License-Identifier: MIT
//
// Minimal X.509 v3 certificate construction for the on-device install server.
//
// iOS cannot create a `SecIdentity` from a key and a certificate; it needs
// either a PKCS#12 blob or a key/certificate pair that already lives in the
// keychain. Rather than implementing PKCS#12 (and its password-based
// encryption), IPAShelf builds a small X.509 certificate, signs it with an
// ECDSA P-256 key held by the Secure Enclave or the keychain, and lets the
// platform layer add the certificate next to its key.
//
// The module is deliberately boring: DER encoding only, no ASN.1 parser beyond
// a tiny helper used by the tests. Everything is portable C++.

#pragma once

#include <cstdint>
#include <ctime>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ipashelf/base.hpp"

namespace ipashelf {

struct CertificateName {
  std::string common_name;
  std::string organization = "IPAShelf";
  std::string organizational_unit;
  std::string country = "US";
};

struct SubjectAltName {
  std::string dns;
  std::optional<std::vector<std::uint8_t>> ip;  // 4 or 16 bytes
};

struct CertificateOptions {
  CertificateName subject;
  /// Used as the issuer. When empty the subject is reused (self-signed).
  CertificateName issuer;
  std::vector<SubjectAltName> alt_names;
  bool is_ca = false;
  std::int64_t serial = 1;
  /// Seconds since the Unix epoch. Zero means "now".
  std::time_t not_before = 0;
  std::uint64_t validity_seconds = 10ULL * 365 * 24 * 60 * 60;
  /// Authority key identifier to embed (the issuer's key identifier; see
  /// `ec_key_identifier`). Empty omits the AKID extension, except that a
  /// self-signed certificate reuses its own subject key identifier.
  Bytes authority_key_id;
};

/// DER signature (ECDSA-Sig-Value) for the given TBSCertificate.
using CertificateSigner = std::function<Result<Bytes>(const Bytes& tbs)>;

/// Wraps an uncompressed P-256 point (`0x04 || X || Y`, 65 bytes) in a
/// SubjectPublicKeyInfo with the `prime256v1` named curve.
[[nodiscard]] Result<Bytes> ec_public_key_info(const Bytes& uncompressed_point);

/// Builds a DER-encoded X.509 v3 certificate signed with ecdsa-with-SHA256.
[[nodiscard]] Result<Bytes> build_ec_certificate(
    const CertificateOptions& options, const Bytes& subject_public_key_info,
    const CertificateSigner& signer);

/// 20-byte key identifier for a P-256 public point: the leading bytes of the
/// SHA-256 of the uncompressed point, following the "hash of the public key"
/// method of RFC 5280 section 4.2.1.2. Used for SKID/AKID.
[[nodiscard]] Bytes ec_key_identifier(const Bytes& uncompressed_point);

/// Converts an IEEE P1363 ECDSA signature (`r || s`, fixed width) into the
/// DER ECDSA-Sig-Value X.509 expects.
[[nodiscard]] Result<Bytes> ecdsa_signature_p1363_to_der(const Bytes& p1363);

/// Normalizes an ECDSA signature to DER ECDSA-Sig-Value, accepting either
/// wire format: an already-DER signature (what `SecKeyCreateSignature`
/// with the X9.62 message algorithms returns) passes through unchanged,
/// P1363 `r || s` is converted. A `CertificateSigner` backed by platform
/// keys must pass its output through here rather than assuming a format:
/// assuming P1363 and converting a DER signature anyway mangles it into a
/// structurally valid SEQUENCE of two garbage integers — certificates
/// that parse everywhere and verify nowhere.
[[nodiscard]] Result<Bytes> ecdsa_signature_to_der(const Bytes& signature);

/// PEM (`-----BEGIN CERTIFICATE-----`) encoding of a DER certificate.
[[nodiscard]] std::string pem_encode(const Bytes& der,
                                     std::string_view label = "CERTIFICATE");

// ---------------------------------------------------------------------------
// Test helpers (also used by the bridge to sanity-check generated buffers)
// ---------------------------------------------------------------------------

struct DerElement {
  std::uint8_t tag = 0;
  Bytes header;
  Bytes content;
};

/// Reads one DER element starting at `offset`; advances `offset` past it.
[[nodiscard]] Result<DerElement> der_read_element(const Bytes& data,
                                                  std::size_t& offset);

/// Returns the DER encoding of `value` as an ASN.1 INTEGER.
[[nodiscard]] Bytes der_integer(std::int64_t value);

/// Returns the DER encoding of an OID from dotted notation.
[[nodiscard]] Result<Bytes> der_object_identifier(std::string_view dotted);

}  // namespace ipashelf
