// SPDX-License-Identifier: MIT
//
// SAP backend backed by the frameworks Apple ships with iOS.
//
// The App Store authenticates every request with an Apple-signed action
// signature. On macOS the signature is produced by CoreFP; iOS carries the
// same primitives in CoreFP/AppleMediaServices even though no public API
// exposes them. This file resolves the private entry points at runtime and
// calls them directly. If a future OS removes them, IPCreateSAPBackend
// reports that the signer is unavailable instead of sending unsigned
// credentials.
//
// Hardware identity:
//   The SAP setup call takes a 24-byte block: a little-endian u32 length
//   prefix followed by the 1-20 byte hardware identity. This matches the
//   layout ipashelf::HardwareIdentity::sap_block() produces. On iOS there is
//   no public MAC address API, so the app persists a random six-byte identity
//   in the Keychain (see IPDeviceIdentity.mm). Unsigned builds cannot access
//   the Keychain, so every launch would generate a fresh identity and SAP
//   signing would fail; sign the app before testing on a real device.

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

/// Candidate SAP entry-point names observed across iOS releases. The symbols
/// are obfuscated and change between OS versions, so we try several spellings
/// per primitive and report exactly which ones were still missing.
struct SapSymbolCandidates {
  const char *initialize[4];
  const char *exchange[4];
  const char *sign[4];
  const char *teardown[4];
  const char *dispose[4];
};

constexpr SapSymbolCandidates kSapSymbolCandidates{
    {
        "cp2g1b9ro",   // iOS 16/17 era
        "cp2g1b9ro2",  // possible iOS 18+ suffix
        "_cp2g1b9ro",  // leading-underscore variant
        nullptr,
    },
    {
        "Mib5yocT",
        "Mib5yocT2",
        "_Mib5yocT",
        nullptr,
    },
    {
        "Fc3vhtJDvr",
        "Fc3vhtJDvr2",
        "_Fc3vhtJDvr",
        nullptr,
    },
    {
        "IPaI1oem5iL",
        "IPaI1oem5iL2",
        "_IPaI1oem5iL",
        nullptr,
    },
    {
        "jEHf8Xzsv8K",
        "jEHf8Xzsv8K2",
        "_jEHf8Xzsv8K",
        nullptr,
    },
};

/// Framework search order. CoreFP is the primary carrier of the SAP
/// primitives; AppleMediaServices and CommerceKit/CommerceCore are fallbacks
/// observed on other iOS releases.
constexpr const char *kSapFrameworkCandidates[] = {
    "/System/Library/PrivateFrameworks/CoreFP.framework/CoreFP",
    "/System/Library/PrivateFrameworks/AppleMediaServices.framework/"
    "AppleMediaServices",
    "/System/Library/PrivateFrameworks/CommerceKit.framework/CommerceKit",
    "/System/Library/PrivateFrameworks/CommerceCore.framework/CommerceCore",
    "/usr/lib/libAppleSSE.dylib",
};

template <typename Fn>
static Fn FindSapSymbol(const std::vector<void *> &images,
                        const char *const *candidates) {
  for (std::size_t index = 0; candidates[index] != nullptr; ++index) {
    if (Fn symbol = reinterpret_cast<Fn>(FindSymbol(images, candidates[index])))
      return symbol;
  }
  return nullptr;
}

