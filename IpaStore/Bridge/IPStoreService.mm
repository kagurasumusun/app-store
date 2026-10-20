// SPDX-License-Identifier: MIT

#import <Foundation/Foundation.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#import "IPInternal.h"

#include "ipashelf/crypto.hpp"
#include "ipashelf/ipa.hpp"
#include "ipashelf/json.hpp"

NSErrorDomain const IPStoreErrorDomain = @"com.example.ipastore.error";

ipashelf::Platform IPCorePlatform(IPStorePlatform platform) {
  switch (platform) {
    case IPStorePlatformIPad:
      return ipashelf::Platform::kIPad;
    case IPStorePlatformMac:
      return ipashelf::Platform::kMacOS;
    case IPStorePlatformTV:
      return ipashelf::Platform::kAppleTV;
    case IPStorePlatformWatch:
      return ipashelf::Platform::kWatchOS;
    case IPStorePlatformVision:
      return ipashelf::Platform::kVisionOS;
    case IPStorePlatformAny:
      return ipashelf::Platform::kUnspecified;
    case IPStorePlatformIPhone:
    default:
      return ipashelf::Platform::kIPhone;
  }
}

namespace {

IPStoreErrorCode PublicCode(ipashelf::ErrorCode code) {
  switch (code) {
    case ipashelf::ErrorCode::kNone:
      return IPStoreErrorInternal;
    case ipashelf::ErrorCode::kInvalidArgument:
      return IPStoreErrorInvalidResponse;
    case ipashelf::ErrorCode::kCancelled:
      return IPStoreErrorCancelled;
    case ipashelf::ErrorCode::kNetwork:
      return IPStoreErrorNetwork;
    case ipashelf::ErrorCode::kHttpStatus:
      return IPStoreErrorHTTPStatus;
    case ipashelf::ErrorCode::kParse:
      return IPStoreErrorInvalidResponse;
    case ipashelf::ErrorCode::kUnauthorized:
      return IPStoreErrorUnauthorized;
    case ipashelf::ErrorCode::kTokenExpired:
      return IPStoreErrorTokenExpired;
    case ipashelf::ErrorCode::kTwoFactorRequired:
      return IPStoreErrorTwoFactorRequired;
    case ipashelf::ErrorCode::kLicenseRequired:
      return IPStoreErrorLicenseRequired;
    case ipashelf::ErrorCode::kTemporarilyUnavailable:
      return IPStoreErrorUnavailable;
    case ipashelf::ErrorCode::kNoOffer:
      return IPStoreErrorUnavailable;
    case ipashelf::ErrorCode::kAppNotFound:
      return IPStoreErrorAppNotFound;
    case ipashelf::ErrorCode::kUnsupported:
      return IPStoreErrorUnsupported;
    case ipashelf::ErrorCode::kSignerUnavailable:
      return IPStoreErrorSignerUnavailable;
    case ipashelf::ErrorCode::kStorage:
      return IPStoreErrorStorage;
    case ipashelf::ErrorCode::kIntegrity:
      return IPStoreErrorIntegrity;
    case ipashelf::ErrorCode::kInternal:
      return IPStoreErrorInternal;
  }
  return IPStoreErrorInternal;
}

NSError *MakeError(const ipashelf::Error &error) {
  NSMutableDictionary<NSString *, id> *info = [NSMutableDictionary dictionary];
  NSString *message = [NSString stringWithUTF8String:error.message.c_str()];
  info[NSLocalizedDescriptionKey] = message.length > 0 ? message : @"The App Store request failed.";
  if (error.http_status != 0) info[@"HTTPStatus"] = @(error.http_status);
  if (!error.apple_failure_type.empty()) {
    info[@"AppleFailureType"] = [NSString stringWithUTF8String:error.apple_failure_type.c_str()];
  }
  return [NSError errorWithDomain:IPStoreErrorDomain
                             code:PublicCode(error.code)
                         userInfo:info];
}

NSError *MakeErrorCode(IPStoreErrorCode code, NSString *message) {
  return [NSError errorWithDomain:IPStoreErrorDomain
                             code:code
                         userInfo:@{NSLocalizedDescriptionKey : message}];
}

NSString *TrimmedRegion(NSString *region, IPStoreAccount *account) {
  NSString *value = region.length > 0 ? region : account.region;
  if (value.length == 0) value = @"US";
  return value.lowercaseString;
}

NSString *SafeFilename(NSString *value) {
  NSMutableString *name = [value mutableCopy] ?: [NSMutableString string];
  for (NSString *bad in @[@"/", @":", @"\\", @"\0"]) {
    [name replaceOccurrencesOfString:bad
                          withString:@"-"
                             options:0
                               range:NSMakeRange(0, name.length)];
  }
  return name;
}

}  // namespace

// ---------------------------------------------------------------------------
// Download centre
// ---------------------------------------------------------------------------

@interface IPDownloadContext : NSObject
@property (nonatomic, copy) void (^progress)(double, long long, long long);
@property (nonatomic, copy) void (^completion)(NSString *_Nullable, NSError *_Nullable);
@property (nonatomic, copy, nullable) NSString *downloadedPath;
@property (nonatomic, strong, nullable) NSError *failureError;
@property (nonatomic, copy) NSString *resumeKey;
@property (nonatomic, strong) NSURLSessionDownloadTask *task;
@end

