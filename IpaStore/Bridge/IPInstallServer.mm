// SPDX-License-Identifier: MIT

#import "IPInstallServer.h"

#import <Foundation/Foundation.h>
#import <Network/Network.h>
#import <Security/Security.h>
#import <Security/SecProtocolOptions.h>
#import <Security/SecProtocolTypes.h>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <system_error>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ipashelf/crypto.hpp"
#include "ipashelf/ota.hpp"
#include "ipashelf/x509.hpp"

#import "IPInternal.h"

namespace {

constexpr std::size_t kChunkSize = 64 * 1024;

NSError *IPInstallError(IPStoreErrorCode code, NSString *message) {
  return [NSError errorWithDomain:IPStoreErrorDomain
                             code:code
                         userInfo:@{NSLocalizedDescriptionKey : message}];
}

NSString *IPCertificateDirectory(void) {
  NSFileManager *manager = [NSFileManager defaultManager];
  NSURL *support = [[manager URLsForDirectory:NSApplicationSupportDirectory
                                    inDomains:NSUserDomainMask] firstObject];
  NSURL *directory =
      [support URLByAppendingPathComponent:@"IPAShelf/Certificates"
                               isDirectory:YES];
  [manager createDirectoryAtURL:directory
      withIntermediateDirectories:YES
                       attributes:nil
                            error:nil];
  [directory setResourceValue:@YES
                       forKey:NSURLIsExcludedFromBackupKey
                        error:nil];
  return directory.path ?: NSTemporaryDirectory();
}

NSString *IPPreferredHost(void) {
  struct ifaddrs *interfaces = NULL;
  if (getifaddrs(&interfaces) != 0) return @"127.0.0.1";
  NSString *fallback = nil;
  NSString *result = nil;
  for (struct ifaddrs *cursor = interfaces; cursor != NULL;
       cursor = cursor->ifa_next) {
    if (cursor->ifa_addr == NULL ||
        cursor->ifa_addr->sa_family != AF_INET) {
      continue;
    }
    const auto *address =
        reinterpret_cast<const struct sockaddr_in *>(cursor->ifa_addr);
    char buffer[INET_ADDRSTRLEN] = {0};
    if (inet_ntop(AF_INET, &address->sin_addr, buffer, sizeof(buffer)) ==
        NULL) {
      continue;
    }
    NSString *text = @(buffer);
    if ([text hasPrefix:@"127."]) continue;
    NSString *name = @(cursor->ifa_name ?: "");
    if ([name isEqualToString:@"en0"]) {
      result = text;
      break;
    }
    if (fallback == nil && ![name isEqualToString:@"lo0"]) fallback = text;
  }
  freeifaddrs(interfaces);
  return result ?: fallback ?: @"127.0.0.1";
}

std::vector<std::vector<std::uint8_t>> IPLocalIPv4Bytes(void) {
  std::vector<std::vector<std::uint8_t>> output;
  output.push_back({127, 0, 0, 1});
  struct ifaddrs *interfaces = NULL;
  if (getifaddrs(&interfaces) != 0) return output;
  for (struct ifaddrs *cursor = interfaces; cursor != NULL;
       cursor = cursor->ifa_next) {
    if (cursor->ifa_addr == NULL ||
        cursor->ifa_addr->sa_family != AF_INET) {
      continue;
    }
    const auto *address =
        reinterpret_cast<const struct sockaddr_in *>(cursor->ifa_addr);
    const auto *bytes = reinterpret_cast<const std::uint8_t *>(&address->sin_addr);
    std::vector<std::uint8_t> value(bytes, bytes + 4);
    if (std::find(output.begin(), output.end(), value) == output.end()) {
      output.push_back(std::move(value));
    }
  }
  freeifaddrs(interfaces);
  return output;
}

SecKeyRef IPCopyOrCreateKey(NSString *tag, NSError **error) CF_RETURNS_RETAINED {
  NSData *tagData = [tag dataUsingEncoding:NSUTF8StringEncoding];
  NSDictionary *query = @{
    (__bridge id)kSecClass : (__bridge id)kSecClassKey,
    (__bridge id)kSecAttrApplicationTag : tagData,
    (__bridge id)kSecAttrKeyType : (__bridge id)kSecAttrKeyTypeECSECPrimeRandom,
    (__bridge id)kSecReturnRef : @YES,
  };
  CFTypeRef existing = NULL;
  const OSStatus found =
      SecItemCopyMatching((__bridge CFDictionaryRef)query, &existing);
  if (found == errSecSuccess && existing != NULL) {
    return (SecKeyRef)existing;
  }
  NSDictionary *attributes = @{
    (__bridge id)kSecAttrKeyType : (__bridge id)kSecAttrKeyTypeECSECPrimeRandom,
    (__bridge id)kSecAttrKeySizeInBits : @256,
    (__bridge id)kSecPrivateKeyAttrs : @{
      (__bridge id)kSecAttrIsPermanent : @YES,
      (__bridge id)kSecAttrApplicationTag : tagData,
      (__bridge id)kSecAttrAccessible :
          (__bridge id)kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly,
      (__bridge id)kSecAttrLabel : tag,
    },
  };
  CFErrorRef cfError = NULL;
  SecKeyRef key = SecKeyCreateRandomKey((__bridge CFDictionaryRef)attributes,
                                        &cfError);
  if (key == NULL) {
    // The failing OSStatus is what tells an entitlement problem apart from a
    // keychain or algorithm problem, so surface it instead of a bland message.
    NSString *detail = @"no error was reported";
    if (cfError != NULL) {
      CFStringRef description = CFErrorCopyDescription(cfError);
      if (description != NULL) {
        detail = [NSString stringWithFormat:@"%@ (code %ld)",
                                            CFBridgingRelease(description),
                                            (long)CFErrorGetCode(cfError)];
      } else {
        detail = [NSString stringWithFormat:@"code %ld",
                                            (long)CFErrorGetCode(cfError)];
      }
    }
    NSLog(@"IPAShelf: creating the install-server key failed: %@ (lookup %d)",
          detail, (int)found);
    if (error != NULL) {
      *error = IPInstallError(
          IPStoreErrorSignerUnavailable,
          [NSString stringWithFormat:@"Could not create the install-server key: %@",
                                     detail]);
    }
  }
  if (cfError != NULL) CFRelease(cfError);
  return key;
}

NSData *IPPublicKeyPoint(SecKeyRef key) {
  SecKeyRef publicKey = SecKeyCopyPublicKey(key);
  if (publicKey == NULL) return nil;
  CFErrorRef error = NULL;
  CFDataRef data = SecKeyCopyExternalRepresentation(publicKey, &error);
  CFRelease(publicKey);
  if (error != NULL) {
    CFRelease(error);
    return nil;
  }
  return CFBridgingRelease(data);
}

ipashelf::CertificateSigner IPSignerForKey(SecKeyRef key) {
  return [key](const ipashelf::Bytes &tbs) -> ipashelf::Result<ipashelf::Bytes> {
    NSData *data = [NSData dataWithBytes:tbs.data() length:tbs.size()];
    CFErrorRef cfError = NULL;
    CFDataRef signature = SecKeyCreateSignature(
        key, kSecKeyAlgorithmECDSASignatureMessageX962SHA256,
        (__bridge CFDataRef)data, &cfError);
    if (signature == NULL) {
      if (cfError != NULL) CFRelease(cfError);
      return ipashelf::make_error(ipashelf::ErrorCode::kSignerUnavailable,
                                  "SecKeyCreateSignature failed");
    }
    ipashelf::Bytes output;
    const auto *bytes =
        static_cast<const std::uint8_t *>(CFDataGetBytePtr(signature));
    output.assign(bytes, bytes + CFDataGetLength(signature));
    CFRelease(signature);
    return output;
  };
}

void IPAddCertificateToKeychain(NSData *der, NSString *label) {
  NSDictionary *deleteQuery = @{
    (__bridge id)kSecClass : (__bridge id)kSecClassCertificate,
    (__bridge id)kSecAttrLabel : label,
  };
  SecItemDelete((__bridge CFDictionaryRef)deleteQuery);

  SecCertificateRef certificate = SecCertificateCreateWithData(
      kCFAllocatorDefault, (__bridge CFDataRef)der);
  if (certificate == NULL) return;
  NSDictionary *attributes = @{
    (__bridge id)kSecClass : (__bridge id)kSecClassCertificate,
    (__bridge id)kSecValueRef : (__bridge id)certificate,
    (__bridge id)kSecAttrLabel : label,
  };
  SecItemAdd((__bridge CFDictionaryRef)attributes, NULL);
  CFRelease(certificate);
}

/// Builds an Apple configuration profile that installs the root certificate.
/// The payload UUID is reused for both the profile and the certificate payload
/// so iOS shows a single install prompt.
static NSData *IPProfileMobileConfig(NSData *caDER) {
  CFUUIDRef uuid = CFUUIDCreate(kCFAllocatorDefault);
  CFStringRef uuidString = CFUUIDCreateString(kCFAllocatorDefault, uuid);
  CFRelease(uuid);
  NSString *payload = (__bridge NSString *)uuidString;
  CFRelease(uuidString);

  NSString *base64 = [caDER base64EncodedStringWithOptions:0];
  NSString *xml = [NSString stringWithFormat:
      @"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      @"<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
      @"<plist version=\"1.0\">\n"
      @"<dict>\n"
      @"    <key>PayloadContent</key>\n"
      @"    <array>\n"
      @"        <dict>\n"
      @"            <key>PayloadCertificateFile</key>\n"
      @"            <data>%@</data>\n"
      @"            <key>PayloadDescription</key>\n"
      @"            <string>IPAShelf Root CA</string>\n"
      @"            <key>PayloadDisplayName</key>\n"
      @"            <string>IPAShelf Root CA</string>\n"
      @"            <key>PayloadIdentifier</key>\n"
      @"            <string>com.example.ipastore.profile.certificate</string>\n"
      @"            <key>PayloadOrganization</key>\n"
      @"            <string>IPAShelf</string>\n"
      @"            <key>PayloadType</key>\n"
      @"            <string>com.apple.security.root</string>\n"
      @"            <key>PayloadUUID</key>\n"
      @"            <string>%@</string>\n"
      @"            <key>PayloadVersion</key>\n"
      @"            <integer>1</integer>\n"
      @"        </dict>\n"
      @"    </array>\n"
      @"    <key>PayloadDescription</key>\n"
      @"    <string>IPAShelf local install server root certificate</string>\n"
      @"    <key>PayloadDisplayName</key>\n"
      @"    <string>IPAShelf Root CA</string>\n"
      @"    <key>PayloadIdentifier</key>\n"
      @"    <string>com.example.ipastore.profile</string>\n"
      @"    <key>PayloadOrganization</key>\n"
      @"    <string>IPAShelf</string>\n"
      @"    <key>PayloadRemovalDisallowed</key>\n"
      @"    <false/>\n"
      @"    <key>PayloadType</key>\n"
      @"    <string>Configuration</string>\n"
      @"    <key>PayloadUUID</key>\n"
      @"    <string>%@</string>\n"
      @"    <key>PayloadVersion</key>\n"
      @"    <integer>1</integer>\n"
      @"</dict>\n"
      @"</plist>\n",
      base64, payload, payload];
  return [xml dataUsingEncoding:NSUTF8StringEncoding];
}

SecIdentityRef IPCopyIdentityForCertificate(NSData *der) CF_RETURNS_RETAINED {
  NSDictionary *query = @{
    (__bridge id)kSecClass : (__bridge id)kSecClassIdentity,
    (__bridge id)kSecReturnRef : @YES,
    (__bridge id)kSecMatchLimit : (__bridge id)kSecMatchLimitAll,
  };
  CFTypeRef items = NULL;
  if (SecItemCopyMatching((__bridge CFDictionaryRef)query, &items) !=
          errSecSuccess ||
      items == NULL) {
    return NULL;
  }
  SecIdentityRef found = NULL;
  CFArrayRef array = (CFArrayRef)items;
  for (CFIndex index = 0; index < CFArrayGetCount(array); ++index) {
    auto identity =
        (SecIdentityRef)CFArrayGetValueAtIndex(array, index);
    SecCertificateRef certificate = NULL;
    if (SecIdentityCopyCertificate(identity, &certificate) != errSecSuccess ||
        certificate == NULL) {
      continue;
    }
    CFDataRef data = SecCertificateCopyData(certificate);
    const bool matches = data != NULL &&
                         CFEqual(data, (__bridge CFDataRef)der);
    if (data != NULL) CFRelease(data);
    CFRelease(certificate);
    if (matches) {
      found = (SecIdentityRef)CFRetain(identity);
      break;
    }
  }
  CFRelease(items);
  return found;
}

bool IPCertificateMatchesKey(SecCertificateRef certificate, SecKeyRef key) {
  SecKeyRef certificateKey = SecCertificateCopyKey(certificate);
  if (certificateKey == NULL) return false;
  SecKeyRef publicKey = SecKeyCopyPublicKey(key);
  if (publicKey == NULL) {
    CFRelease(certificateKey);
    return false;
  }
  CFErrorRef firstError = NULL;
  CFErrorRef secondError = NULL;
  CFDataRef first =
      SecKeyCopyExternalRepresentation(certificateKey, &firstError);
  CFDataRef second = SecKeyCopyExternalRepresentation(publicKey, &secondError);
  bool matches = false;
  if (first != NULL && second != NULL) {
    matches = CFEqual(first, second);
  }
  if (first != NULL) CFRelease(first);
  if (second != NULL) CFRelease(second);
  if (firstError != NULL) CFRelease(firstError);
  if (secondError != NULL) CFRelease(secondError);
  CFRelease(certificateKey);
  CFRelease(publicKey);
  return matches;
}

}  // namespace

