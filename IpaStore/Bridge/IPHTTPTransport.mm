// SPDX-License-Identifier: MIT

#import <Foundation/Foundation.h>

#include <memory>

#import "IPInternal.h"

// Redirect policy is per request. Requests that carry credentials or SAP
// signatures (sign-in, ent/download, purchase history) must never be
// forwarded automatically: the core inspects every Location itself, re-signs
// and only then retries. Those core requests arrive with follow_redirects
// cleared, and the transport marks their tasks so this delegate refuses the
// redirect and surfaces the 3xx response instead. Every other request (bag,
// catalog, legacy download endpoints) follows redirects like a normal HTTP
// client, matching what the core expects.
static NSString *const kIPNoRedirectTaskDescription = @"ipashelf-no-redirect";

@interface IPNoRedirectDelegate : NSObject <NSURLSessionTaskDelegate>
@end

@implementation IPNoRedirectDelegate
- (void)URLSession:(NSURLSession *)session
                          task:(NSURLSessionTask *)task
    willPerformHTTPRedirection:(NSHTTPURLResponse *)response
                    newRequest:(NSURLRequest *)request
             completionHandler:(void (^)(NSURLRequest *_Nullable))completionHandler {
  if ([task.taskDescription isEqualToString:kIPNoRedirectTaskDescription]) {
    completionHandler(nil);
  } else {
    completionHandler(request);
  }
}
@end

namespace {

NSURLSession *IPSessions(void) {
  static NSURLSession *session;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    NSURLSessionConfiguration *configuration =
        [NSURLSessionConfiguration defaultSessionConfiguration];
    configuration.timeoutIntervalForRequest = 120;
    configuration.timeoutIntervalForResource = 300;
    configuration.HTTPMaximumConnectionsPerHost = 4;
    configuration.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
    configuration.URLCache = nil;
    configuration.HTTPShouldSetCookies = YES;
    configuration.HTTPCookieStorage = [NSHTTPCookieStorage sharedHTTPCookieStorage];
    static IPNoRedirectDelegate *delegate = nil;
    static dispatch_once_t delegateOnce;
    dispatch_once(&delegateOnce, ^{
      delegate = [[IPNoRedirectDelegate alloc] init];
    });
    session = [NSURLSession sessionWithConfiguration:configuration
                                            delegate:delegate
                                       delegateQueue:nil];
  });
  return session;
}

}  // namespace

class IPHTTPTransport final : public ipashelf::HttpTransport {
 public:
  static IPHTTPTransport &Shared() {
    static IPHTTPTransport transport;
    return transport;
  }

  ipashelf::Result<ipashelf::HttpResponse> send(
      const ipashelf::HttpRequest &request,
      const ipashelf::CancellationToken &token) override {
    if (token.cancelled()) {
      return ipashelf::make_error(ipashelf::ErrorCode::kCancelled,
                                  "request cancelled");
    }
    NSString *urlString = [NSString stringWithUTF8String:request.url.c_str()];
    NSURL *url = urlString != nil ? [NSURL URLWithString:urlString] : nil;
    if (url == nil) {
      return ipashelf::make_error(ipashelf::ErrorCode::kInvalidArgument,
                                  "invalid request URL");
    }
    NSMutableURLRequest *urlRequest =
        [NSMutableURLRequest requestWithURL:url];
    urlRequest.HTTPMethod =
        [NSString stringWithUTF8String:request.method.c_str()] ?: @"GET";
    for (const auto &header : request.headers.items()) {
      NSString *name = [NSString stringWithUTF8String:header.first.c_str()];
      NSString *value = [NSString stringWithUTF8String:header.second.c_str()];
      if (name != nil && value != nil) {
        [urlRequest setValue:value forHTTPHeaderField:name];
      }
    }
    if (!request.body.empty()) {
      urlRequest.HTTPBody = [NSData dataWithBytes:request.body.data()
                                           length:request.body.size()];
    }
    if (request.range_start.has_value()) {
      // Partial-zip version inspection reads the package with range GETs.
      NSString *range;
      if (request.range_end.has_value()) {
        range = [NSString stringWithFormat:@"bytes=%llu-%llu",
                                           (unsigned long long)*request.range_start,
                                           (unsigned long long)*request.range_end];
      } else {
        range = [NSString stringWithFormat:@"bytes=%llu-",
                                           (unsigned long long)*request.range_start];
      }
      [urlRequest setValue:range forHTTPHeaderField:@"Range"];
      // A transcoded body would invalidate the byte offsets the core is
      // about to verify against Content-Range.
      [urlRequest setValue:@"identity" forHTTPHeaderField:@"Accept-Encoding"];
    }

    dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);
    auto response = std::make_shared<ipashelf::HttpResponse>();
    auto error = std::make_shared<ipashelf::Error>();

    NSURLSessionDataTask *task = [IPSessions()
        dataTaskWithRequest:urlRequest
          completionHandler:^(NSData *data, NSURLResponse *urlResponse,
                              NSError *urlError) {
            if (urlError != nil) {
              if (urlError.code == NSURLErrorCancelled) {
                *error = ipashelf::make_error(ipashelf::ErrorCode::kCancelled,
                                              "request cancelled");
              } else {
                *error = ipashelf::Error{
                    ipashelf::ErrorCode::kNetwork,
                    urlError.localizedDescription.UTF8String
                        ? std::string(urlError.localizedDescription.UTF8String)
                        : std::string("network error")};
              }
            } else {
              NSHTTPURLResponse *http = (NSHTTPURLResponse *)urlResponse;
              if ([http isKindOfClass:[NSHTTPURLResponse class]]) {
                response->status = (int)http.statusCode;
                for (id key in http.allHeaderFields) {
                  id value = http.allHeaderFields[key];
                  if ([key isKindOfClass:[NSString class]] &&
                      [value isKindOfClass:[NSString class]]) {
                    response->headers.add(
                        std::string([(NSString *)key UTF8String]),
                        std::string([(NSString *)value UTF8String]));
                  }
                }
                response->final_url = http.URL.absoluteString.UTF8String ?: "";
              }
              if (data.length > 0) {
                const auto *bytes = static_cast<const std::uint8_t *>(data.bytes);
                response->body.assign(bytes, bytes + data.length);
              }
            }
            dispatch_semaphore_signal(semaphore);
          }];
    if (!request.follow_redirects) {
      task.taskDescription = kIPNoRedirectTaskDescription;
    }
    [task resume];

    const dispatch_time_t deadline =
        dispatch_time(DISPATCH_TIME_NOW, (int64_t)(120 * NSEC_PER_SEC));
    if (dispatch_semaphore_wait(semaphore, deadline) != 0) {
      [task cancel];
      return ipashelf::make_error(ipashelf::ErrorCode::kNetwork,
                                  "request timed out");
    }
    if (error->code != ipashelf::ErrorCode::kNone) {
      return *error;
    }
    if (token.cancelled()) {
      return ipashelf::make_error(ipashelf::ErrorCode::kCancelled,
                                  "request cancelled");
    }
    return *response;
  }

 private:
  IPHTTPTransport() = default;
};

ipashelf::HttpTransport &IPSharedHTTPTransport(void) {
  return IPHTTPTransport::Shared();
}