@implementation IPDownloadContext
@end

@interface IPDownloadCenter : NSObject <NSURLSessionDownloadDelegate>
@property (class, nonatomic, readonly) IPDownloadCenter *shared;
/// Downloads `url`, resuming from any partial data previously saved under
/// `resumeKey` (for example "<appID>-<externalVersionID>"). Partial data
/// survives cancellation and app relaunches; it is discarded once the
/// download completes or the server rejects the resumed request.
- (void)downloadURL:(NSURL *)url
             headers:(NSDictionary<NSString *, NSString *> *)headers
           resumeKey:(NSString *)resumeKey
            progress:(void (^)(double, long long, long long))progress
          completion:(void (^)(NSString *_Nullable, NSError *_Nullable))completion;
- (void)cancelAll;
@end

@interface IPDownloadCenter ()
- (nullable IPDownloadContext *)contextForTask:(NSURLSessionTask *)task;
- (void)finishTask:(NSURLSessionTask *)task error:(nullable NSError *)error;
@end

@implementation IPDownloadCenter {
  NSURLSession *_session;
  NSOperationQueue *_delegateQueue;
  NSMutableDictionary<NSNumber *, IPDownloadContext *> *_contexts;
  NSLock *_lock;
}

+ (NSString *)resumeDataDirectory {
  NSString *caches = [NSSearchPathForDirectoriesInDomains(
      NSCachesDirectory, NSUserDomainMask, YES) firstObject];
  NSString *directory =
      [caches stringByAppendingPathComponent:@"IPDownloadResume"];
  [[NSFileManager defaultManager] createDirectoryAtPath:directory
                            withIntermediateDirectories:YES
                                             attributes:nil
                                                  error:nil];
  return directory;
}

+ (NSString *)resumeDataPathForKey:(NSString *)key {
  return [[self resumeDataDirectory]
      stringByAppendingPathComponent:
          [SafeFilename(key) stringByAppendingPathExtension:@"resumedata"]];
}

- (nullable NSData *)resumeDataForKey:(NSString *)key {
  if (key.length == 0) return nil;
  return [NSData dataWithContentsOfFile:[IPDownloadCenter resumeDataPathForKey:key]];
}

- (void)storeResumeData:(NSData *)data forKey:(NSString *)key {
  if (key.length == 0 || data.length == 0) return;
  [data writeToFile:[IPDownloadCenter resumeDataPathForKey:key] atomically:YES];
}

- (void)discardResumeDataForKey:(NSString *)key {
  if (key.length == 0) return;
  [[NSFileManager defaultManager]
      removeItemAtPath:[IPDownloadCenter resumeDataPathForKey:key]
                 error:nil];
}

+ (IPDownloadCenter *)shared {
  static IPDownloadCenter *centre;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    centre = [[IPDownloadCenter alloc] init];
  });
  return centre;
}

- (instancetype)init {
  self = [super init];
  if (self != nil) {
    _contexts = [NSMutableDictionary dictionary];
    _lock = [[NSLock alloc] init];
    _delegateQueue = [[NSOperationQueue alloc] init];
    _delegateQueue.maxConcurrentOperationCount = 1;
    _delegateQueue.name = @"com.example.ipastore.downloads";

    NSURLSessionConfiguration *configuration =
        [NSURLSessionConfiguration defaultSessionConfiguration];
    configuration.timeoutIntervalForRequest = 60;
    configuration.timeoutIntervalForResource = 60 * 60 * 6;
    configuration.HTTPMaximumConnectionsPerHost = 2;
    _session = [NSURLSession sessionWithConfiguration:configuration
                                             delegate:self
                                        delegateQueue:_delegateQueue];
  }
  return self;
}

- (void)downloadURL:(NSURL *)url
             headers:(NSDictionary<NSString *, NSString *> *)headers
           resumeKey:(NSString *)resumeKey
            progress:(void (^)(double, long long, long long))progress
          completion:(void (^)(NSString *_Nullable, NSError *_Nullable))completion {
  NSURLSessionDownloadTask *task = nil;
  NSData *resumeData = [self resumeDataForKey:resumeKey];
  if (resumeData.length > 0) {
    task = [_session downloadTaskWithResumeData:resumeData];
  }
  if (task == nil) {
    NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:url];
    request.HTTPMethod = @"GET";
    for (NSString *name in headers) {
      [request setValue:headers[name] forHTTPHeaderField:name];
    }
    task = [_session downloadTaskWithRequest:request];
  }
  IPDownloadContext *context = [[IPDownloadContext alloc] init];
  context.progress = progress;
  context.completion = completion;
  context.resumeKey = resumeKey ?: @"";
  context.task = task;
  [_lock lock];
  _contexts[@(task.taskIdentifier)] = context;
  [_lock unlock];
  [task resume];
}

- (IPDownloadContext *)contextForTask:(NSURLSessionTask *)task {
  [_lock lock];
  IPDownloadContext *context = _contexts[@(task.taskIdentifier)];
  [_lock unlock];
  return context;
}