// ---------------------------------------------------------------------------
// Certificate value type
// ---------------------------------------------------------------------------

@interface IPInstallCertificate ()
@property (nonatomic, copy, readwrite) NSString *commonName;
@property (nonatomic, copy, readwrite) NSData *der;
@property (nonatomic, copy, readwrite) NSString *sha256Fingerprint;
- (instancetype)initWithCommonName:(NSString *)commonName
                               der:(NSData *)der;
@end

namespace {

NSString *IPFingerprint(NSData *der) {
  if (der.length == 0) return @"";
  const auto digest = ipashelf::sha256(der.bytes, der.length);
  const std::string hex = ipashelf::hex_encode(
      ipashelf::Bytes(digest.begin(), digest.end()), true);
  NSMutableString *text = [NSMutableString string];
  for (std::size_t index = 0; index < hex.size(); index += 2) {
    if (text.length > 0) [text appendString:@":"];
    [text appendString:[NSString stringWithUTF8String:hex.substr(index, 2).c_str()]];
  }
  return text;
}

}  // namespace

@implementation IPInstallCertificate

- (instancetype)initWithCommonName:(NSString *)commonName
                               der:(NSData *)der {
  self = [super init];
  if (self != nil) {
    _commonName = [commonName copy];
    _der = [der copy];
    _sha256Fingerprint = IPFingerprint(der);
  }
  return self;
}

