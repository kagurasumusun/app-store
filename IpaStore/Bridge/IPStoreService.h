// SPDX-License-Identifier: MIT
//
// Objective-C facade for the portable IPAShelf core. Swift only sees this
// header; every C++ type stays inside the implementation file.

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

typedef NS_ENUM(NSInteger, IPStorePlatform) {
  IPStorePlatformIPhone = 0,
  IPStorePlatformIPad,
  IPStorePlatformMac,
  IPStorePlatformTV,
  IPStorePlatformWatch,
  IPStorePlatformVision,
  /// No platform constraint (purchase-history listing only).
  IPStorePlatformAny,
};

typedef NS_ENUM(NSInteger, IPStoreErrorCode) {
  IPStoreErrorCancelled = 1,
  IPStoreErrorNetwork,
  IPStoreErrorHTTPStatus,
  IPStoreErrorInvalidResponse,
  IPStoreErrorUnauthorized,
  IPStoreErrorTokenExpired,
  IPStoreErrorTwoFactorRequired,
  IPStoreErrorLicenseRequired,
  IPStoreErrorUnavailable,
  IPStoreErrorAppNotFound,
  IPStoreErrorUnsupported,
  IPStoreErrorSignerUnavailable,
  IPStoreErrorStorage,
  IPStoreErrorIntegrity,
  IPStoreErrorPurchaseFailed,
  IPStoreErrorInternal,
};

FOUNDATION_EXPORT NSErrorDomain const IPStoreErrorDomain;

@interface IPStoreApp : NSObject
@property (nonatomic, readonly) long long appID NS_SWIFT_NAME(appID);
@property (nonatomic, copy, readonly) NSString *bundleID NS_SWIFT_NAME(bundleID);
@property (nonatomic, copy, readonly) NSString *name;
@property (nonatomic, copy, readonly) NSString *version;
@property (nonatomic, copy, readonly) NSString *seller;
@property (nonatomic, copy, readonly) NSString *artworkURL NS_SWIFT_NAME(artworkURL);
@property (nonatomic, copy, readonly) NSString *appDescription NS_SWIFT_NAME(appDescription);
@property (nonatomic, copy, readonly) NSString *releaseNotes;
@property (nonatomic, copy, readonly) NSString *releaseDate;
@property (nonatomic, copy, readonly) NSString *minimumOSVersion NS_SWIFT_NAME(minimumOSVersion);
@property (nonatomic, copy, readonly) NSString *artistName;
@property (nonatomic, copy, readonly) NSString *sellerURL NS_SWIFT_NAME(sellerURL);
@property (nonatomic, copy, readonly) NSString *primaryGenre;
@property (nonatomic, copy, readonly) NSString *contentAdvisoryRating;
@property (nonatomic, copy, readonly) NSString *currency;
@property (nonatomic, copy, readonly) NSString *trackViewURL NS_SWIFT_NAME(trackViewURL);
@property (nonatomic, copy, readonly) NSString *currentVersionReleaseDate;
@property (nonatomic, copy, readonly) NSString *languageCodes;
@property (nonatomic, copy, readonly) NSArray<NSString *> *screenshots;
@property (nonatomic, copy, readonly) NSArray<NSString *> *iPadScreenshots NS_SWIFT_NAME(iPadScreenshots);
@property (nonatomic, copy, readonly) NSArray<NSString *> *supportedDevices;
@property (nonatomic, readonly) double averageRating;
@property (nonatomic, readonly) long long ratingCount;
@property (nonatomic, readonly) double currentVersionAverageRating;
@property (nonatomic, readonly) long long currentVersionRatingCount;
@property (nonatomic, readonly) long long byteSize;
@property (nonatomic, readonly, getter=isUniversal) BOOL universal;
@property (nonatomic, readonly) double price;
@property (nonatomic, readonly) BOOL hasInAppPurchases NS_SWIFT_NAME(hasInAppPurchases);
@property (nonatomic, copy, readonly) NSArray<NSString *> *platforms NS_SWIFT_NAME(platforms);
/// When the signed-in account acquired the app; only set for purchase
/// history entries.
@property (nonatomic, copy, readonly, nullable) NSDate *purchaseDate NS_SWIFT_NAME(purchaseDate);
@end