- (void)finishTask:(NSURLSessionTask *)task error:(NSError *)error {
  [_lock lock];
  IPDownloadContext *context = _contexts[@(task.taskIdentifier)];
  [_contexts removeObjectForKey:@(task.taskIdentifier)];
  [_lock unlock];
  if (context == nil) return;

  NSError *effectiveError = context.failureError ?: error;
  if (effectiveError == nil && context.downloadedPath.length > 0) {
    [self discardResumeDataForKey:context.resumeKey];
    if (context.completion != nil) context.completion(context.downloadedPath, nil);
    return;
  }

  if (effectiveError == nil) {
    effectiveError = MakeErrorCode(IPStoreErrorStorage,
                                   @"The download finished without a file.");
  }
  // Interrupted transfers may carry fresh resume data; keep it so the next
  // attempt continues where this one stopped. A completed HTTP error means
  // the saved state (or its signed URL) is stale, so drop it instead.
  if (context.failureError == nil) {
    NSData *resumeData = error.userInfo[NSURLSessionDownloadTaskResumeData];
    if ([resumeData isKindOfClass:[NSData class]]) {
      [self storeResumeData:resumeData forKey:context.resumeKey];
    }
  } else {
    [self discardResumeDataForKey:context.resumeKey];
  }
  if (context.completion == nil) return;
  if (effectiveError.code == NSURLErrorCancelled &&
      [effectiveError.domain isEqualToString:NSURLErrorDomain]) {
    context.completion(nil, MakeErrorCode(IPStoreErrorCancelled,
                                          @"The download was cancelled."));
  } else {
    context.completion(nil, effectiveError);
  }
}

- (void)URLSession:(NSURLSession *)session
                 downloadTask:(NSURLSessionDownloadTask *)downloadTask
                 didWriteData:(int64_t)bytesWritten
            totalBytesWritten:(int64_t)totalBytesWritten
    totalBytesExpectedToWrite:(int64_t)totalBytesExpectedToWrite {
  IPDownloadContext *context = [self contextForTask:downloadTask];
  if (context.progress == nil) return;
  const double fraction =
      totalBytesExpectedToWrite > 0
          ? static_cast<double>(totalBytesWritten) /
                static_cast<double>(totalBytesExpectedToWrite)
          : 0.0;
  context.progress(fraction, totalBytesWritten, totalBytesExpectedToWrite);
}

- (void)URLSession:(NSURLSession *)session
                 downloadTask:(NSURLSessionDownloadTask *)downloadTask
    didFinishDownloadingToURL:(NSURL *)location {
  IPDownloadContext *context = [self contextForTask:downloadTask];
  if (context == nil) return;
  // Download tasks happily save an HTTP error body as the "file"; only a
  // 200 (fresh) or 206 (resumed) response is a package.
  if ([downloadTask.response isKindOfClass:[NSHTTPURLResponse class]]) {
    const NSInteger status =
        ((NSHTTPURLResponse *)downloadTask.response).statusCode;
    if (status != 200 && status != 206) {
      context.failureError = [NSError
          errorWithDomain:IPStoreErrorDomain
                     code:IPStoreErrorHTTPStatus
                 userInfo:@{
                   NSLocalizedDescriptionKey : [NSString
                       stringWithFormat:@"The package server returned HTTP %ld.",
                                        (long)status],
                   @"HTTPStatus" : @(status),
                 }];
      context.downloadedPath = nil;
      return;
    }
  }
  NSString *name = [NSString stringWithFormat:@"ipastore-%lu.ipa",
                                             (unsigned long)downloadTask.taskIdentifier];
  NSString *destination = [NSTemporaryDirectory() stringByAppendingPathComponent:name];
  NSFileManager *manager = [NSFileManager defaultManager];
  [manager removeItemAtPath:destination error:nil];
  NSError *error = nil;
  if (![manager moveItemAtURL:location toURL:[NSURL fileURLWithPath:destination]
                        error:&error]) {
    context.downloadedPath = nil;
    return;
  }
  context.downloadedPath = destination;
}

- (void)URLSession:(NSURLSession *)session
                    task:(NSURLSessionTask *)task
    didCompleteWithError:(NSError *)error {
  [self finishTask:task error:error];
}

- (void)URLSession:(NSURLSession *)session
                 downloadTask:(NSURLSessionDownloadTask *)downloadTask
    didResumeAtOffset:(int64_t)fileOffset
          expectedTotalBytes:(int64_t)expectedTotalBytes {
  IPDownloadContext *context = [self contextForTask:downloadTask];
  if (context.progress == nil) return;
  const double fraction =
      expectedTotalBytes > 0
          ? static_cast<double>(fileOffset) /
                static_cast<double>(expectedTotalBytes)
          : 0.0;
  context.progress(fraction, fileOffset, expectedTotalBytes);
}