@end

// ---------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------

@class IPInstallConnection;

@interface IPInstallServer ()
@property (nonatomic, readwrite) IPInstallServerState state;
@property (nonatomic, strong, readwrite, nullable) NSError *startupError;
/// Internal forms. They are not exposed to Swift because the
/// `NSError **` convention would rename them during import.
- (BOOL)startWithError:(NSError *_Nullable *_Nullable)error;
+ (nullable IPInstallCertificate *)prepareCertificateWithError:
    (NSError *_Nullable *_Nullable)error;
- (void)notePayloadProgress:(double)fraction message:(NSString *)message;
- (void)connectionDidFinish:(IPInstallConnection *)connection;
- (void)reportFailure:(NSError *)error;
@end

namespace {

struct ConnectionState {
  std::shared_ptr<ipashelf::HttpRequestStream> stream =
      std::make_shared<ipashelf::HttpRequestStream>();
  std::shared_ptr<ipashelf::OtaRouter> router;
  std::shared_ptr<std::ifstream> file;
  std::uint64_t fileTotal = 0;
  std::uint64_t fileSent = 0;
  std::string head;
  bool finished = false;
};

}  // namespace

@interface IPInstallConnection : NSObject
- (instancetype)initWithConnection:(nw_connection_t)connection
                            router:(std::shared_ptr<ipashelf::OtaRouter>)router
                            server:(IPInstallServer *)server;
- (void)start;
- (void)cancel;
@end

