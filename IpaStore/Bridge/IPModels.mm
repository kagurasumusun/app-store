// SPDX-License-Identifier: MIT

#import "IPInternal.h"

static NSString *IPString(const std::string &value) {
  return [[NSString alloc] initWithBytes:value.data()
                                  length:value.size()
                                encoding:NSUTF8StringEncoding] ?: @"";
}

static NSArray<NSString *> *IPStrings(const std::vector<std::string> &values) {
  NSMutableArray<NSString *> *result = [NSMutableArray arrayWithCapacity:values.size()];
  for (const auto &value : values) {
    [result addObject:IPString(value)];
  }
  return result;
}

static NSArray<NSString *> *IPPlatforms(const std::vector<ipashelf::Platform> &platforms) {
  NSMutableArray<NSString *> *result = [NSMutableArray arrayWithCapacity:platforms.size()];
  for (const auto platform : platforms) {
    const std::string name = ipashelf::to_string(platform);
    [result addObject:IPString(name)];
  }
  return result;
}

@implementation IPStoreApp {
  long long _appIDStorage;
}

- (instancetype)initWithApp:(const ipashelf::App &)app {
  self = [super init];
  if (self != nil) {
    _appIDStorage = app.id;
    _bundleID = IPString(app.bundle_id);
    _name = IPString(app.name);
    _version = IPString(app.version);
    _seller = IPString(app.seller);
    _artworkURL = IPString(app.artwork_url);
    _appDescription = IPString(app.description);
    _releaseNotes = IPString(app.release_notes);
    _releaseDate = IPString(app.release_date);
    _minimumOSVersion = IPString(app.minimum_os_version);
    _artistName = IPString(app.artist_name);
    _sellerURL = IPString(app.seller_url);
    _primaryGenre = IPString(app.primary_genre);
    _contentAdvisoryRating = IPString(app.content_advisory_rating);
    _currency = IPString(app.currency);
    _trackViewURL = IPString(app.track_view_url);
    _currentVersionReleaseDate = IPString(app.current_version_release_date);
    _languageCodes = IPString(app.language_codes);
    _screenshots = IPStrings(app.screenshot_urls) ?: @[];
    _iPadScreenshots = IPStrings(app.ipad_screenshot_urls) ?: @[];
    _supportedDevices = IPStrings(app.supported_devices) ?: @[];
    _averageRating = app.average_rating;
    _ratingCount = app.rating_count;
    _currentVersionAverageRating = app.current_version_average_rating;
    _currentVersionRatingCount = app.current_version_rating_count;
    _byteSize = app.byte_size;
    _universal = app.is_universal;
    _price = app.price;
    _hasInAppPurchases = app.has_arcade_offer;
    _platforms = IPPlatforms(app.platforms) ?: @[];
  }
  return self;
}

- (long long)appID {
  return _appIDStorage;
}

@end

@implementation IPStoreAccount

- (instancetype)initWithAccount:(const ipashelf::Account &)account {
  self = [super init];
  if (self != nil) {
    _email = IPString(account.email);
    _name = IPString(account.name);
    _region = IPString(account.region);
    _storefront = IPString(account.storefront);
    _directoryServicesID = IPString(account.directory_services_id);
    _password = IPString(account.password);
    _passwordToken = IPString(account.password_token);
    _pod = IPString(account.pod);
    _deviceID = IPString(account.device_id);
  }
  return self;
}

- (ipashelf::Account)coreAccount {
  ipashelf::Account account;
  account.email = _email.UTF8String ?: "";
  account.name = _name.UTF8String ?: "";
  account.region = _region.UTF8String ?: "";
  account.storefront = _storefront.UTF8String ?: "";
  account.directory_services_id = _directoryServicesID.UTF8String ?: "";
  account.password = _password.UTF8String ?: "";
  account.password_token = _passwordToken.UTF8String ?: "";
  account.pod = _pod.UTF8String ?: "";
  account.device_id = _deviceID.UTF8String ?: "";
  return account;
}

@end

@implementation IPStoreReview

- (instancetype)initWithReview:(const ipashelf::Review &)review {
  self = [super init];
  if (self != nil) {
    _reviewID = IPString(review.id);
    _author = IPString(review.author);
    _title = IPString(review.title);
    _body = IPString(review.body);
    _version = IPString(review.version);
    _updated = IPString(review.updated);
    _rating = static_cast<NSInteger>(review.rating);
  }
  return self;
}

@end

@implementation IPStoreReviewsPage

- (instancetype)initWithPage:(const ipashelf::ReviewsPage &)page {
  self = [super init];
  if (self != nil) {
    NSMutableArray<IPStoreReview *> *reviews =
        [NSMutableArray arrayWithCapacity:page.reviews.size()];
    for (const auto &review : page.reviews) {
      [reviews addObject:[[IPStoreReview alloc] initWithReview:review]];
    }
    _reviews = reviews;
    _page = static_cast<NSInteger>(page.page);
    _hasMore = page.has_more;
  }
  return self;
}

@end

@implementation IPStoreStorefront

- (instancetype)initWithStorefront:(const ipashelf::Storefront &)storefront {
  self = [super init];
  if (self != nil) {
    _code = IPString(storefront.code);
    _name = IPString(storefront.name);
    _storefrontID = IPString(storefront.storefront_id);
    _currency = IPString(storefront.currency);
  }
  return self;
}

@end

@implementation IPStoreSearchFilter

- (instancetype)init {
  self = [super init];
  if (self != nil) {
    _term = @"";
    _region = @"us";
    _platform = IPStorePlatformIPhone;
    _limit = 25;
    _genreID = 0;
    _sort = @"relevance";
  }
  return self;
}

@end

@implementation IPStoreLoginResult

- (instancetype)initWithAccount:(nullable IPStoreAccount *)account
              twoFactorRequired:(BOOL)twoFactorRequired {
  self = [super init];
  if (self != nil) {
    _account = account;
    _twoFactorRequired = twoFactorRequired;
  }
  return self;
}

@end

@implementation IPStoreVersion

- (instancetype)initWithExternalID:(NSString *)externalID
                    displayVersion:(NSString *)displayVersion {
  self = [super init];
  if (self != nil) {
    _externalVersionID = [externalID copy];
    _displayVersion = [displayVersion copy];
  }
  return self;
}

@end