- (void)cancelAll {
  [_lock lock];
  NSArray<IPDownloadContext *> *contexts = _contexts.allValues;
  [_lock unlock];
  for (IPDownloadContext *context in contexts) {
    // Preserve partial data so a later attempt can resume instead of
    // starting the (potentially multi-gigabyte) package over.
    [context.task cancelByProducingResumeData:^(NSData *_Nullable resumeData) {
      if (resumeData.length > 0) {
        [self storeResumeData:resumeData forKey:context.resumeKey];
      }
    }];
  }
}

@end

// ---------------------------------------------------------------------------
// Service
// ---------------------------------------------------------------------------

static NSDictionary<NSString *, id> *SidecarForApp(IPStoreApp *app,
                                                   ipashelf::DownloadOffer &offer,
                                                   NSString *path,
                                                   long long size) {
  return @{
    @"appID" : @(app.appID),
    @"bundleID" : app.bundleID ?: @"",
    @"name" : app.name ?: @"",
    @"version" : app.version ?: @"",
    @"externalVersionID" :
        [NSString stringWithUTF8String:offer.external_version_id.c_str()] ?: @"",
    @"downloadedAt" : @([[NSDate date] timeIntervalSince1970]),
    @"size" : @(size),
    @"file" : path.lastPathComponent ?: @"",
  };
}

@interface IPStoreService ()
- (void)finishDownloadAtPath:(NSString *)path
                         app:(IPStoreApp *)app
                     account:(IPStoreAccount *)account
                       offer:(ipashelf::DownloadOffer)offer
                     coreApp:(ipashelf::App)coreApp
                 coreAccount:(ipashelf::Account)coreAccount
                     version:(std::string)version
                    platform:(ipashelf::Platform)platform
                  completion:(void (^)(NSString *_Nullable, NSError *_Nullable))completion;
@end

/// Best-effort download of the artwork Apple reports with the download item
/// (injected into the finished IPA as iTunesArtwork). A failure only costs
/// the icon, never the package, so it is not fatal.
static std::optional<ipashelf::Bytes> FetchArtwork(const std::string &url) {
  if (url.empty()) return std::nullopt;
  ipashelf::HttpRequest request;
  request.method = "GET";
  request.url = url;
  request.headers.set(
      "User-Agent", std::string(ipashelf::kDefaultUserAgent));
  ipashelf::CancellationSource source;
  auto response = IPSharedHTTPTransport().send(request, source.token());
  if (!response) return std::nullopt;
  if (response.value().status != 200 || response.value().body.empty()) {
    return std::nullopt;
  }
  return std::move(response.value().body);
}

static ipashelf::StoreClient MakeStoreClient(void) {
  ipashelf::StoreConfiguration configuration;
  configuration.sleep = [](std::chrono::milliseconds duration) {
    std::this_thread::sleep_for(duration);
  };
  // No SAP backend factory: the core falls back to its complete
  // pure-software SAP v200 signer, which runs inside the app sandbox
  // without any private framework.

  // The purchase-history (DAAP) requests carry the device timezone, like
  // the real Configurator client does.
  NSTimeZone *timeZone = [NSTimeZone localTimeZone];
  configuration.time_zone_name = timeZone.name.UTF8String ?: "UTC";
  configuration.utc_offset_seconds =
      static_cast<int>([timeZone secondsFromGMT]);

  // kbsync blobs for the ent/download endpoint are generated by FairPlay
  // machine code upstream (there is no portable reimplementation), so no
  // generator is installed and downloads use the legacy
  // volumeStore/redownload/updateProduct chain. The cache hooks are wired
  // to the Keychain so a future generator drops in without changes here.
  configuration.kbsync_cache_load = []() -> std::optional<std::string> {
    NSString *value = [IPKeychain stringForKey:@"kbsync-v1"];
    if (value == nil) return std::nullopt;
    return std::string(value.UTF8String ?: "");
  };
  configuration.kbsync_cache_store = [](std::string_view entry) {
    NSString *value = [[NSString alloc] initWithBytes:entry.data()
                                               length:entry.size()
                                             encoding:NSUTF8StringEncoding];
    if (value != nil) {
      [IPKeychain setString:value forKey:@"kbsync-v1" error:nil];
    }
  };
  configuration.kbsync_cache_remove = []() {
    [IPKeychain removeKey:@"kbsync-v1" error:nil];
  };
  return ipashelf::StoreClient(IPSharedHTTPTransport(), std::move(configuration));
}

@implementation IPStoreService {
  IPAccountStore *_accountStore;
  dispatch_queue_t _queue;
}

+ (IPStoreService *)shared {
  static IPStoreService *service;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    service = [[IPStoreService alloc] init];
  });
  return service;
}

- (instancetype)init {
  self = [super init];
  if (self != nil) {
    _accountStore = [[IPAccountStore alloc] init];
    _queue = dispatch_queue_create("com.example.ipastore.service",
                                   DISPATCH_QUEUE_SERIAL);
    (void)IPHardwareIdentity();
  }
  return self;
}

- (NSArray<IPStoreAccount *> *)accounts {
  return _accountStore.accounts;
}

- (nullable IPStoreAccount *)activeAccount {
  return _accountStore.activeAccount;
}

- (NSString *)deviceIdentifier {
  const std::string guid = IPHardwareIdentity().guid();
  return [NSString stringWithUTF8String:guid.c_str()] ?: @"";
}