/// A storefront the user can search in.
@interface IPStoreStorefront : NSObject
@property (nonatomic, copy, readonly) NSString *code;
@property (nonatomic, copy, readonly) NSString *name;
@property (nonatomic, copy, readonly) NSString *storefrontID NS_SWIFT_NAME(storefrontID);
@property (nonatomic, copy, readonly) NSString *currency;
@end

/// Filters applied to a catalog search. `freeOnly` is a tri-state: nil keeps
/// paid and free results.
@interface IPStoreSearchFilter : NSObject
@property (nonatomic, copy) NSString *term;
@property (nonatomic, copy) NSString *region;
@property (nonatomic) IPStorePlatform platform;
@property (nonatomic) NSInteger limit;
@property (nonatomic) long long genreID NS_SWIFT_NAME(genreID);
@property (nonatomic, copy, nullable) NSNumber *freeOnly;
/// "relevance", "name", "rating" or "recent".
@property (nonatomic, copy) NSString *sort;
@end

@interface IPStoreAccount : NSObject
@property (nonatomic, copy, readonly) NSString *email;
@property (nonatomic, copy, readonly) NSString *name;
@property (nonatomic, copy, readonly) NSString *region;
@property (nonatomic, copy, readonly) NSString *storefront;
@property (nonatomic, copy, readonly) NSString *directoryServicesID NS_SWIFT_NAME(directoryServicesID);
/// Whether the account's password is stored (in the Keychain) so its token
/// can be rotated without asking for the password again.
@property (nonatomic, readonly) BOOL hasSavedPassword;
@end

@interface IPStoreLoginResult : NSObject
@property (nonatomic, strong, readonly, nullable) IPStoreAccount *account;
@property (nonatomic, readonly) BOOL twoFactorRequired;
@end

@interface IPStoreVersion : NSObject
@property (nonatomic, copy, readonly) NSString *externalVersionID NS_SWIFT_NAME(externalVersionID);
/// Marketing version read from the package itself; empty until the version
/// metadata has been loaded.
@property (nonatomic, copy, readonly) NSString *displayVersion;
/// Release date read from the package; nil until metadata has been loaded.
@property (nonatomic, copy, readonly, nullable) NSDate *releaseDate;
/// Whether this is the build the store currently serves by default.
@property (nonatomic, readonly, getter=isLatest) BOOL latest;
@end

/// Display version and release date of a single external version, read from
/// the package's own Info.plist.
@interface IPStoreVersionMetadata : NSObject
@property (nonatomic, copy, readonly) NSString *displayVersion;
@property (nonatomic, copy, readonly, nullable) NSDate *releaseDate;
@end

/// One page of the account's purchase history.
@interface IPStoreOwnedAppsPage : NSObject
@property (nonatomic, copy, readonly) NSArray<IPStoreApp *> *apps;
@property (nonatomic, readonly) NSInteger total;
@property (nonatomic, readonly) NSInteger page;
@property (nonatomic, readonly) NSInteger limit;
@end

@interface IPStoreService : NSObject

@property (class, nonatomic, readonly) IPStoreService *shared;

/// Persisted accounts, ordered by most recently used.
@property (nonatomic, copy, readonly) NSArray<IPStoreAccount *> *accounts NS_SWIFT_NAME(accounts);
@property (nonatomic, strong, readonly, nullable) IPStoreAccount *activeAccount NS_SWIFT_NAME(activeAccount);
/// Six-byte protocol identity rendered as uppercase hexadecimal.
@property (nonatomic, copy, readonly) NSString *deviceIdentifier NS_SWIFT_NAME(deviceIdentifier);

- (void)setActiveAccount:(nullable IPStoreAccount *)account
    NS_SWIFT_NAME(setActiveAccount(_:));
- (void)removeAccount:(IPStoreAccount *)account
    NS_SWIFT_NAME(removeAccount(_:));

/// Every storefront the app can search in, ordered by display name.
+ (NSArray<IPStoreStorefront *> *)storefronts NS_SWIFT_NAME(storefronts());

- (void)loginWithEmail:(NSString *)email
              password:(NSString *)password
              authCode:(nullable NSString *)authCode
                region:(NSString *)region
            completion:(void (^)(IPStoreLoginResult *_Nullable result,
                                 NSError *_Nullable error))completion
    NS_SWIFT_NAME(login(email:password:authCode:region:completion:));