@implementation IPInstallConnection {
  nw_connection_t _connection;
  std::shared_ptr<ipashelf::OtaRouter> _router;
  std::shared_ptr<ConnectionState> _state;
  __weak IPInstallServer *_server;
  bool _cancelled;
}

- (instancetype)initWithConnection:(nw_connection_t)connection
                            router:(std::shared_ptr<ipashelf::OtaRouter>)router
                            server:(IPInstallServer *)server {
  self = [super init];
  if (self != nil) {
    _connection = connection;
    _router = std::move(router);
    _server = server;
    _state = std::make_shared<ConnectionState>();
    _state->router = _router;
  }
  return self;
}

- (void)start {
  nw_connection_set_queue(_connection,
                          dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0));
  __weak IPInstallConnection *weakSelf = self;
  nw_connection_set_state_changed_handler(
      _connection, ^(nw_connection_state_t state, nw_error_t error) {
        if (state == nw_connection_state_failed ||
            state == nw_connection_state_cancelled) {
          [weakSelf finish];
        }
      });
  nw_connection_start(_connection);
  [self receiveNext];
}

- (void)receiveNext {
  if (_cancelled) return;
  __weak IPInstallConnection *weakSelf = self;
  nw_connection_receive(
      _connection, 1, 64 * 1024,
      ^(dispatch_data_t content, nw_content_context_t context,
        bool is_complete, nw_error_t error) {
        IPInstallConnection *strongSelf = weakSelf;
        if (strongSelf == nil) return;
        [strongSelf handleData:content complete:is_complete error:error];
      });
}

- (void)handleData:(dispatch_data_t)content
          complete:(bool)complete
             error:(nw_error_t)error {
  if (_cancelled) return;
  if (error != nil) {
    [self finish];
    return;
  }
  if (content != NULL) {
    const void *bytes = NULL;
    size_t size = 0;
    dispatch_data_t mapped = dispatch_data_create_map(content, &bytes, &size);
    if (bytes != NULL && size > 0) {
      _state->stream->append(bytes, size);
    }
    (void)mapped;
  }
  auto request = _state->stream->next();
  if (!request.has_value()) {
    if (complete) {
      [self finish];
      return;
    }
    [self receiveNext];
    return;
  }
  if (!request.value()) {
    const std::string response =
        "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    [self sendBytes:response isComplete:false thenFinish:YES];
    return;
  }
  if (complete && _state->stream->buffered() == 0) {
    // No more bytes are coming; the request is still complete.
  }
  const ipashelf::HttpServerResponse response =
      _router->route(request.value().value());
  _state->head = response.head(
      ipashelf::format_http_date(std::time(nullptr)));
  if (response.head_only) {
    [self sendBytes:_state->head isComplete:false thenFinish:YES];
    return;
  }
  if (response.has_file()) {
    _state->file = std::make_shared<std::ifstream>(response.file_path,
                                                   std::ios::binary);
    if (!*_state->file) {
      [self sendBytes:_state->head isComplete:false thenFinish:YES];
      return;
    }
    _state->file->seekg(static_cast<std::streamoff>(response.file_offset));
    _state->fileTotal = response.file_length;
    _state->fileSent = 0;
    if (![self sendString:_state->head]) return;
    [self sendNextChunk];
    return;
  }
  std::string payload = _state->head + response.body;
  [self sendBytes:payload isComplete:false thenFinish:YES];
}

- (BOOL)sendString:(const std::string &)text {
  if (text.empty()) return YES;
  NSData *data = [NSData dataWithBytes:text.data() length:text.size()];
  return [self sendData:data];
}

- (BOOL)sendData:(NSData *)data {
  if (_cancelled) return NO;
  dispatch_data_t payload = dispatch_data_create(
      data.bytes, data.length, dispatch_get_main_queue(), ^{
        // Keeps the NSData alive until Network.framework releases the buffer.
        (void)data;
      });
  nw_connection_send(_connection, payload,
                     NW_CONNECTION_DEFAULT_STREAM_CONTEXT, false,
                     ^(nw_error_t error) {
                       (void)error;
                     });
  return YES;
}

- (void)sendNextChunk {
  if (_cancelled) return;
  if (_state->fileTotal == 0) {
    [self finishSending];
    return;
  }
  const std::size_t wanted = static_cast<std::size_t>(
      std::min<std::uint64_t>(_state->fileTotal - _state->fileSent, kChunkSize));
  std::vector<char> buffer(wanted);
  _state->file->read(buffer.data(), static_cast<std::streamsize>(wanted));
  const std::streamsize read = _state->file->gcount();
  if (read <= 0) {
    [self finishSending];
    return;
  }
  NSData *data = [NSData dataWithBytes:buffer.data()
                                length:static_cast<NSUInteger>(read)];
  _state->fileSent += static_cast<std::uint64_t>(read);
  const double fraction = _state->fileTotal == 0
                              ? 0
                              : static_cast<double>(_state->fileSent) /
                                    static_cast<double>(_state->fileTotal);
  [_server notePayloadProgress:fraction message:nil];
  __weak IPInstallConnection *weakSelf = self;
  dispatch_data_t payload = dispatch_data_create(
      data.bytes, data.length,
      dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        (void)data;
      });
  nw_connection_send(
      _connection, payload, NW_CONNECTION_DEFAULT_STREAM_CONTEXT, false,
      ^(nw_error_t error) {
        IPInstallConnection *strongSelf = weakSelf;
        if (strongSelf == nil) return;
        if (error != nil) {
          [strongSelf finish];
        } else {
          [strongSelf sendNextChunk];
        }
      });
}