- (void)setActiveAccount:(nullable IPStoreAccount *)account {
  _accountStore.activeAccount = account;
}

- (void)removeAccount:(IPStoreAccount *)account {
  [_accountStore removeAccount:account];
}

- (void)loginWithEmail:(NSString *)email
              password:(NSString *)password
              authCode:(nullable NSString *)authCode
                region:(NSString *)region
            completion:(void (^)(IPStoreLoginResult *_Nullable,
                                 NSError *_Nullable))completion {
  dispatch_async(_queue, ^{
    ipashelf::StoreClient client = MakeStoreClient();
    ipashelf::LoginRequest request;
    request.email = email.UTF8String ?: "";
    request.password = password.UTF8String ?: "";
    request.auth_code = authCode.UTF8String ?: "";
    request.region = region.length > 0 ? region.UTF8String : "US";
    request.hardware = IPHardwareIdentity();

    ipashelf::CancellationSource source;
    auto result = client.login(request, source.token());
    dispatch_async(dispatch_get_main_queue(), ^{
      if (!result) {
        completion(nil, MakeError(result.error()));
        return;
      }
      if (result.value().two_factor_required) {
        completion([[IPStoreLoginResult alloc] initWithAccount:nil
                                            twoFactorRequired:YES],
                   nil);
        return;
      }
      IPStoreAccount *account =
          [[IPStoreAccount alloc] initWithAccount:result.value().account];
      [self->_accountStore upsertAccount:account];
      [self->_accountStore setActiveAccount:account];
      completion([[IPStoreLoginResult alloc] initWithAccount:account
                                          twoFactorRequired:NO],
                 nil);
    });
  });
}

- (void)refreshAccount:(IPStoreAccount *)account
              authCode:(nullable NSString *)authCode
            completion:(void (^)(IPStoreLoginResult *_Nullable,
                                 NSError *_Nullable))completion {
  dispatch_async(_queue, ^{
    // Rotate the *stored* account: the caller may hold a stale handle.
    IPStoreAccount *stored =
        [self->_accountStore accountForEmail:account.email] ?: account;
    const ipashelf::Account coreAccount = stored.coreAccount;
    if (coreAccount.password.empty()) {
      dispatch_async(dispatch_get_main_queue(), ^{
        completion(nil, MakeErrorCode(IPStoreErrorUnauthorized,
                                      @"This account has no saved password. "
                                      @"Sign in again to refresh it."));
      });
      return;
    }
    ipashelf::StoreClient client = MakeStoreClient();
    ipashelf::LoginRequest request;
    request.email = coreAccount.email;
    request.password = coreAccount.password;
    request.auth_code = authCode.UTF8String ?: "";
    request.region = coreAccount.region.empty() ? "US" : coreAccount.region;
    request.hardware = IPHardwareIdentity();

    ipashelf::CancellationSource source;
    auto result = client.login(request, source.token());
    dispatch_async(dispatch_get_main_queue(), ^{
      if (!result) {
        completion(nil, MakeError(result.error()));
        return;
      }
      if (result.value().two_factor_required) {
        completion([[IPStoreLoginResult alloc] initWithAccount:nil
                                            twoFactorRequired:YES],
                   nil);
        return;
      }
      IPStoreAccount *refreshed =
          [[IPStoreAccount alloc] initWithAccount:result.value().account];
      [self->_accountStore upsertAccount:refreshed];
      completion([[IPStoreLoginResult alloc] initWithAccount:refreshed
                                          twoFactorRequired:NO],
                 nil);
    });
  });
}

- (void)purchaseApp:(IPStoreApp *)app
            account:(IPStoreAccount *)account
           platform:(IPStorePlatform)platform
         completion:(void (^)(NSError *_Nullable))completion {
  dispatch_async(_queue, ^{
    ipashelf::StoreClient client = MakeStoreClient();
    ipashelf::App coreApp;
    coreApp.id = app.appID;
    coreApp.bundle_id = app.bundleID.UTF8String ?: "";
    coreApp.name = app.name.UTF8String ?: "";
    coreApp.price = app.price;
    ipashelf::CancellationSource source;
    auto status = client.purchase(account.coreAccount, coreApp,
                                  IPCorePlatform(platform), source.token());
    dispatch_async(dispatch_get_main_queue(), ^{
      completion(status ? nil : MakeError(status.error()));
    });
  });
}

- (void)searchForTerm:(NSString *)term
               region:(NSString *)region
             platform:(IPStorePlatform)platform
                limit:(NSInteger)limit
           completion:(void (^)(NSArray<IPStoreApp *> *_Nullable,
                                NSError *_Nullable))completion {
  dispatch_async(_queue, ^{
    ipashelf::StoreClient client = MakeStoreClient();
    ipashelf::CancellationSource source;
    auto result = client.search(term.UTF8String ?: "",
                                TrimmedRegion(region, self->_accountStore.activeAccount).UTF8String ?: "us",
                                IPCorePlatform(platform),
                                static_cast<int>(std::clamp<NSInteger>(limit, 1, 200)),
                                source.token());
    dispatch_async(dispatch_get_main_queue(), ^{
      if (!result) {
        completion(nil, MakeError(result.error()));
        return;
      }
      NSMutableArray<IPStoreApp *> *apps = [NSMutableArray array];
      for (const auto &app : result.value().results) {
        [apps addObject:[[IPStoreApp alloc] initWithApp:app]];
      }
      completion(apps, nil);
    });
  });
}

