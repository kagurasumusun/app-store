// SPDX-License-Identifier: MIT
//
// SAP backend backed by the frameworks Apple ships with iOS.
//
// The App Store authenticates every request with an Apple-signed action
// signature. On macOS the signature is produced by CoreFP; iOS carries the same
// primitives in CoreFP/AppleMediaServices even though no public API exposes
// them. This file resolves the private entry points at runtime and calls them
// directly. If a future OS removes them, IPCreateSAPBackend reports that the
// signer is unavailable instead of sending unsigned credentials.

#import <Foundation/Foundation.h>

#include <dlfcn.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#import "IPInternal.h"

namespace {

using InitializeFn = int32_t (*)(std::uint64_t *context, void *hardwareBlock);
using ExchangeFn = int32_t (*)(std::uint32_t version, void *hardwareBlock,
                               std::uint64_t context, void *input,
                               std::size_t inputLength, void **output,
                               std::size_t *outputLength, int32_t *state);
using SignFn = int32_t (*)(std::uint64_t context, void *input,
                           std::size_t inputLength, void **output,
                           std::size_t *outputLength);
using TeardownFn = int32_t (*)(std::uint64_t context);
using DisposeFn = int32_t (*)(void *allocation);

struct SAPSymbols {
  InitializeFn initialize = nullptr;
  ExchangeFn exchange = nullptr;
  SignFn sign = nullptr;
  TeardownFn teardown = nullptr;
  DisposeFn dispose = nullptr;
  std::string problem;
};

void *OpenImage(const char *path) {
  void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  return handle;
}

void *FindSymbol(const std::vector<void *> &images, const char *name) {
  const std::string underscored = std::string("_") + name;
  for (void *image : images) {
    if (image == nullptr) continue;
    void *symbol = dlsym(image, name);
    if (symbol != nullptr) return symbol;
    symbol = dlsym(image, underscored.c_str());
    if (symbol != nullptr) return symbol;
  }
  for (const char *candidate : {name, underscored.c_str()}) {
    if (void *symbol = dlsym(RTLD_DEFAULT, candidate)) return symbol;
  }
  return nullptr;
}

SAPSymbols &Symbols() {
  static SAPSymbols symbols;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    const std::vector<std::string> candidates = {
        "/System/Library/PrivateFrameworks/CoreFP.framework/CoreFP",
        "/System/Library/PrivateFrameworks/AppleMediaServices.framework/"
        "AppleMediaServices",
        "/System/Library/PrivateFrameworks/CommerceKit.framework/CommerceKit",
        "/System/Library/PrivateFrameworks/CommerceCore.framework/CommerceCore",
        "/usr/lib/libAppleSSE.dylib",
    };
    std::vector<void *> images;
    for (const auto &path : candidates) {
      void *handle = OpenImage(path.c_str());
      if (handle != nullptr) images.push_back(handle);
    }
    // The dynamic loader also searches already-loaded images.
    images.push_back(nullptr);

    symbols.initialize =
        reinterpret_cast<InitializeFn>(FindSymbol(images, "cp2g1b9ro"));
    symbols.exchange =
        reinterpret_cast<ExchangeFn>(FindSymbol(images, "Mib5yocT"));
    symbols.sign = reinterpret_cast<SignFn>(FindSymbol(images, "Fc3vhtJDvr"));
    symbols.teardown =
        reinterpret_cast<TeardownFn>(FindSymbol(images, "IPaI1oem5iL"));
    symbols.dispose = reinterpret_cast<DisposeFn>(FindSymbol(images, "jEHf8Xzsv8K"));

    if (symbols.initialize == nullptr || symbols.exchange == nullptr ||
        symbols.sign == nullptr || symbols.teardown == nullptr ||
        symbols.dispose == nullptr) {
      symbols.problem =
          "SAP entry points are missing. App Store signed-in features require "
          "a physical iPhone or iPad; the Simulator does not ship CoreFP.";
    }
  });
  return symbols;
}

std::shared_ptr<ipashelf::SapBackend> SharedSAPBackend();

class IPAppleSAPBackend final : public ipashelf::SapBackend {
 public:
  static std::shared_ptr<IPAppleSAPBackend> Create() {
    return std::shared_ptr<IPAppleSAPBackend>(new IPAppleSAPBackend());
  }