- (void)finishSending {
  if (_state->finished) return;
  _state->finished = true;
  __weak IPInstallConnection *weakSelf = self;
  nw_connection_send(_connection, NULL,
                     NW_CONNECTION_DEFAULT_STREAM_CONTEXT, true,
                     ^(nw_error_t error) {
                       (void)error;
                       IPInstallConnection *strongSelf = weakSelf;
                       if (strongSelf != nil) [strongSelf finish];
                     });
}

- (void)sendBytes:(const std::string &)text
       isComplete:(BOOL)complete
      thenFinish:(BOOL)finish {
  (void)complete;
  if (text.empty()) {
    [self finishSending];
    return;
  }
  NSData *data = [NSData dataWithBytes:text.data() length:text.size()];
  dispatch_data_t payload = dispatch_data_create(
      data.bytes, data.length, dispatch_get_main_queue(), ^{
        (void)data;
      });
  __weak IPInstallConnection *weakSelf = self;
  nw_connection_send(_connection, payload,
                     NW_CONNECTION_DEFAULT_STREAM_CONTEXT, false,
                     ^(nw_error_t error) {
                       IPInstallConnection *strongSelf = weakSelf;
                       if (strongSelf == nil) return;
                       if (error != nil) {
                         [strongSelf finish];
                       } else if (finish) {
                         [strongSelf finishSending];
                       }
                     });
}

- (void)finish {
  if (_cancelled) return;
  _cancelled = true;
  nw_connection_cancel(_connection);
  [_server connectionDidFinish:self];
}

- (void)cancel {
  [self finish];
}

@end

@implementation IPInstallCertificateResult

- (instancetype)initWithCertificate:(nullable IPInstallCertificate *)certificate
                              error:(nullable NSError *)error {
  self = [super init];
  if (self != nil) {
    _certificate = certificate;
    _error = error;
  }
  return self;
}

@end

@implementation IPInstallServer {
  NSString *_bundleID;
  NSString *_version;
  NSString *_title;
  NSString *_ipaPath;
  NSString *_artworkPath;

  nw_listener_t _listener;
  dispatch_queue_t _tlsQueue;
  SecIdentityRef _identity;
  std::shared_ptr<ipashelf::OtaRouter> _router;
  std::shared_ptr<ipashelf::OtaRouter> _plainRouter;
  std::mutex _routerMutex;
  std::atomic<bool> _stopping;

  int _plainSocket;
  int _plainPort;
  int _tlsPort;
  NSString *_host;
  NSString *_caPath;

  NSMutableSet<IPInstallConnection *> *_connections;

  NSTimeInterval _lastProgressReport;
}

- (instancetype)initWithBundleID:(NSString *)bundleID
                         version:(NSString *)version
                           title:(NSString *)title
                       ipaAtPath:(NSString *)ipaPath
                     artworkPath:(nullable NSString *)artworkPath {
  self = [super init];
  if (self != nil) {
    _bundleID = [bundleID copy];
    _version = [version copy];
    _title = [title copy];
    _ipaPath = [ipaPath copy];
    _artworkPath = [artworkPath copy];
    _tlsQueue = dispatch_queue_create("com.example.ipastore.install.tls",
                                      DISPATCH_QUEUE_SERIAL);
    _connections = [NSMutableSet set];
    _plainSocket = -1;
    _plainPort = 0;
    _tlsPort = 0;
    _state = IPInstallServerStateIdle;
    _stopping.store(false);
  }
  return self;
}

- (void)dealloc {
  [self stop];
  if (_identity != NULL) {
    CFRelease(_identity);
    _identity = NULL;
  }
}

+ (IPInstallCertificateResult *)prepareCertificate {
  NSError *error = nil;
  IPInstallCertificate *certificate = [self prepareCertificateWithError:&error];
  if (certificate == nil && error == nil) {
    error = IPInstallError(IPStoreErrorInternal,
                           @"Certificate generation failed.");
  }
  return [[IPInstallCertificateResult alloc] initWithCertificate:certificate
                                                           error:error];
}

- (BOOL)start {
  NSError *error = nil;
  const BOOL started = [self startWithError:&error];
  _startupError = started ? nil : (error ?: IPInstallError(
                                             IPStoreErrorInternal,
                                             @"The install server did not start."));
  return started;
}


#pragma mark - Certificate