+ (NSArray<IPStoreStorefront *> *)storefronts {
  NSMutableArray<IPStoreStorefront *> *result = [NSMutableArray array];
  for (const auto &entry : ipashelf::storefronts()) {
    [result addObject:[[IPStoreStorefront alloc] initWithStorefront:entry]];
  }
  return result;
}

- (void)searchWithFilter:(IPStoreSearchFilter *)filter
              completion:(void (^)(NSArray<IPStoreApp *> *_Nullable,
                                   NSError *_Nullable))completion {
  dispatch_async(_queue, ^{
    ipashelf::StoreClient client = MakeStoreClient();
    ipashelf::SearchQuery query = [filter coreQuery];
    if (query.region.empty()) {
      query.region = TrimmedRegion(filter.region, self->_accountStore.activeAccount).UTF8String ?: "us";
    }
    ipashelf::CancellationSource source;
    auto result = client.search(query, source.token());
    dispatch_async(dispatch_get_main_queue(), ^{
      if (!result) {
        completion(nil, MakeError(result.error()));
        return;
      }
      NSMutableArray<IPStoreApp *> *apps = [NSMutableArray array];
      for (const auto &app : result.value().results) {
        [apps addObject:[[IPStoreApp alloc] initWithApp:app]];
      }
      completion(apps, nil);
    });
  });
}

- (void)lookupAppID:(long long)appID
           bundleID:(nullable NSString *)bundleID
             region:(NSString *)region
           platform:(IPStorePlatform)platform
         completion:(void (^)(IPStoreApp *_Nullable,
                              NSError *_Nullable))completion {
  dispatch_async(_queue, ^{
    ipashelf::StoreClient client = MakeStoreClient();
    ipashelf::CancellationSource source;
    auto result = client.lookup(appID,
                                bundleID.UTF8String ?: "",
                                TrimmedRegion(region, self->_accountStore.activeAccount).UTF8String ?: "us",
                                IPCorePlatform(platform), source.token());
    dispatch_async(dispatch_get_main_queue(), ^{
      if (!result) {
        completion(nil, MakeError(result.error()));
        return;
      }
      completion([[IPStoreApp alloc] initWithApp:result.value()], nil);
    });
  });
}

- (void)latestExternalVersionForApp:(IPStoreApp *)app
                             region:(NSString *)region
                           platform:(IPStorePlatform)platform
                         completion:(void (^)(NSString *_Nullable,
                                              NSError *_Nullable))completion {
  dispatch_async(_queue, ^{
    ipashelf::StoreClient client = MakeStoreClient();
    ipashelf::App coreApp;
    coreApp.id = app.appID;
    coreApp.bundle_id = app.bundleID.UTF8String ?: "";
    ipashelf::CancellationSource source;
    auto result = client.latest_external_version_id(
        coreApp,
        TrimmedRegion(region, self->_accountStore.activeAccount).UTF8String ?: "us",
        IPCorePlatform(platform), source.token());
    dispatch_async(dispatch_get_main_queue(), ^{
      if (!result) {
        completion(nil, MakeError(result.error()));
        return;
      }
      completion([NSString stringWithUTF8String:result.value().c_str()], nil);
    });
  });
}

- (void)listVersionsForApp:(IPStoreApp *)app
                   account:(IPStoreAccount *)account
                  platform:(IPStorePlatform)platform
                completion:(void (^)(NSArray<IPStoreVersion *> *_Nullable,
                                     NSError *_Nullable))completion {
  dispatch_async(_queue, ^{
    ipashelf::StoreClient client = MakeStoreClient();
    ipashelf::App coreApp;
    coreApp.id = app.appID;
    coreApp.bundle_id = app.bundleID.UTF8String ?: "";
    ipashelf::CancellationSource source;
    auto result = client.version_list(
        account.coreAccount, coreApp, IPCorePlatform(platform), source.token());
    dispatch_async(dispatch_get_main_queue(), ^{
      if (!result) {
        completion(nil, MakeError(result.error()));
        return;
      }
      NSMutableArray<IPStoreVersion *> *versions = [NSMutableArray array];
      for (const auto &identifier : result.value().identifiers) {
        [versions addObject:[[IPStoreVersion alloc]
                                initWithExternalID:[NSString stringWithUTF8String:
                                                               identifier.c_str()]
                                    displayVersion:@""
                                       releaseDate:nil
                                            latest:identifier ==
                                                   result.value().latest]];
      }
      completion(versions, nil);
    });
  });
}

