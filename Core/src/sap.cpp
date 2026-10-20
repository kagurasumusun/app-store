// SPDX-License-Identifier: MIT

#include "ipashelf/sap.hpp"

#include "ipashelf/plist.hpp"
#include "ipashelf/url.hpp"

namespace ipashelf {
namespace {

constexpr std::string_view kCertificateKey = "sign-sap-setup-cert";
constexpr std::string_view kSetupBufferKey = "sign-sap-setup-buffer";
constexpr std::size_t kMaxSetupBody = 1U << 20;  // 1 MiB

Result<Bytes> send_setup_request(HttpTransport& transport,
                                 const HttpRequest& request,
                                 const CancellationToken& token) {
  auto response = transport.send(request, token);
  if (!response) return Result<Bytes>(response.error());
  if (response.value().status != 200) {
    return Error{ErrorCode::kHttpStatus,
                 "SAP setup request returned HTTP " +
                     std::to_string(response.value().status),
                 response.value().status};
  }
  if (response.value().body.size() > kMaxSetupBody) {
    return make_error(ErrorCode::kParse, "SAP setup response is too large");
  }
  return std::move(response.value().body);
}

}  // namespace

SapSession::SapSession(std::shared_ptr<SapBackend> backend,
                       std::uint64_t context, Bytes hardware_block)
    : backend_(std::move(backend)),
      context_(context),
      hardware_block_(std::move(hardware_block)) {}

SapSession::~SapSession() { (void)close(); }

Result<Bytes> SapSession::sign(const Bytes& data) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (closed_ || context_ == 0) {
    return make_error(ErrorCode::kSignerUnavailable, "SAP signer is closed");
  }
  auto signature = backend_->sign(context_, data);
  if (!signature) return signature;
  if (signature.value().empty()) {
    return make_error(ErrorCode::kInternal, "SAP signer returned an empty signature");
  }
  return signature;
}

Status SapSession::close() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (closed_) return ok_status();
  closed_ = true;
  Status status;
  if (context_ != 0 && backend_) {
    status = backend_->teardown(context_);
    context_ = 0;
  }
  secure_zero(hardware_block_.data(), hardware_block_.size());
  hardware_block_.clear();
  return status;
}

Bytes sap_setup_envelope(const Bytes& payload) {
  PlistValue root = PlistValue::dictionary();
  root.set(std::string(kSetupBufferKey), PlistValue::data(payload));
  const std::string xml = root.dump_xml();
  return Bytes(xml.begin(), xml.end());
}

Result<Bytes> sap_plist_data(const Bytes& body, std::string_view key) {
  auto plist = PlistValue::parse(body);
  if (!plist) return plist.error();
  const PlistValue* value = plist.value().find(key);
  if (value == nullptr || !value->is_data() || value->data_value().empty()) {
    return make_error(ErrorCode::kParse,
                      "SAP plist did not contain " + std::string(key));
  }
  return value->data_value();
}

Result<std::unique_ptr<SapSession>> establish_sap_session(
    HttpTransport& transport, const SapConfiguration& configuration,
    std::string_view user_agent, const CancellationToken& token,
    std::shared_ptr<SapBackend> backend) {
  if (configuration.version != 200) {
    return make_error(ErrorCode::kUnsupported,
                      "unsupported SAP setup version");
  }
  if (!valid_https_url(configuration.setup_url) ||
      !valid_https_url(configuration.certificate_url)) {
    return make_error(ErrorCode::kInvalidArgument,
                      "SAP setup endpoints must be absolute HTTPS URLs");
  }
  if (configuration.hardware.bytes.empty() ||
      configuration.hardware.bytes.size() > 20) {
    return make_error(ErrorCode::kInvalidArgument,
                      "SAP hardware identity is invalid");
  }
  if (token.cancelled()) {
    return make_error(ErrorCode::kCancelled, "SAP setup cancelled");
  }

  if (!backend) {
    return make_error(ErrorCode::kSignerUnavailable,
                      "SAP backend is not available");
  }

  auto hardware_block = configuration.hardware.sap_block();
  if (!hardware_block) return hardware_block.error();

  HttpRequest certificate_request;
  certificate_request.method = "GET";
  certificate_request.url = configuration.certificate_url;
  certificate_request.headers.set("User-Agent", std::string(user_agent));
  auto certificate_body = send_setup_request(transport, certificate_request, token);
  if (!certificate_body) return certificate_body.error();
  auto certificate = sap_plist_data(certificate_body.value(), kCertificateKey);
  if (!certificate) return certificate.error();

  auto context = backend->initialize(hardware_block.value());
  if (!context) return context.error();
  if (context.value() == 0) {
    return make_error(ErrorCode::kInternal,
                      "SAP initialization returned a null context");
  }

  const auto close_on_failure = [&](Error error) {
    (void)backend->teardown(context.value());
    return Result<std::unique_ptr<SapSession>>(std::move(error));
  };

  // First exchange: the backend produces the request sent to Apple.
  auto first = backend->exchange(configuration.version,
                                         hardware_block.value(),
                                         context.value(), certificate.value());
  if (!first) {
    return close_on_failure(first.error());
  }
  if (first.value().second != 1 || first.value().first.empty()) {
    return close_on_failure(make_error(
        ErrorCode::kInternal, "SAP setup did not enter the expected state"));
  }

  HttpRequest setup_request;
  setup_request.method = "POST";
  setup_request.url = configuration.setup_url;
  setup_request.headers.set("User-Agent", std::string(user_agent));
  setup_request.headers.set("Content-Type", "application/x-plist");
  setup_request.body = sap_setup_envelope(first.value().first);
  auto setup_body = send_setup_request(transport, setup_request, token);
  if (!setup_body) return close_on_failure(setup_body.error());

  auto reply = sap_plist_data(setup_body.value(), kSetupBufferKey);
  if (!reply) return close_on_failure(reply.error());

  auto second = backend->exchange(configuration.version,
                                          hardware_block.value(),
                                          context.value(), reply.value());
  if (!second) {
    return close_on_failure(second.error());
  }
  if (second.value().second != 0) {
    return close_on_failure(make_error(
        ErrorCode::kInternal, "SAP setup did not complete"));
  }

  return std::unique_ptr<SapSession>(
      new SapSession(backend, context.value(), hardware_block.value()));
}

// `create_platform_sap_backend` is defined by the pure-software SAP
// implementation in sap_software.cpp, which is part of the portable core
// and runs identically on every platform (including sandboxed iOS apps).

}  // namespace ipashelf