+ (nullable IPInstallCertificate *)prepareCertificateWithError:
    (NSError *_Nullable *_Nullable)error {
  NSString *directory = IPCertificateDirectory();
  NSString *caPath = [directory stringByAppendingPathComponent:@"root-ca.der"];

  NSError *keyError = nil;
  SecKeyRef caKey = IPCopyOrCreateKey(@"com.example.ipastore.install.ca-key",
                                      &keyError);
  if (caKey == NULL) {
    if (error != NULL) {
      *error = keyError ?: IPInstallError(IPStoreErrorSignerUnavailable,
                                          @"Could not create the CA key.");
    }
    return nil;
  }

  NSData *caDER = [NSData dataWithContentsOfFile:caPath];
  SecCertificateRef existing = caDER.length > 0
                                   ? SecCertificateCreateWithData(
                                         kCFAllocatorDefault,
                                         (__bridge CFDataRef)caDER)
                                   : NULL;
  if (existing != NULL && !IPCertificateMatchesKey(existing, caKey)) {
    CFRelease(existing);
    existing = NULL;
  }
  if (existing == NULL) {
    NSData *point = IPPublicKeyPoint(caKey);
    if (point.length != 65) {
      CFRelease(caKey);
      if (error != NULL) {
        *error = IPInstallError(IPStoreErrorSignerUnavailable,
                                @"Could not read the CA public key.");
      }
      return nil;
    }
    const auto *bytes = static_cast<const std::uint8_t *>(point.bytes);
    ipashelf::Bytes uncompressed(bytes, bytes + point.length);
    auto spki = ipashelf::ec_public_key_info(uncompressed);
    if (!spki) {
      CFRelease(caKey);
      if (error != NULL) {
        *error = IPInstallError(IPStoreErrorInternal,
                                @"Could not encode the CA public key.");
      }
      return nil;
    }
    ipashelf::CertificateOptions options;
    options.subject.common_name = "IPAShelf Root CA";
    options.subject.organization = "IPAShelf";
    options.is_ca = true;
    options.serial = 1;
    options.not_before = std::time(nullptr) - 24 * 60 * 60;
    options.validity_seconds = 10ULL * 365 * 24 * 60 * 60;
    auto der = ipashelf::build_ec_certificate(options, spki.value(),
                                              IPSignerForKey(caKey));
    if (!der) {
      CFRelease(caKey);
      if (error != NULL) {
        *error = IPInstallError(IPStoreErrorSignerUnavailable,
                                @"Could not sign the CA certificate.");
      }
      return nil;
    }
    caDER = [NSData dataWithBytes:der.value().data()
                           length:der.value().size()];
    if (![caDER writeToFile:caPath atomically:YES]) {
      CFRelease(caKey);
      if (error != NULL) {
        *error = IPInstallError(IPStoreErrorStorage,
                                @"Could not persist the root certificate.");
      }
      return nil;
    }
    existing = SecCertificateCreateWithData(kCFAllocatorDefault,
                                            (__bridge CFDataRef)caDER);
  }
  if (existing != NULL) {
    IPAddCertificateToKeychain(caDER, @"IPAShelf Install CA");
    CFRelease(existing);
  }
  CFRelease(caKey);
  return [[IPInstallCertificate alloc] initWithCommonName:@"IPAShelf Root CA"
                                                     der:caDER];
}

- (BOOL)startWithError:(NSError *_Nullable *_Nullable)error {
  if (_state == IPInstallServerStateStarting ||
      _state == IPInstallServerStateWaiting ||
      _state == IPInstallServerStateSending) {
    return YES;
  }
  [self setState:IPInstallServerStateStarting progress:0 message:@"Preparing"];

  IPInstallCertificate *certificate =
      [IPInstallServer prepareCertificateWithError:error];
  if (certificate == nil) {
    [self setState:IPInstallServerStateFailed progress:0
            message:@"Certificate generation failed"];
    return NO;
  }
  _certificate = certificate;
  _caPath = [IPCertificateDirectory()
      stringByAppendingPathComponent:@"root-ca.der"];

  NSError *keyError = nil;
  SecKeyRef caKey = IPCopyOrCreateKey(@"com.example.ipastore.install.ca-key",
                                      &keyError);
  SecKeyRef leafKey = IPCopyOrCreateKey(
      @"com.example.ipastore.install.leaf-key", &keyError);
  if (caKey == NULL || leafKey == NULL) {
    if (caKey != NULL) CFRelease(caKey);
    if (leafKey != NULL) CFRelease(leafKey);
    if (error != NULL) {
      *error = keyError ?: IPInstallError(IPStoreErrorSignerUnavailable,
                                          @"Could not create the leaf key.");
    }
    [self setState:IPInstallServerStateFailed progress:0
            message:@"Key generation failed"];
    return NO;
  }

  NSData *leafPoint = IPPublicKeyPoint(leafKey);
  if (leafPoint.length != 65) {
    CFRelease(caKey);
    CFRelease(leafKey);
    if (error != NULL) {
      *error = IPInstallError(IPStoreErrorSignerUnavailable,
                              @"Could not read the leaf public key.");
    }
    [self setState:IPInstallServerStateFailed progress:0
            message:@"Public key unavailable"];
    return NO;
  }
  const auto *pointBytes =
      static_cast<const std::uint8_t *>(leafPoint.bytes);
  ipashelf::Bytes point(pointBytes, pointBytes + leafPoint.length);
  auto spki = ipashelf::ec_public_key_info(point);
  if (!spki) {
    CFRelease(caKey);
    CFRelease(leafKey);
    if (error != NULL) {
      *error = IPInstallError(IPStoreErrorInternal, @"Public key encoding failed.");
    }
    [self setState:IPInstallServerStateFailed progress:0 message:@"Encoding"];
    return NO;
  }

  ipashelf::CertificateOptions options;
  options.subject.common_name = "IPAShelf Device";
  options.subject.organization = "IPAShelf";
  options.issuer.common_name = "IPAShelf Root CA";
  options.issuer.organization = "IPAShelf";
  options.issuer.country = "US";
  options.serial = 2;
  options.not_before = std::time(nullptr) - 24 * 60 * 60;
  options.validity_seconds = 2ULL * 365 * 24 * 60 * 60;
  options.alt_names.push_back(ipashelf::SubjectAltName{"localhost", std::nullopt});
  options.alt_names.push_back(
      ipashelf::SubjectAltName{"ipashelf.local", std::nullopt});
  for (const auto &address : IPLocalIPv4Bytes()) {
    options.alt_names.push_back(ipashelf::SubjectAltName{"", address});
  }
  auto leafDER = ipashelf::build_ec_certificate(options, spki.value(),
                                                IPSignerForKey(caKey));
  CFRelease(caKey);
  if (!leafDER) {
    CFRelease(leafKey);
    if (error != NULL) {
      *error = IPInstallError(IPStoreErrorSignerUnavailable,
                              @"Could not sign the leaf certificate.");
    }
    [self setState:IPInstallServerStateFailed progress:0 message:@"Signing"];
    return NO;
  }
  NSData *leafCertificate = [NSData dataWithBytes:leafDER.value().data()
                                           length:leafDER.value().size()];
  IPAddCertificateToKeychain(leafCertificate, @"IPAShelf Install Leaf");
  _identity = IPCopyIdentityForCertificate(leafCertificate);
  CFRelease(leafKey);
  if (_identity == NULL) {
    if (error != NULL) {
      *error = IPInstallError(IPStoreErrorSignerUnavailable,
                              @"iOS did not expose the certificate identity.");
    }
    [self setState:IPInstallServerStateFailed progress:0
            message:@"Identity unavailable"];
    return NO;
  }

  _host = IPPreferredHost();
  if (![self startTLSListenerWithError:error]) {
    [self setState:IPInstallServerStateFailed progress:0 message:@"TLS failed"];
    return NO;
  }
  if (![self startPlainServerWithError:error]) {
    [self setState:IPInstallServerStateFailed progress:0 message:@"HTTP failed"];
    return NO;
  }
  [self setState:IPInstallServerStateWaiting progress:0
          message:@"Waiting for the device"];
  return YES;
}