/// Rotates the account's password token by signing in again with the stored
/// password. The refreshed account replaces the stored one. When Apple
/// asks for a verification code the result reports `twoFactorRequired` and
/// the caller can retry with `authCode` set.
- (void)refreshAccount:(IPStoreAccount *)account
              authCode:(nullable NSString *)authCode
            completion:(void (^)(IPStoreLoginResult *_Nullable result,
                                 NSError *_Nullable error))completion
    NS_SWIFT_NAME(refresh(account:authCode:completion:));

/// Acquires the app's license for the account without downloading it.
/// Free apps only; already-owned apps succeed.
- (void)purchaseApp:(IPStoreApp *)app
            account:(IPStoreAccount *)account
           platform:(IPStorePlatform)platform
         completion:(void (^)(NSError *_Nullable error))completion
    NS_SWIFT_NAME(purchase(app:account:platform:completion:));

- (void)searchForTerm:(NSString *)term
               region:(NSString *)region
             platform:(IPStorePlatform)platform
                limit:(NSInteger)limit
           completion:(void (^)(NSArray<IPStoreApp *> *_Nullable apps,
                                NSError *_Nullable error))completion
    NS_SWIFT_NAME(search(term:region:platform:limit:completion:));

- (void)searchWithFilter:(IPStoreSearchFilter *)filter
              completion:(void (^)(NSArray<IPStoreApp *> *_Nullable apps,
                                   NSError *_Nullable error))completion
    NS_SWIFT_NAME(search(filter:completion:));

- (void)lookupAppID:(long long)appID
           bundleID:(nullable NSString *)bundleID
             region:(NSString *)region
           platform:(IPStorePlatform)platform
         completion:(void (^)(IPStoreApp *_Nullable app,
                              NSError *_Nullable error))completion
    NS_SWIFT_NAME(lookup(appID:bundleID:region:platform:completion:));

- (void)listVersionsForApp:(IPStoreApp *)app
                   account:(IPStoreAccount *)account
                  platform:(IPStorePlatform)platform
                completion:(void (^)(NSArray<IPStoreVersion *> *_Nullable versions,
                                     NSError *_Nullable error))completion
    NS_SWIFT_NAME(listVersions(app:account:platform:completion:));

/// Resolves the marketing version and release date of one external version
/// from the version-pinned download response. The package itself is never
/// downloaded.
- (void)versionMetadataForApp:(IPStoreApp *)app
                      account:(IPStoreAccount *)account
            externalVersionID:(NSString *)externalVersionID
                     platform:(IPStorePlatform)platform
                   completion:(void (^)(IPStoreVersionMetadata *_Nullable metadata,
                                        NSError *_Nullable error))completion
    NS_SWIFT_NAME(versionMetadata(app:account:externalVersionID:platform:completion:));

/// Lists the apps the account owns, newest purchase first. `platform` may be
/// IPStorePlatformAny to skip filtering; page is 1-based and limit must not
/// exceed 100.
- (void)ownedAppsForAccount:(IPStoreAccount *)account
                   platform:(IPStorePlatform)platform
                       page:(NSInteger)page
                      limit:(NSInteger)limit
                 completion:(void (^)(IPStoreOwnedAppsPage *_Nullable page,
                                      NSError *_Nullable error))completion
    NS_SWIFT_NAME(ownedApps(account:platform:page:limit:completion:));

/// Purchases the app when needed and downloads a patched IPA. Progress is
/// reported on an internal queue; hop to the main actor before touching UI.
- (void)downloadApp:(IPStoreApp *)app
            account:(IPStoreAccount *)account
   externalVersionID:(nullable NSString *)externalVersionID
           platform:(IPStorePlatform)platform
           progress:(void (^_Nullable)(double fraction, long long receivedBytes,
                                       long long expectedBytes))progress
         completion:(void (^)(NSString *_Nullable ipaPath,
                              NSError *_Nullable error))completion
    NS_SWIFT_NAME(download(app:account:externalVersionID:platform:progress:completion:));

- (void)cancelDownloads NS_SWIFT_NAME(cancelDownloads());

/// Cancels one app's download. Works before the transfer starts too (the
/// store offer may still be in flight): the cancellation is remembered and
/// applied when the transfer would begin, then forgotten.
- (void)cancelDownloadForAppID:(long long)appID
           externalVersionID:(nullable NSString *)externalVersionID
    NS_SWIFT_NAME(cancelDownload(appID:externalVersionID:));

@end

/// Finished IPAs live here and are exposed through the Files app.
FOUNDATION_EXPORT NSString *IPStoreLibraryDirectory(void);

NS_ASSUME_NONNULL_END