- (void)versionMetadataForApp:(IPStoreApp *)app
                      account:(IPStoreAccount *)account
            externalVersionID:(NSString *)externalVersionID
                     platform:(IPStorePlatform)platform
                   completion:(void (^)(IPStoreVersionMetadata *_Nullable,
                                        NSError *_Nullable))completion {
  dispatch_async(_queue, ^{
    ipashelf::StoreClient client = MakeStoreClient();
    ipashelf::App coreApp;
    coreApp.id = app.appID;
    coreApp.bundle_id = app.bundleID.UTF8String ?: "";
    ipashelf::CancellationSource source;
    auto result = client.version_metadata(
        account.coreAccount, coreApp, externalVersionID.UTF8String ?: "",
        IPCorePlatform(platform), source.token());
    dispatch_async(dispatch_get_main_queue(), ^{
      if (!result) {
        completion(nil, MakeError(result.error()));
        return;
      }
      completion([[IPStoreVersionMetadata alloc] initWithMetadata:result.value()],
                 nil);
    });
  });
}

- (void)ownedAppsForAccount:(IPStoreAccount *)account
                   platform:(IPStorePlatform)platform
                       page:(NSInteger)page
                      limit:(NSInteger)limit
                 completion:(void (^)(IPStoreOwnedAppsPage *_Nullable,
                                      NSError *_Nullable))completion {
  dispatch_async(_queue, ^{
    if (account == nil) {
      dispatch_async(dispatch_get_main_queue(), ^{
        completion(nil, MakeErrorCode(IPStoreErrorUnauthorized,
                                      @"Sign in before listing purchased apps."));
      });
      return;
    }
    ipashelf::StoreClient client = MakeStoreClient();
    ipashelf::CancellationSource source;
    std::optional<ipashelf::Platform> filter;
    if (platform != IPStorePlatformAny) filter = IPCorePlatform(platform);
    auto result = client.owned_apps(
        account.coreAccount, filter, static_cast<int>(page),
        static_cast<int>(limit), source.token());
    dispatch_async(dispatch_get_main_queue(), ^{
      if (!result) {
        completion(nil, MakeError(result.error()));
        return;
      }
      completion([[IPStoreOwnedAppsPage alloc] initWithPage:result.value()], nil);
    });
  });
}

- (void)downloadApp:(IPStoreApp *)app
            account:(IPStoreAccount *)account
   externalVersionID:(nullable NSString *)externalVersionID
           platform:(IPStorePlatform)platform
           progress:(void (^_Nullable)(double, long long, long long))progress
         completion:(void (^)(NSString *_Nullable,
                              NSError *_Nullable))completion {
  dispatch_async(_queue, ^{
    if (account == nil) {
      dispatch_async(dispatch_get_main_queue(), ^{
        completion(nil, MakeErrorCode(IPStoreErrorUnauthorized,
                                      @"Sign in before downloading an app."));
      });
      return;
    }
    ipashelf::StoreClient client = MakeStoreClient();
    ipashelf::CancellationSource source;
    ipashelf::Account coreAccount = account.coreAccount;
    ipashelf::App coreApp;
    coreApp.id = app.appID;
    coreApp.bundle_id = app.bundleID.UTF8String ?: "";
    coreApp.name = app.name.UTF8String ?: "";
    coreApp.price = app.price;

    const std::string version =
        externalVersionID.length > 0 ? std::string(externalVersionID.UTF8String) : std::string();
    auto offer = client.download_offer(coreAccount, coreApp, version,
                                       IPCorePlatform(platform), source.token());
    if (!offer && offer.error().code == ipashelf::ErrorCode::kLicenseRequired) {
      auto purchase = client.purchase(coreAccount, coreApp,
                                      IPCorePlatform(platform), source.token());
      if (!purchase) {
        dispatch_async(dispatch_get_main_queue(), ^{
          completion(nil, MakeError(purchase.error()));
        });
        return;
      }
      offer = client.download_offer(coreAccount, coreApp, version,
                                    IPCorePlatform(platform), source.token());
    }
    if (!offer) {
      dispatch_async(dispatch_get_main_queue(), ^{
        completion(nil, MakeError(offer.error()));
      });
      return;
    }

    NSString *packageURL =
        [NSString stringWithUTF8String:offer.value().url.c_str()];
    NSURL *url = packageURL != nil ? [NSURL URLWithString:packageURL] : nil;
    if (url == nil) {
      dispatch_async(dispatch_get_main_queue(), ^{
        completion(nil, MakeErrorCode(IPStoreErrorInvalidResponse,
                                      @"Apple returned an invalid package URL."));
      });
      return;
    }

    ipashelf::DownloadOffer offerCopy = offer.value();
    NSDictionary<NSString *, NSString *> *headers = @{
      @"User-Agent" : [NSString stringWithUTF8String:std::string(ipashelf::kDefaultUserAgent).c_str()],
    };

    NSString *resumeKey = [NSString
        stringWithFormat:@"%lld-%@", (long long)app.appID,
                         [NSString stringWithUTF8String:
                                       offerCopy.external_version_id.c_str()]
                             ?: @"latest"];
    [[IPDownloadCenter shared]
        downloadURL:url
           headers:headers
         resumeKey:resumeKey
          progress:^(double fraction, long long received, long long expected) {
            if (progress != nil) progress(fraction, received, expected);
          }
        completion:^(NSString *_Nullable downloadedPath, NSError *_Nullable error) {
          if (error != nil) {
            dispatch_async(dispatch_get_main_queue(), ^{
              completion(nil, error);
            });
            return;
          }
          dispatch_async(self->_queue, ^{
            void (^onMain)(NSString *, NSError *) =
                ^(NSString *savedPath, NSError *saveError) {
                  dispatch_async(dispatch_get_main_queue(), ^{
                    completion(savedPath, saveError);
                  });
                };
            [self finishDownloadAtPath:downloadedPath
                                  app:app
                              account:account
                                 offer:offerCopy
                              coreApp:coreApp
                          coreAccount:coreAccount
                              version:version
                             platform:IPCorePlatform(platform)
                           completion:onMain];
          });
        }];
  });
}

