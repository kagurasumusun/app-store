// SPDX-License-Identifier: MIT
//
// On-demand (OTA) installation for IPAShelf.
//
// iOS installs an IPA through `itms-services` only when the manifest is served
// over HTTPS from a certificate the device trusts. IPAShelf therefore:
//
//   * creates a local root CA and a leaf certificate for the device's current
//     addresses (all key material stays in the keychain),
//   * serves the manifest, the icons and the IPA over TLS with that identity,
//   * serves the root certificate and a help page over plain HTTP, because the
//     certificate cannot be fetched over a TLS connection that is not trusted
//     yet.
//
// The UI shows the certificate fingerprint and instructs the user to install
// and trust the profile before tapping Install.

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

typedef NS_ENUM(NSInteger, IPInstallServerState) {
  IPInstallServerStateIdle = 0,
  IPInstallServerStateStarting,
  /// Everything is ready and the manifest can be served.
  IPInstallServerStateWaiting,
  /// The device is downloading the payload.
  IPInstallServerStateSending,
  IPInstallServerStateFailed,
  IPInstallServerStateStopped,
};

/// The self-signed root certificate the user installs once.
@interface IPInstallCertificate : NSObject
@property (nonatomic, copy, readonly) NSString *commonName;
/// DER-encoded X.509 certificate.
@property (nonatomic, copy, readonly) NSData *der;
/// Uppercase SHA-256 of `der`, colon separated every byte.
@property (nonatomic, copy, readonly) NSString *sha256Fingerprint;
@end

@interface IPInstallServer : NSObject

- (instancetype)initWithBundleID:(NSString *)bundleID
                         version:(NSString *)version
                           title:(NSString *)title
                       ipaAtPath:(NSString *)ipaPath
                     artworkPath:(nullable NSString *)artworkPath
    NS_DESIGNATED_INITIALIZER;
- (instancetype)init NS_UNAVAILABLE;

/// `itms-services://...` URL to open with `UIApplication`.
@property (nonatomic, copy, readonly, nullable) NSString *installURL;
/// HTTPS manifest URL (useful for diagnostics).
@property (nonatomic, copy, readonly, nullable) NSString *manifestURL;
/// Plain HTTP landing page. Open it in Safari on the device to install the
/// root certificate and start the installation.
@property (nonatomic, copy, readonly, nullable) NSString *pageURL;
/// Plain HTTP URL of the DER certificate.
@property (nonatomic, copy, readonly, nullable) NSString *certificateURL;
@property (nonatomic, strong, readonly, nullable) IPInstallCertificate *certificate;
@property (nonatomic, readonly) IPInstallServerState state;

/// Called on the main queue whenever the state or the transfer progress
/// changes. `progress` is in 0...1 while sending.
@property (nonatomic, copy, nullable) void (^stateHandler)(
    IPInstallServerState state, double progress, NSString *message);

- (BOOL)startWithError:(NSError *_Nullable *_Nullable)error
    NS_SWIFT_NAME(start());
- (void)stop;

/// Generates (or loads) the root certificate without starting a server. Used
/// by Settings to show the fingerprint and export the profile.
+ (IPInstallCertificate *)prepareCertificateWithError:
    (NSError *_Nullable *_Nullable)error
    NS_SWIFT_NAME(prepareCertificate());

@end

NS_ASSUME_NONNULL_END