- (BOOL)startTLSListenerWithError:(NSError **_Nullable)error {
  // Network.framework hands TLS options to a configure block; the older
  // nw_tls_create_options() convenience is not part of the iOS 27 headers.
  SecIdentityRef identity = _identity;
  nw_parameters_t parameters = nw_parameters_create_secure_tcp(
      ^(nw_protocol_options_t tlsOptions) {
        sec_protocol_options_t tls =
            nw_tls_copy_sec_protocol_options(tlsOptions);
        if (tls == nil) {
          return;
        }
        sec_identity_t secIdentity = sec_identity_create(identity);
        if (secIdentity != nil) {
          // sec_identity_t and sec_protocol_options_t are ARC-managed
          // objects, so no CFRelease is needed here.
          sec_protocol_options_set_local_identity(tls, secIdentity);
        }
        sec_protocol_options_set_min_tls_protocol_version(
            tls, tls_protocol_version_TLSv12);
        sec_protocol_options_set_max_tls_protocol_version(
            tls, tls_protocol_version_TLSv13);
      },
      NW_PARAMETERS_DEFAULT_CONFIGURATION);
  if (parameters == NULL) {
    if (error != NULL) {
      *error = IPInstallError(IPStoreErrorInternal, @"Could not create parameters.");
    }
    return NO;
  }

  nw_endpoint_t endpoint =
      nw_endpoint_create_host("0.0.0.0", "0");
  nw_parameters_set_local_endpoint(parameters, endpoint);
  nw_listener_t listener = nw_listener_create(parameters);
  if (listener == NULL) {
    if (error != NULL) {
      *error = IPInstallError(IPStoreErrorInternal, @"Could not create listener.");
    }
    return NO;
  }
  nw_listener_set_queue(listener, _tlsQueue);

  __block dispatch_semaphore_t ready = dispatch_semaphore_create(0);
  __block bool failed = false;
  nw_listener_set_state_changed_handler(
      listener, ^(nw_listener_state_t state, nw_error_t nwError) {
        if (state == nw_listener_state_ready) {
          dispatch_semaphore_signal(ready);
        } else if (state == nw_listener_state_failed) {
          failed = true;
          dispatch_semaphore_signal(ready);
        }
      });

  __weak IPInstallServer *weakSelf = self;
  nw_listener_set_new_connection_handler(
      listener, ^(nw_connection_t connection) {
        IPInstallServer *strongSelf = weakSelf;
        if (strongSelf == nil) {
          nw_connection_cancel(connection);
          return;
        }
        [strongSelf acceptConnection:connection];
      });

  nw_listener_start(listener);
  const dispatch_time_t deadline =
      dispatch_time(DISPATCH_TIME_NOW, 4 * NSEC_PER_SEC);
  if (dispatch_semaphore_wait(ready, deadline) != 0 || failed) {
    nw_listener_cancel(listener);
    if (error != NULL) {
      *error = IPInstallError(IPStoreErrorNetwork,
                              @"The install listener did not start.");
    }
    return NO;
  }
  _listener = listener;
  _tlsPort = nw_listener_get_port(listener);
  if (_tlsPort <= 0) {
    if (error != NULL) {
      *error = IPInstallError(IPStoreErrorNetwork,
                              @"The install listener has no port.");
    }
    return NO;
  }

  const std::string base = "https://" + std::string(_host.UTF8String) + ":" +
                           std::to_string(_tlsPort);
  ipashelf::OtaPackage package;
  package.bundle_id = _bundleID.UTF8String ?: "";
  package.display_version = _version.UTF8String ?: "";
  package.title = _title.UTF8String ?: "";
  package.payload_path = _ipaPath.UTF8String ?: "";
  package.icon_small_path = _artworkPath.UTF8String ?: "";
  package.icon_large_path = _artworkPath.UTF8String ?: "";
  package.certificate_path = _caPath.UTF8String ?: "";
  if (_certificate != nil) {
    NSData *profile = IPProfileMobileConfig(_certificate.der);
    package.profile_data =
        std::string(static_cast<const char *>(profile.bytes),
                    profile.length);
  }
  std::error_code sizeError;
  package.payload_size = static_cast<std::uint64_t>(
      std::filesystem::file_size(package.payload_path, sizeError));
  _router = std::make_shared<ipashelf::OtaRouter>(base, package);
  return YES;
}