- (void)finishDownloadAtPath:(NSString *)path
                         app:(IPStoreApp *)app
                     account:(IPStoreAccount *)account
                       offer:(ipashelf::DownloadOffer)offer
                     coreApp:(ipashelf::App)coreApp
                 coreAccount:(ipashelf::Account)coreAccount
                     version:(std::string)version
                    platform:(ipashelf::Platform)platform
                  completion:(void (^)(NSString *_Nullable, NSError *_Nullable))completion {
  NSFileManager *manager = [NSFileManager defaultManager];
  if (path.length == 0) {
    completion(nil, MakeErrorCode(IPStoreErrorStorage,
                                  @"The download could not be saved."));
    return;
  }

  if (offer.md5.size() == 32) {
    FILE *file = fopen(path.fileSystemRepresentation, "rb");
    if (file == nullptr) {
      completion(nil, MakeErrorCode(IPStoreErrorStorage,
                                    @"The downloaded package could not be read."));
      return;
    }
    ipashelf::Md5 digest;
    std::uint8_t buffer[1 << 16];
    std::size_t read = 0;
    while ((read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
      digest.update(buffer, read);
    }
    fclose(file);
    const auto hash = digest.finish();
    const std::string actual = ipashelf::hex_encode(
        ipashelf::Bytes(hash.begin(), hash.end()), false);
    if (actual != offer.md5) {
      [manager removeItemAtPath:path error:nil];
      completion(nil, MakeErrorCode(IPStoreErrorIntegrity,
                                    @"The downloaded package failed its checksum."));
      return;
    }
  }

  std::string source = std::string(path.fileSystemRepresentation);
  auto info = ipashelf::inspect_ipa(source);
  if (!info) {
    [manager removeItemAtPath:path error:nil];
    completion(nil, MakeError(info.error()));
    return;
  }
  if (!coreApp.bundle_id.empty() && info.value().bundle_id != coreApp.bundle_id) {
    [manager removeItemAtPath:path error:nil];
    completion(nil, MakeErrorCode(IPStoreErrorIntegrity,
                                  @"Apple returned a package for a different app."));
    return;
  }
  if (const auto platformStatus =
          ipashelf::validate_ipa_platform(info.value(), platform);
      !platformStatus) {
    [manager removeItemAtPath:path error:nil];
    completion(nil, MakeError(platformStatus.error()));
    return;
  }
  auto sinfEntries = ipashelf::resolve_sinf_entries(info.value(), offer.sinfs);
  if (!sinfEntries) {
    [manager removeItemAtPath:path error:nil];
    completion(nil, MakeError(sinfEntries.error()));
    return;
  }

  ipashelf::IpaPatchOptions options;
  options.source_path = source;
  NSString *bundle = [NSString stringWithUTF8String:info.value().bundle_id.c_str()] ?: @"app";
  std::string label = version.empty() ? offer.external_version_id : version;
  if (label.empty()) label = info.value().display_version;
  NSString *suffix = [NSString stringWithUTF8String:label.c_str()] ?: @"download";
  NSString *filename = [NSString stringWithFormat:@"%@-%@.ipa",
                                                  SafeFilename(bundle),
                                                  SafeFilename(suffix)];
  NSString *destination = [IPStoreLibraryDirectory() stringByAppendingPathComponent:filename];
  options.destination_path = destination.fileSystemRepresentation;
  options.sinf_entries = std::move(sinfEntries).value();
  options.metadata = ipashelf::build_itunes_metadata(offer, coreAccount);
  options.artwork = FetchArtwork(offer.artwork_url);

  ipashelf::CancellationSource patchSource;
  auto status = ipashelf::write_patched_ipa(options, patchSource.token());
  [manager removeItemAtPath:path error:nil];
  if (!status) {
    completion(nil, MakeError(status.error()));
    return;
  }

  NSDictionary<NSString *, id> *sidecar = SidecarForApp(
      app, offer, destination, (long long)info.value().file_size);
  NSData *json = [NSJSONSerialization dataWithJSONObject:sidecar
                                                 options:NSJSONWritingPrettyPrinted
                                                   error:nil];
  if (json != nil) {
    [json writeToFile:[destination stringByAppendingPathExtension:@"json"]
           atomically:YES];
  }
  completion(destination, nil);
}

- (void)cancelDownloads {
  [[IPDownloadCenter shared] cancelAll];
}

@end