void *OpenImage(const char *path) {
  void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (handle == nullptr) {
    NSLog(@"IPAShelf: could not load SAP framework %s: %s", path,
          dlerror() ?: "unknown error");
  }
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

/// Records which framework supplied each entry point, for diagnostics.
struct SAPSymbols {
  InitializeFn initialize = nullptr;
  ExchangeFn exchange = nullptr;
  SignFn sign = nullptr;
  TeardownFn teardown = nullptr;
  DisposeFn dispose = nullptr;
  const char *initialize_from = nullptr;
  const char *exchange_from = nullptr;
  const char *sign_from = nullptr;
  const char *teardown_from = nullptr;
  const char *dispose_from = nullptr;
  std::string problem;
};

template <typename Fn>
static Fn FindSapSymbolFrom(const std::vector<void *> &images,
                            const char *const *candidates,
                            const char *&from_out) {
  for (std::size_t index = 0; candidates[index] != nullptr; ++index) {
    if (Fn symbol = reinterpret_cast<Fn>(FindSymbol(images, candidates[index]))) {
      from_out = candidates[index];
      return symbol;
    }
  }
  from_out = nullptr;
  return nullptr;
}

SAPSymbols &Symbols() {
  static SAPSymbols symbols;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    std::vector<void *> images;
    std::vector<const char *> loaded;
    for (const char *path : kSapFrameworkCandidates) {
      void *handle = OpenImage(path);
      if (handle != nullptr) {
        images.push_back(handle);
        loaded.push_back(path);
      }
    }
    // The dynamic loader also searches already-loaded images.
    images.push_back(nullptr);

    symbols.initialize =
        FindSapSymbolFrom<InitializeFn>(images, kSapSymbolCandidates.initialize,
                                        symbols.initialize_from);
    symbols.exchange =
        FindSapSymbolFrom<ExchangeFn>(images, kSapSymbolCandidates.exchange,
                                      symbols.exchange_from);
    symbols.sign =
        FindSapSymbolFrom<SignFn>(images, kSapSymbolCandidates.sign,
                                  symbols.sign_from);
    symbols.teardown =
        FindSapSymbolFrom<TeardownFn>(images, kSapSymbolCandidates.teardown,
                                      symbols.teardown_from);
    symbols.dispose =
        FindSapSymbolFrom<DisposeFn>(images, kSapSymbolCandidates.dispose,
                                     symbols.dispose_from);

    if (symbols.initialize == nullptr || symbols.exchange == nullptr ||
        symbols.sign == nullptr || symbols.teardown == nullptr ||
        symbols.dispose == nullptr) {
      std::vector<std::string> missing;
      if (symbols.initialize == nullptr) missing.push_back("initialize");
      if (symbols.exchange == nullptr) missing.push_back("exchange");
      if (symbols.sign == nullptr) missing.push_back("sign");
      if (symbols.teardown == nullptr) missing.push_back("teardown");
      if (symbols.dispose == nullptr) missing.push_back("dispose");
      std::string detail;
      for (std::size_t index = 0; index < missing.size(); ++index) {
        if (index > 0) detail += ", ";
        detail += missing[index];
      }
      std::string loaded_detail;
      for (std::size_t index = 0; index < loaded.size(); ++index) {
        if (index > 0) loaded_detail += ", ";
        loaded_detail += loaded[index];
      }
      symbols.problem =
          "SAP entry points missing (" + detail + "). Loaded frameworks: " +
          loaded_detail +
          ". App Store signed-in features require a physical iPhone or iPad; "
          "the Simulator does not ship CoreFP. On iOS 27 the obfuscated "
          "entry-point names may have changed; rebuild with updated symbol "
          "candidates in IpaStore/Bridge/IPSAPBackend.mm.";
      NSLog(@"IPAShelf: %s", symbols.problem.c_str());
    } else {
      NSLog(@"IPAShelf: SAP backend ready (initialize=%s, exchange=%s, "
            "sign=%s, teardown=%s, dispose=%s)",
            symbols.initialize_from ?: "?",
            symbols.exchange_from ?: "?",
            symbols.sign_from ?: "?",
            symbols.teardown_from ?: "?",
            symbols.dispose_from ?: "?");
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
    // The SAP setup block is 24 bytes: LE u32 length prefix followed by the
    // 1-20 byte hardware identity. ipashelf::HardwareIdentity::sap_block()
    // produces exactly this layout.
    if (hardware_block.size() < 4) {
      return ipashelf::make_error(
          ipashelf::ErrorCode::kInvalidArgument,
          "hardware block too short (need at least 4-byte length prefix)");
    }
    std::uint32_t declared = 0;
    std::memcpy(&declared, hardware_block.data(), 4);
    if (declared < 1 || declared > 20 ||
        declared + 4 > hardware_block.size()) {
      return ipashelf::make_error(
          ipashelf::ErrorCode::kInvalidArgument,
          "hardware block length prefix is inconsistent with block size");
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
          "SAP initialization failed (status " + std::to_string(status) +
              "); the hardware identity may be invalid or the device may not "
          "support App Store signing");
    }
    if (context == 0) {
      return ipashelf::make_error(
          ipashelf::ErrorCode::kSignerUnavailable,
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
          "SAP exchange failed (status " + std::to_string(status) + ")");
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
          "SAP signing failed (status " + std::to_string(status) + ")");
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
                            "SAP teardown failed (status " +
                                std::to_string(status) + ")");
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
