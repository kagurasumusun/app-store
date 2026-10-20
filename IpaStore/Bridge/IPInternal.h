// SPDX-License-Identifier: MIT
//
// Internal Objective-C++ declarations shared by the bridge implementation
// files. This header must never be imported by the Swift bridging header.

#import <Foundation/Foundation.h>

#ifdef __cplusplus
#include <memory>

#include "ipashelf/base.hpp"
#include "ipashelf/models.hpp"
#include "ipashelf/sap.hpp"
#include "ipashelf/store_client.hpp"
#endif

#import "IPStoreService.h"

NS_ASSUME_NONNULL_BEGIN

@interface IPStoreApp ()
@property (nonatomic, copy, readwrite) NSString *bundleID;
@property (nonatomic, copy, readwrite) NSString *name;
@property (nonatomic, copy, readwrite) NSString *version;
@property (nonatomic, copy, readwrite) NSString *seller;
@property (nonatomic, copy, readwrite) NSString *artworkURL;
@property (nonatomic, copy, readwrite) NSString *appDescription;
@property (nonatomic, copy, readwrite) NSString *releaseNotes;
@property (nonatomic, copy, readwrite) NSString *releaseDate;
@property (nonatomic, copy, readwrite) NSString *minimumOSVersion;
@property (nonatomic, readwrite) double price;
@property (nonatomic, readwrite) BOOL hasInAppPurchases;
@property (nonatomic, copy, readwrite) NSArray<NSString *> *platforms;
#ifdef __cplusplus
- (instancetype)initWithApp:(const ipashelf::App &)app;
#endif
@end

@interface IPStoreAccount ()
@property (nonatomic, copy, readwrite) NSString *email;
@property (nonatomic, copy, readwrite) NSString *name;
@property (nonatomic, copy, readwrite) NSString *region;
@property (nonatomic, copy, readwrite) NSString *storefront;
@property (nonatomic, copy, readwrite) NSString *directoryServicesID;
@property (nonatomic, copy) NSString *password;
@property (nonatomic, copy) NSString *passwordToken;
@property (nonatomic, copy) NSString *pod;
@property (nonatomic, copy) NSString *deviceID;
#ifdef __cplusplus
- (instancetype)initWithAccount:(const ipashelf::Account &)account;
- (ipashelf::Account)coreAccount;
#endif
@end

@interface IPStoreVersion ()
@property (nonatomic, copy, readwrite) NSString *externalVersionID;
@property (nonatomic, copy, readwrite) NSString *displayVersion;
- (instancetype)initWithExternalID:(NSString *)externalID
                    displayVersion:(NSString *)displayVersion;
@end

@interface IPStoreReview ()
@property (nonatomic, copy, readwrite) NSString *reviewID;
@property (nonatomic, copy, readwrite) NSString *author;
@property (nonatomic, copy, readwrite) NSString *title;
@property (nonatomic, copy, readwrite) NSString *body;
@property (nonatomic, copy, readwrite) NSString *version;
@property (nonatomic, copy, readwrite) NSString *updated;
@property (nonatomic, readwrite) NSInteger rating;
#ifdef __cplusplus
- (instancetype)initWithReview:(const ipashelf::Review &)review;
#endif
@end

@interface IPStoreReviewsPage ()
@property (nonatomic, copy, readwrite) NSArray<IPStoreReview *> *reviews;
@property (nonatomic, readwrite) NSInteger page;
@property (nonatomic, readwrite) BOOL hasMore;
#ifdef __cplusplus
- (instancetype)initWithPage:(const ipashelf::ReviewsPage &)page;
#endif
@end

@interface IPStoreStorefront ()
@property (nonatomic, copy, readwrite) NSString *code;
@property (nonatomic, copy, readwrite) NSString *name;
@property (nonatomic, copy, readwrite) NSString *storefrontID;
@property (nonatomic, copy, readwrite) NSString *currency;
#ifdef __cplusplus
- (instancetype)initWithStorefront:(const ipashelf::Storefront &)storefront;
#endif
@end

#ifdef __cplusplus
/// Bridges the public Objective-C enum to the portable core.
[[nodiscard]] ipashelf::Platform IPCorePlatform(IPStorePlatform platform);
#endif

@interface IPStoreSearchFilter ()
#ifdef __cplusplus
- (ipashelf::SearchQuery)coreQuery;
#endif
@end

@interface IPStoreLoginResult ()
- (instancetype)initWithAccount:(nullable IPStoreAccount *)account
              twoFactorRequired:(BOOL)twoFactorRequired;
@end

/// Keychain-backed string storage. All values are encrypted at rest by iOS.
@interface IPKeychain : NSObject
+ (nullable NSString *)stringForKey:(NSString *)key;
+ (BOOL)setString:(NSString *)value
           forKey:(NSString *)key
            error:(NSError *_Nullable *_Nullable)error;
+ (BOOL)removeKey:(NSString *)key error:(NSError *_Nullable *_Nullable)error;
@end

/// Persists the signed-in account list as JSON inside a single Keychain item.
@interface IPAccountStore : NSObject
@property (nonatomic, copy, readonly) NSArray<IPStoreAccount *> *accounts;
@property (nonatomic, strong, nullable) IPStoreAccount *activeAccount;
- (void)upsertAccount:(IPStoreAccount *)account;
- (void)removeAccount:(IPStoreAccount *)account;
- (nullable IPStoreAccount *)accountForEmail:(NSString *)email;
@end

#ifdef __cplusplus
/// Performs the CoreFP/AppleMediaServices SAP handshake using the symbols that
/// Apple ships with iOS.
ipashelf::Result<std::shared_ptr<ipashelf::SapBackend>> IPCreateSAPBackend();

/// Shared NSURLSession-backed transport used by every StoreClient.
ipashelf::HttpTransport &IPSharedHTTPTransport();

/// Six-byte protocol identity persisted in the Keychain for the lifetime of
/// the installation.
ipashelf::HardwareIdentity IPHardwareIdentity();
#endif

/// Maps a fully-qualified IPA path below Documents to the Files-visible URL.
NSURL *IPStoreLibraryURL(void);

NS_ASSUME_NONNULL_END
