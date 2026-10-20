// SPDX-License-Identifier: MIT
//
// On-demand (OTA) installation for IPAShelf.
//
// iOS installs an IPA through `itms-services` only when the manifest is served
// over HTTPS from a certificate the device trusts. IPAShelf therefore:
//
//   * creates a local root CA and a leaf certificate for `localhost` (plus the
//     device's current addresses; all key material stays in the keychain),
//   * serves the manifest, the icons and the IPA over TLS with that identity
//     on loopback — no network beyond this device is involved,
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

/// Outcome of `+[IPInstallServer prepareCertificate]`.
///
/// A plain Objective-C `NSError **` method imports into Swift with a name the
/// compiler derives from the selector, which is easy to get wrong. Returning
/// an explicit result object keeps the Swift surface stable and lets the
/// caller decide how to present the failure.
@interface IPInstallCertificateResult : NSObject
@property (nonatomic, strong, readonly, nullable) IPInstallCertificate *certificate;
@property (nonatomic, strong, readonly, nullable) NSError *error;
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
/// Plain HTTP URL of the DER certificate (for raw certificate download).
@property (nonatomic, copy, readonly, nullable) NSString *certificateURL;
/// Plain HTTP URL of the configuration profile that embeds the root
/// certificate. Opening this in Safari starts the profile install flow.
@property (nonatomic, copy, readonly, nullable) NSString *profileURL;
@property (nonatomic, strong, readonly, nullable) IPInstallCertificate *certificate;
@property (nonatomic, readonly) IPInstallServerState state;

/// Called on the main queue whenever the state or the transfer progress
/// changes. `progress` is in 0...1 while sending.
@property (nonatomic, copy, nullable) void (^stateHandler)(
    IPInstallServerState state, double progress, NSString *message);

/// Starts the TLS listener and the plain-HTTP certificate page.
/// Returns `NO` when it fails; `startupError` then explains why.
- (BOOL)start;
/// Why the last `start` failed, if it did.
@property (nonatomic, strong, readonly, nullable) NSError *startupError;
- (void)stop;

/// Generates (or loads) the root certificate without starting a server. Used
/// by Settings to show the fingerprint and export the profile.
+ (IPInstallCertificateResult *)prepareCertificate;

/// Whether this device currently trusts the given root certificate as a TLS
/// anchor — profile installed AND enabled under Certificate Trust Settings.
/// Evaluated against the system trust store as-is; Settings uses it to show
/// "not set up yet" versus a ready state instead of guessing.
+ (BOOL)rootCertificateIsTrusted:(IPInstallCertificate *)certificate
    NS_SWIFT_NAME(rootCertificateIsTrusted(_:));

@end

/// Serves only the root-certificate profile over plain HTTP on loopback, so
/// Settings can offer "Install Root Certificate" before any package exists.
/// Opening `profileURL` in Safari starts the iOS profile download flow.
@interface IPInstallCertificateServer : NSObject

@property (nonatomic, copy, readonly, nullable) NSString *profileURL;
@property (nonatomic, copy, readonly, nullable) NSString *certificateURL;
@property (nonatomic, strong, readonly, nullable) IPInstallCertificate *certificate;
@property (nonatomic, strong, readonly, nullable) NSError *startupError;

/// Creates (or loads) the root certificate and starts serving it.
- (BOOL)start;
- (void)stop;

@end

NS_ASSUME_NONNULL_END