- (BOOL)startPlainServerWithError:(NSError **_Nullable)error {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    if (error != NULL) {
      *error = IPInstallError(IPStoreErrorNetwork, @"Could not open a socket.");
    }
    return NO;
  }
  int reuse = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  address.sin_port = 0;
  if (bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
      listen(fd, 8) != 0) {
    close(fd);
    if (error != NULL) {
      *error = IPInstallError(IPStoreErrorNetwork,
                              @"Could not bind the certificate server.");
    }
    return NO;
  }
  socklen_t length = sizeof(address);
  getsockname(fd, reinterpret_cast<sockaddr *>(&address), &length);
  _plainSocket = fd;
  _plainPort = ntohs(address.sin_port);

  const std::string tlsBase = "https://" + std::string(_host.UTF8String) + ":" +
                              std::to_string(_tlsPort);
  const std::string plainBase = "http://" + std::string(_host.UTF8String) + ":" +
                                std::to_string(_plainPort);
  ipashelf::OtaPackage package;
  package.bundle_id = _bundleID.UTF8String ?: "";
  package.display_version = _version.UTF8String ?: "";
  package.title = _title.UTF8String ?: "";
  package.payload_path = _ipaPath.UTF8String ?: "";
  package.icon_small_path = _artworkPath.UTF8String ?: "";
  package.icon_large_path = _artworkPath.UTF8String ?: "";
  package.certificate_path = _caPath.UTF8String ?: "";
  if (_certificate != nil) {
    NSData *profile = IPProfileMobileConfig(_certificate.der);
    package.profile_data =
        std::string(static_cast<const char *>(profile.bytes),
                    profile.length);
  }
  {
    std::lock_guard<std::mutex> lock(_routerMutex);
    _plainRouter = std::make_shared<ipashelf::OtaRouter>(plainBase, package);
    _plainRouter->set_page_links(_router->install_url(),
                                 plainBase + _plainRouter->routes().profile);
  }

  __weak IPInstallServer *weakSelf = self;
  const int listenFD = fd;
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
    IPInstallServer *strongSelf = weakSelf;
    if (strongSelf == nil) {
      close(listenFD);
      return;
    }
    while (!strongSelf->_stopping.load()) {
      const int client = accept(listenFD, nullptr, nullptr);
      if (client < 0) break;
      std::shared_ptr<ipashelf::OtaRouter> router;
      {
        std::lock_guard<std::mutex> lock(strongSelf->_routerMutex);
        router = strongSelf->_plainRouter;
      }
      if (router) {
        ipashelf::CancellationSource cancellation;
        const auto status = ipashelf::serve_http_connection(
            client, *router, cancellation.token());
        (void)status;
      }
      close(client);
    }
  });
  return YES;
}

#pragma mark - Connections

- (void)acceptConnection:(nw_connection_t)connection {
  if (_stopping.load()) {
    nw_connection_cancel(connection);
    return;
  }
  IPInstallConnection *handler =
      [[IPInstallConnection alloc] initWithConnection:connection
                                              router:_router
                                              server:self];
  [_connections addObject:handler];
  [handler start];
}

- (void)connectionDidFinish:(IPInstallConnection *)connection {
  dispatch_async(_tlsQueue, ^{
    [self->_connections removeObject:connection];
  });
}

- (void)notePayloadProgress:(double)fraction message:(nullable NSString *)message {
  const NSTimeInterval now = [NSDate timeIntervalSinceReferenceDate];
  if (fraction < 1.0 && now - _lastProgressReport < 0.1) return;
  _lastProgressReport = now;
  [self setState:IPInstallServerStateSending
        progress:fraction
         message:message ?: @"Sending the package"];
}

- (void)reportFailure:(NSError *)error {
  [self setState:IPInstallServerStateFailed progress:0
          message:error.localizedDescription ?: @"Installation failed"];
}

#pragma mark - State

- (void)setState:(IPInstallServerState)state
        progress:(double)progress
         message:(NSString *)message {
  _state = state;
  void (^handler)(IPInstallServerState, double, NSString *) = _stateHandler;
  if (handler == nil) return;
  dispatch_async(dispatch_get_main_queue(), ^{
    handler(state, progress, message ?: @"");
  });
}

- (nullable NSString *)manifestURL {
  if (!_router) return nil;
  return [NSString stringWithUTF8String:_router->manifest_url().c_str()];
}

- (nullable NSString *)installURL {
  if (!_router) return nil;
  return [NSString stringWithUTF8String:_router->install_url().c_str()];
}

- (nullable NSString *)pageURL {
  if (_plainPort <= 0) return nil;
  return [NSString stringWithFormat:@"http://%@:%d/", _host, _plainPort];
}

- (nullable NSString *)certificateURL {
  if (_plainPort <= 0) return nil;
  return [NSString stringWithFormat:@"http://%@:%d/ca.cer", _host, _plainPort];
}

- (nullable NSString *)profileURL {
  if (_plainPort <= 0) return nil;
  return [NSString stringWithFormat:@"http://%@:%d/profile.mobileconfig",
         _host, _plainPort];
}

- (void)stop {
  _stopping.store(true);
  if (_listener != NULL) {
    nw_listener_cancel(_listener);
    _listener = NULL;
  }
  __block NSArray<IPInstallConnection *> *connections = nil;
  dispatch_sync(_tlsQueue, ^{
    connections = [self->_connections copy];
  });
  for (IPInstallConnection *connection in connections) {
    [connection cancel];
  }
  dispatch_sync(_tlsQueue, ^{
    [self->_connections removeAllObjects];
  });
  if (_plainSocket >= 0) {
    shutdown(_plainSocket, SHUT_RDWR);
    close(_plainSocket);
    _plainSocket = -1;
  }
  if (_state != IPInstallServerStateFailed) {
    [self setState:IPInstallServerStateStopped progress:0 message:@"Stopped"];
  }
}

@end
