// SPDX-License-Identifier: MIT

#import <Security/Security.h>

#import "IPInternal.h"

static NSString *const kIPKeychainService = @"com.example.ipastore";

@implementation IPKeychain

+ (NSMutableDictionary<id, id> *)baseQueryForKey:(NSString *)key {
  return [@{
    (__bridge id)kSecClass : (__bridge id)kSecClassGenericPassword,
    (__bridge id)kSecAttrService : kIPKeychainService,
    (__bridge id)kSecAttrAccount : key,
  } mutableCopy];
}

+ (nullable NSString *)stringForKey:(NSString *)key {
  NSMutableDictionary<id, id> *query = [self baseQueryForKey:key];
  query[(__bridge id)kSecReturnData] = @YES;
  query[(__bridge id)kSecMatchLimit] = (__bridge id)kSecMatchLimitOne;

  CFTypeRef result = NULL;
  const OSStatus status = SecItemCopyMatching((__bridge CFDictionaryRef)query,
                                              &result);
  if (status != errSecSuccess || result == NULL) return nil;
  NSData *data = CFBridgingRelease(result);
  if (![data isKindOfClass:[NSData class]]) return nil;
  return [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
}

+ (BOOL)setString:(NSString *)value
           forKey:(NSString *)key
            error:(NSError *_Nullable *_Nullable)error {
  NSData *data = [value dataUsingEncoding:NSUTF8StringEncoding];
  if (data == nil) {
    if (error != NULL) {
      *error = [NSError errorWithDomain:IPStoreErrorDomain
                                   code:IPStoreErrorStorage
                               userInfo:@{NSLocalizedDescriptionKey : @"Could not encode Keychain value."}];
    }
    return NO;
  }

  NSMutableDictionary<id, id> *query = [self baseQueryForKey:key];
  NSDictionary<id, id> *attributes = @{(__bridge id)kSecValueData : data};
  OSStatus status = SecItemUpdate((__bridge CFDictionaryRef)query,
                                  (__bridge CFDictionaryRef)attributes);
  if (status == errSecItemNotFound) {
    query[(__bridge id)kSecValueData] = data;
    query[(__bridge id)kSecAttrAccessible] =
        (__bridge id)kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly;
    status = SecItemAdd((__bridge CFDictionaryRef)query, NULL);
  }
  if (status != errSecSuccess) {
    if (error != NULL) {
      *error = [NSError errorWithDomain:IPStoreErrorDomain
                                   code:IPStoreErrorStorage
                               userInfo:@{
                                 NSLocalizedDescriptionKey :
                                     [NSString stringWithFormat:@"Keychain update failed (%d).", (int)status]
                               }];
    }
    return NO;
  }
  return YES;
}

+ (BOOL)removeKey:(NSString *)key error:(NSError *_Nullable *_Nullable)error {
  NSMutableDictionary<id, id> *query = [self baseQueryForKey:key];
  const OSStatus status = SecItemDelete((__bridge CFDictionaryRef)query);
  if (status == errSecSuccess || status == errSecItemNotFound) return YES;
  if (error != NULL) {
    *error = [NSError errorWithDomain:IPStoreErrorDomain
                                 code:IPStoreErrorStorage
                             userInfo:@{
                               NSLocalizedDescriptionKey :
                                   [NSString stringWithFormat:@"Keychain delete failed (%d).", (int)status]
                             }];
  }
  return NO;
}

@end

@interface IPAccountStore ()
- (void)reload;
- (BOOL)persist;
@end

@implementation IPAccountStore {
  NSMutableArray<IPStoreAccount *> *_accounts;
}

- (instancetype)init {
  self = [super init];
  if (self != nil) {
    _accounts = [NSMutableArray array];
    [self reload];
  }
  return self;
}

- (void)reload {
  NSString *json = [IPKeychain stringForKey:@"accounts"];
  [_accounts removeAllObjects];
  if (json.length == 0) return;
  const std::string text = ipashelf::string_from_bytes(
      ipashelf::bytes_from_string(json.UTF8String));
  auto parsed = ipashelf::accounts_from_json(text);
  if (!parsed) {
    NSLog(@"IPAShelf: could not decode stored accounts: %s",
          parsed.error().message.c_str());
    return;
  }
  for (const auto &account : parsed.value()) {
    [_accounts addObject:[[IPStoreAccount alloc] initWithAccount:account]];
  }
}

- (NSArray<IPStoreAccount *> *)accounts {
  return [_accounts copy];
}

- (nullable IPStoreAccount *)accountForEmail:(NSString *)email {
  for (IPStoreAccount *account in _accounts) {
    if ([account.email caseInsensitiveCompare:email] == NSOrderedSame) {
      return account;
    }
  }
  return nil;
}

- (nullable IPStoreAccount *)activeAccount {
  NSString *email = [[NSUserDefaults standardUserDefaults]
      stringForKey:@"IPActiveAccountEmail"];
  if (email.length == 0) return _accounts.firstObject;
  return [self accountForEmail:email] ?: _accounts.firstObject;
}

- (void)setActiveAccount:(nullable IPStoreAccount *)account {
  NSUserDefaults *defaults = [NSUserDefaults standardUserDefaults];
  if (account == nil) {
    [defaults removeObjectForKey:@"IPActiveAccountEmail"];
  } else {
    [defaults setObject:account.email forKey:@"IPActiveAccountEmail"];
  }
  [defaults synchronize];
}

- (BOOL)persist {
  std::vector<ipashelf::Account> accounts;
  accounts.reserve(_accounts.count);
  for (IPStoreAccount *account in _accounts) {
    accounts.push_back(account.coreAccount);
  }
  const std::string json = ipashelf::accounts_to_json(accounts);
  NSString *text = [NSString stringWithUTF8String:json.c_str()];
  NSError *error = nil;
  if (![IPKeychain setString:text ?: @"" forKey:@"accounts" error:&error]) {
    NSLog(@"IPAShelf: could not persist accounts: %@", error.localizedDescription);
    return NO;
  }
  return YES;
}

- (void)upsertAccount:(IPStoreAccount *)account {
  for (NSUInteger index = 0; index < _accounts.count; ++index) {
    if ([_accounts[index].email caseInsensitiveCompare:account.email] ==
        NSOrderedSame) {
      _accounts[index] = account;
      [self persist];
      return;
    }
  }
  [_accounts addObject:account];
  [self persist];
}

- (void)removeAccount:(IPStoreAccount *)account {
  for (NSUInteger index = 0; index < _accounts.count; ++index) {
    if ([_accounts[index].email caseInsensitiveCompare:account.email] ==
        NSOrderedSame) {
      [_accounts removeObjectAtIndex:index];
      break;
    }
  }
  if ([self.activeAccount.email caseInsensitiveCompare:account.email] ==
      NSOrderedSame) {
    [self setActiveAccount:_accounts.firstObject];
  }
  [self persist];
}

@end
