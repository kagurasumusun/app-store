// SPDX-License-Identifier: MIT

import Foundation

/// Swift-facing wrapper around the Objective-C++ bridge. Every call runs on the
/// bridge's serial queue and resumes the caller on the main actor because the
/// Objective-C facade delivers completions on the main queue.
///
/// `@unchecked Sendable` is sound here: the only stored property is the
/// bridge singleton, which serializes all work on its own queue; this class
/// itself keeps no mutable state.
final class StoreService: @unchecked Sendable {
    private let bridge = IPStoreService.shared

    var accounts: [StoreAccount] {
        bridge.accounts.map(StoreAccount.init(handle:))
    }

    var activeAccount: StoreAccount? {
        bridge.activeAccount.map(StoreAccount.init(handle:))
    }

    var deviceIdentifier: String {
        bridge.deviceIdentifier
    }

    var storefronts: [StoreStorefront] {
        IPStoreService.storefronts().map {
            StoreStorefront(code: $0.code,
                            name: $0.name,
                            storefrontID: $0.storefrontID,
                            currency: $0.currency)
        }
    }

    func storefront(for region: String) -> StoreStorefront? {
        storefronts.first { $0.code.caseInsensitiveCompare(region) == .orderedSame }
    }

    func setActiveAccount(_ account: StoreAccount) {
        bridge.setActiveAccount(account.handle)
    }

    func removeAccount(_ account: StoreAccount) {
        bridge.removeAccount(account.handle)
    }

    func login(email: String,
               password: String,
               authCode: String?,
               region: String) async throws -> LoginOutcome {
        try await withCheckedThrowingContinuation { continuation in
            bridge.login(email: email,
                         password: password,
                         authCode: authCode,
                         region: region) { result, error in
                if let error {
                    continuation.resume(throwing: StoreError(error))
                    return
                }
                guard let result else {
                    continuation.resume(throwing: StoreError(
                        code: .invalidResponse,
                        message: "Apple returned an empty sign-in response."))
                    return
                }
                if result.twoFactorRequired {
                    continuation.resume(returning: .twoFactorRequired)
                } else if let account = result.account {
                    continuation.resume(returning: .signedIn(StoreAccount(handle: account)))
                } else {
                    continuation.resume(throwing: StoreError(
                        code: .invalidResponse,
                        message: "Apple did not return an account."))
                }
            }
        }
    }

    /// Rotates the account's password token using its stored password.
    /// Returns `.twoFactorRequired` when Apple wants a fresh code.
    func refresh(account: StoreAccount, authCode: String?) async throws -> LoginOutcome {
        try await withCheckedThrowingContinuation { continuation in
            bridge.refresh(account: account.handle,
                           authCode: authCode) { result, error in
                if let error {
                    continuation.resume(throwing: StoreError(error))
                    return
                }
                guard let result else {
                    continuation.resume(throwing: StoreError(
                        code: .invalidResponse,
                        message: "Apple returned an empty sign-in response."))
                    return
                }
                if result.twoFactorRequired {
                    continuation.resume(returning: .twoFactorRequired)
                } else if let account = result.account {
                    continuation.resume(returning: .signedIn(StoreAccount(handle: account)))
                } else {
                    continuation.resume(throwing: StoreError(
                        code: .invalidResponse,
                        message: "Apple did not return an account."))
                }
            }
        }
    }

