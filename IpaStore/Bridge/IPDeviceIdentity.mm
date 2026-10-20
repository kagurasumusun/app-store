// SPDX-License-Identifier: MIT

#import <Security/Security.h>

#import "IPInternal.h"

ipashelf::HardwareIdentity IPHardwareIdentity(void) {
  static ipashelf::HardwareIdentity identity;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    NSString *stored = [IPKeychain stringForKey:@"hardware-identity"];
    if (stored.length > 0) {
      auto parsed = ipashelf::HardwareIdentity::from_hex(
          ipashelf::string_from_bytes(
              ipashelf::bytes_from_string(stored.UTF8String ?: "")));
      if (parsed && parsed.value().bytes.size() == 6) {
        identity = parsed.value();
        return;
      }
      NSLog(@"IPAShelf: stored hardware identity is invalid; regenerating");
    }
    identity = ipashelf::HardwareIdentity::random();
    NSString *hex = [NSString
        stringWithUTF8String:ipashelf::hex_encode(identity.bytes, true).c_str()];
    NSError *error = nil;
    if (![IPKeychain setString:hex ?: @"" forKey:@"hardware-identity" error:&error]) {
      NSLog(@"IPAShelf: could not persist hardware identity to Keychain: %@. "
            "Unsigned builds cannot access the Keychain, so SAP signing will "
            "fail on every launch. Sign the app with a development/distribution "
            "identity before testing sign-in on a real device.",
            error.localizedDescription);
    } else {
      NSLog(@"IPAShelf: hardware identity persisted (guid=%@)",
            [NSString stringWithUTF8String:identity.guid().c_str()]);
    }
  });
  return identity;
}

NSURL *IPStoreLibraryURL(void) {
  NSFileManager *manager = [NSFileManager defaultManager];
  NSURL *documents = [manager URLsForDirectory:NSDocumentDirectory
                                     inDomains:NSUserDomainMask]
                         .firstObject;
  NSURL *library = [documents URLByAppendingPathComponent:@"IPAs" isDirectory:YES];
  NSError *error = nil;
  if (![manager createDirectoryAtURL:library
         withIntermediateDirectories:YES
                          attributes:nil
                               error:&error]) {
    NSLog(@"IPAShelf: could not create library directory: %@",
          error.localizedDescription);
  }
  [library setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:nil];
  return library;
}

NSString *IPStoreLibraryDirectory(void) {
  return IPStoreLibraryURL().path ?: @"";
}