  ipashelf::Result<std::uint64_t> initialize(
      const ipashelf::Bytes &hardware_block) override {
    if (hardware_block.empty()) {
      return ipashelf::make_error(ipashelf::ErrorCode::kInvalidArgument,
                                  "empty hardware block");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    std::uint64_t context = 0;
    std::vector<std::uint8_t> block(hardware_block);
    const int32_t status =
        Symbols().initialize(&context, static_cast<void *>(block.data()));
    ipashelf::secure_zero(block.data(), block.size());
    if (status != 0) {
      return ipashelf::make_error(
          ipashelf::ErrorCode::kSignerUnavailable,
          "SAP initialization failed (" + std::to_string(status) + ")");
    }
    if (context == 0) {
      return ipashelf::make_error(ipashelf::ErrorCode::kSignerUnavailable,
                                  "SAP initialization returned no context");
    }
    return context;
  }

  ipashelf::Result<std::pair<ipashelf::Bytes, std::int32_t>> exchange(
      std::uint32_t version, const ipashelf::Bytes &hardware_block,
      std::uint64_t context, const ipashelf::Bytes &input) override {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::uint8_t> block(hardware_block);
    std::vector<std::uint8_t> request(input);
    void *output = nullptr;
    std::size_t output_length = 0;
    std::int32_t state = 0;
    const int32_t status = Symbols().exchange(
        version, static_cast<void *>(block.data()), context,
        static_cast<void *>(request.data()), request.size(), &output,
        &output_length, &state);
    ipashelf::secure_zero(block.data(), block.size());
    if (status != 0) {
      return ipashelf::make_error(
          ipashelf::ErrorCode::kSignerUnavailable,
          "SAP exchange failed (" + std::to_string(status) + ")");
    }
    ipashelf::Bytes result;
    if (output != nullptr && output_length > 0) {
      const auto *bytes = static_cast<const std::uint8_t *>(output);
      result.assign(bytes, bytes + output_length);
    }
    if (output != nullptr) {
      Symbols().dispose(output);
    }
    return std::make_pair(std::move(result), state);
  }

  ipashelf::Result<ipashelf::Bytes> sign(
      std::uint64_t context, const ipashelf::Bytes &input) override {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::uint8_t> payload(input);
    void *output = nullptr;
    std::size_t output_length = 0;
    const int32_t status =
        Symbols().sign(context, static_cast<void *>(payload.data()),
                       payload.size(), &output, &output_length);
    if (status != 0) {
      return ipashelf::make_error(
          ipashelf::ErrorCode::kSignerUnavailable,
          "SAP signing failed (" + std::to_string(status) + ")");
    }
    ipashelf::Bytes result;
    if (output != nullptr && output_length > 0) {
      const auto *bytes = static_cast<const std::uint8_t *>(output);
      result.assign(bytes, bytes + output_length);
    }
    if (output != nullptr) {
      Symbols().dispose(output);
    }
    if (result.empty()) {
      return ipashelf::make_error(ipashelf::ErrorCode::kSignerUnavailable,
                                  "SAP returned an empty signature");
    }
    return result;
  }

  ipashelf::Status teardown(std::uint64_t context) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (context == 0) return ipashelf::ok_status();
    const int32_t status = Symbols().teardown(context);
    if (status != 0) {
      return ipashelf::fail(ipashelf::ErrorCode::kSignerUnavailable,
                            "SAP teardown failed (" + std::to_string(status) +
                                ")");
    }
    return ipashelf::ok_status();
  }

 private:
  IPAppleSAPBackend() = default;
  std::mutex mutex_;
};

std::shared_ptr<ipashelf::SapBackend> SharedSAPBackend() {
  static std::shared_ptr<ipashelf::SapBackend> backend =
      std::static_pointer_cast<ipashelf::SapBackend>(IPAppleSAPBackend::Create());
  return backend;
}

}  // namespace

ipashelf::Result<std::shared_ptr<ipashelf::SapBackend>> IPCreateSAPBackend() {
  SAPSymbols &symbols = Symbols();
  if (!symbols.problem.empty()) {
    return ipashelf::make_error(ipashelf::ErrorCode::kSignerUnavailable,
                                symbols.problem);
  }
  return SharedSAPBackend();
}