    /// Acquires the app's license without downloading it. Free apps only;
    /// already-owned apps succeed.
    func purchase(app: StoreApp,
                  account: StoreAccount,
                  platform: StorePlatform) async throws {
        guard let handle = app.handle else {
            throw StoreError(code: .invalidResponse, message: "Missing app handle.")
        }
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            bridge.purchase(app: handle,
                            account: account.handle,
                            platform: platform.bridge) { error in
                if let error {
                    continuation.resume(throwing: StoreError(error))
                } else {
                    continuation.resume(returning: Void())
                }
            }
        }
    }

    func search(term: String,
                region: String,
                platform: StorePlatform,
                limit: Int) async throws -> [StoreApp] {
        var filter = SearchFilter()
        filter.term = term
        filter.region = region
        filter.platform = platform
        filter.limit = limit
        return try await search(filter: filter)
    }

    func search(filter: SearchFilter) async throws -> [StoreApp] {
        let request = IPStoreSearchFilter()
        request.term = filter.term
        request.region = filter.region
        request.platform = filter.platform.bridge
        request.limit = filter.limit
        request.genreID = filter.genreID
        request.sort = filter.sort.rawValue
        if let freeOnly = filter.freeOnly {
            request.freeOnly = NSNumber(value: freeOnly)
        }
        return try await withCheckedThrowingContinuation { continuation in
            bridge.search(filter: request) { apps, error in
                if let error {
                    continuation.resume(throwing: StoreError(error))
                } else {
                    continuation.resume(returning: (apps ?? []).map(StoreApp.init(handle:)))
                }
            }
        }
    }

    func lookup(appID: Int64,
                bundleID: String?,
                region: String,
                platform: StorePlatform) async throws -> StoreApp {
        try await withCheckedThrowingContinuation { continuation in
            bridge.lookup(appID: appID,
                          bundleID: bundleID,
                          region: region,
                          platform: platform.bridge) { app, error in
                if let error {
                    continuation.resume(throwing: StoreError(error))
                } else if let app {
                    continuation.resume(returning: StoreApp(handle: app))
                } else {
                    continuation.resume(throwing: StoreError(
                        code: .appNotFound,
                        message: "The app was not found in this storefront."))
                }
            }
        }
    }

    func listVersions(app: StoreApp,
                      account: StoreAccount,
                      platform: StorePlatform) async throws -> [StoreVersion] {
        guard let handle = app.handle else {
            throw StoreError(code: .invalidResponse, message: "Missing app handle.")
        }
        return try await withCheckedThrowingContinuation { continuation in
            bridge.listVersions(app: handle,
                                account: account.handle,
                                platform: platform.bridge) { versions, error in
                if let error {
                    continuation.resume(throwing: StoreError(error))
                } else {
                    continuation.resume(returning: (versions ?? []).map(StoreVersion.init(handle:)))
                }
            }
        }
    }

    /// Reads one build's marketing version and release date from the
    /// version-pinned download response (the package is not downloaded).
    func versionMetadata(app: StoreApp,
                         account: StoreAccount,
                         externalVersionID: String,
                         platform: StorePlatform) async throws -> StoreVersionMetadata {
        guard let handle = app.handle else {
            throw StoreError(code: .invalidResponse, message: "Missing app handle.")
        }
        return try await withCheckedThrowingContinuation { continuation in
            bridge.versionMetadata(app: handle,
                                   account: account.handle,
                                   externalVersionID: externalVersionID,
                                   platform: platform.bridge) { metadata, error in
                if let error {
                    continuation.resume(throwing: StoreError(error))
                } else if let metadata {
                    continuation.resume(returning: StoreVersionMetadata(
                        displayVersion: metadata.displayVersion,
                        releaseDate: metadata.releaseDate))
                } else {
                    continuation.resume(throwing: StoreError(
                        code: .invalidResponse,
                        message: "Apple did not return version metadata."))
                }
            }
        }
    }

    /// One page of the account's purchase history, newest purchase first.
    /// `platform` nil lists every platform.
    func ownedApps(account: StoreAccount,
                   platform: StorePlatform?,
                   page: Int,
                   limit: Int) async throws -> StoreOwnedAppsPage {
        let bridgePlatform: IPStorePlatform = platform?.bridge ?? IPStorePlatform.`any`
        return try await withCheckedThrowingContinuation { continuation in
            bridge.ownedApps(account: account.handle,
                             platform: bridgePlatform,
                             page: page,
                             limit: limit) { page, error in
                if let error {
                    continuation.resume(throwing: StoreError(error))
                } else if let page {
                    continuation.resume(returning: StoreOwnedAppsPage(
                        apps: page.apps.map(StoreApp.init(handle:)),
                        total: Int(page.total),
                        page: Int(page.page),
                        limit: Int(page.limit)))
                } else {
                    continuation.resume(throwing: StoreError(
                        code: .invalidResponse,
                        message: "Apple did not return the purchase history."))
                }
            }
        }
    }

    func download(app: StoreApp,
                  account: StoreAccount,
                  externalVersionID: String?,
                  platform: StorePlatform,
                  progress: @escaping @Sendable (Double, Int64, Int64) -> Void) async throws -> URL {
        guard let handle = app.handle else {
            throw StoreError(code: .invalidResponse, message: "Missing app handle.")
        }
        return try await withCheckedThrowingContinuation { continuation in
            bridge.download(app: handle,
                            account: account.handle,
                            externalVersionID: externalVersionID,
                            platform: platform.bridge,
                            progress: { fraction, received, expected in
                                progress(fraction, received, expected)
                            }) { path, error in
                if let error {
                    continuation.resume(throwing: StoreError(error))
                } else if let path {
                    continuation.resume(returning: URL(fileURLWithPath: path))
                } else {
                    continuation.resume(throwing: StoreError(
                        code: .storage,
                        message: "The download finished without a file."))
                }
            }
        }
    }

    func cancelDownloads() {
        bridge.cancelDownloads()
    }

    func cancelDownload(appID: Int64, externalVersionID: String?) {
        bridge.cancelDownload(appID: appID, externalVersionID: externalVersionID)
    }

    func libraryDirectory() -> URL {
        URL(fileURLWithPath: IPStoreLibraryDirectory(), isDirectory: true)
    }
}
