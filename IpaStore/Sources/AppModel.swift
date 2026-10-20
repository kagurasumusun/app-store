// SPDX-License-Identifier: MIT

import Foundation
import Observation
import UIKit

struct AlertMessage: Identifiable {
    let id = UUID()
    let title: String
    let message: String
}

@MainActor
@Observable
final class DownloadItem: Identifiable {
    enum State: Equatable {
        case downloading
        case processing
        case finished
        case failed
        case cancelled
    }

    let app: StoreApp
    let versionID: String?
    let startedAt = Date()

    /// Immutable and `Sendable`, so the `Identifiable` conformance does not
    /// cross the main actor (Swift 6 treats that as an error).
    let id: String

    var fraction: Double = 0
    var receivedBytes: Int64 = 0
    var expectedBytes: Int64 = 0
    var bytesPerSecond: Double = 0
    var state: State = .downloading
    var errorText: String?
    var savedPath: String?

    @ObservationIgnored private var lastSampleDate = Date()
    @ObservationIgnored private var lastSampleBytes: Int64 = 0

    init(app: StoreApp, versionID: String?) {
        self.app = app
        self.versionID = versionID
        self.id = "\(app.appID)-\(Date().timeIntervalSince1970)"
    }

    var progressLabel: String {
        if state == .processing { return "Verifying and patching…" }
        guard expectedBytes > 0 else {
            return ByteCountFormatter.string(fromByteCount: receivedBytes, countStyle: .file)
        }
        let received = ByteCountFormatter.string(fromByteCount: receivedBytes, countStyle: .file)
        let expected = ByteCountFormatter.string(fromByteCount: expectedBytes, countStyle: .file)
        return "\(received) of \(expected)"
    }

    var speedLabel: String {
        guard bytesPerSecond > 1 else { return "" }
        let speed = ByteCountFormatter.string(fromByteCount: Int64(bytesPerSecond),
                                              countStyle: .file)
        guard state == .downloading, fraction > 0.01 else { return "\(speed)/s" }
        let remaining = max(0, Double(expectedBytes - receivedBytes) / bytesPerSecond)
        if remaining < 1 { return "\(speed)/s" }
        let formatter = DateComponentsFormatter()
        formatter.allowedUnits = remaining > 90 ? [.minute, .second] : [.second]
        formatter.unitsStyle = .abbreviated
        let eta = formatter.string(from: remaining) ?? "\(Int(remaining))s"
        return "\(speed)/s · \(eta) left"
    }

    func update(received: Int64, expected: Int64, fraction: Double) {
        let now = Date()
        let interval = now.timeIntervalSince(lastSampleDate)
        if interval >= 0.5 {
            let delta = received - lastSampleBytes
            let instant = Double(delta) / interval
            bytesPerSecond = bytesPerSecond == 0 ? instant : bytesPerSecond * 0.7 + instant * 0.3
            lastSampleDate = now
            lastSampleBytes = received
        }
        receivedBytes = received
        expectedBytes = expected
        self.fraction = fraction
    }
}

/// The root tab identifiers. Kept as a fixed enum (rather than a computed list)
/// so the tab bar always has a selectable value: linked-on-iOS-27 `TabView`
/// traps when the selection does not match a visible tab.
enum AppTab: String, Hashable, CaseIterable, Identifiable {
    case search
    case purchased
    case downloads
    case accounts
    case settings

    var id: String { rawValue }

    var title: String {
        switch self {
        case .search: return "Search"
        case .purchased: return "Purchased"
        case .downloads: return "Downloads"
        case .accounts: return "Accounts"
        case .settings: return "Settings"
        }
    }

    var systemImage: String {
        switch self {
        case .search: return "magnifyingglass"
        case .purchased: return "bag"
        case .downloads: return "arrow.down.circle"
        case .accounts: return "person.crop.circle"
        case .settings: return "gearshape"
        }
    }
}

@MainActor
@Observable
final class AppModel {
    // Accounts
    var accounts: [StoreAccount] = []
    var activeAccount: StoreAccount?
    var isRefreshingToken = false

    // Catalog
    var platform: StorePlatform = .iPhone
    var region: String = "US"
    var storefronts: [StoreStorefront] = []

    var searchText = ""
    var filter = SearchFilter()
    var searchResults: [StoreApp] = []
    var isSearching = false
    var hasSearched = false
    var recentSearches: [String] = []

    // Details
    var versions: [Int64: [StoreVersion]] = [:]
    var versionErrors: [Int64: String] = [:]

    // Purchase history
    var ownedApps: [StoreApp] = []
    var ownedTotal = 0
    var ownedPage = 0
    var isLoadingOwned = false
    var ownedError: String?
    var ownedNeedsSignIn = false
    var hasLoadedOwned = false
    /// nil means "every platform".
    var ownedPlatformFilter: StorePlatform?
    var ownedSearch = ""

    // Downloads and downloaded files
    var downloads: [DownloadItem] = []
    var library: [LibraryItem] = []
    var librarySearch = ""

    // Installation
    var installService = InstallService()

    // Presentation
    var selectedTab: AppTab = .search
    var alert: AlertMessage?
    var showLogin = false

    let service = StoreService()

    var deviceIdentifier: String { service.deviceIdentifier }

    var appVersion: String {
        let version = Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String
        let build = Bundle.main.object(forInfoDictionaryKey: "CFBundleVersion") as? String
        return "\(version ?? "0.1.0") (\(build ?? "1"))"
    }

    var currentStorefront: StoreStorefront? {
        storefronts.first { $0.code.caseInsensitiveCompare(region) == .orderedSame }
    }

    var runningDownloadCount: Int {
        downloads.count { $0.state == .downloading || $0.state == .processing }
    }

    var visibleLibrary: [LibraryItem] {
        let needle = librarySearch.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !needle.isEmpty else { return library }
        return library.filter {
            $0.name.localizedCaseInsensitiveContains(needle)
                || $0.bundleID.localizedCaseInsensitiveContains(needle)
                || $0.version.localizedCaseInsensitiveContains(needle)
        }
    }

    var libraryTotalSize: Int64 {
        library.reduce(0) { $0 + $1.byteSize }
    }

    init() {
        storefronts = service.storefronts
        region = UserDefaults.standard.string(forKey: "IPAShelf.region") ?? "US"
        if let raw = UserDefaults.standard.string(forKey: "IPAShelf.platform"),
           let stored = StorePlatform(rawValue: raw) {
            platform = stored
        }
        recentSearches = UserDefaults.standard.stringArray(forKey: "IPAShelf.recentSearches") ?? []
        reloadAccounts()
        refreshLibrary()
        syncFilter()
    }

    // MARK: Accounts

    func reloadAccounts() {
        let previousEmail = activeAccount?.email
        accounts = service.accounts
        activeAccount = service.activeAccount
        if previousEmail != activeAccount?.email {
            // Version lists and purchase history are tied to the licences
            // of one account; never carry them across a switch.
            versions.removeAll()
            versionErrors.removeAll()
            ownedApps.removeAll()
            ownedTotal = 0
            ownedPage = 0
            ownedError = nil
            ownedNeedsSignIn = false
            hasLoadedOwned = false
        }
        if let account = activeAccount, !account.region.isEmpty {
            region = account.region
            syncFilter()
        }
    }

    func setActive(_ account: StoreAccount) {
        service.setActiveAccount(account)
        reloadAccounts()
    }

    func remove(_ account: StoreAccount) {
        service.removeAccount(account)
        reloadAccounts()
    }

    func login(email: String, password: String, authCode: String?, region: String) async -> LoginOutcome? {
        do {
            let outcome = try await service.login(email: email,
                                                 password: password,
                                                 authCode: authCode,
                                                 region: region)
            if case .signedIn = outcome {
                reloadAccounts()
            }
            return outcome
        } catch {
            present(error)
            return nil
        }
    }

    /// Rotates the account's password token with its stored password.
    /// Returns the refreshed account, `.twoFactorRequired`, or nil (the
    /// error has already been presented) when `presentError` is true.
    func refreshToken(for account: StoreAccount,
                      authCode: String? = nil,
                      presentError: Bool = true) async -> LoginOutcome? {
        guard !isRefreshingToken else { return nil }
        isRefreshingToken = true
        defer { isRefreshingToken = false }
        do {
            let outcome = try await service.refresh(account: account, authCode: authCode)
            if case .signedIn = outcome {
                reloadAccounts()
            }
            return outcome
        } catch {
            if presentError { present(error) }
            return nil
        }
    }

    /// Best-effort silent rotation used when an operation reports an expired
    /// token: succeeds only when no verification code is needed.
    private func refreshActiveSilently() async -> StoreAccount? {
        guard let account = activeAccount, account.hasSavedPassword else { return nil }
        let outcome = await refreshToken(for: account, presentError: false)
        if case .signedIn(let refreshed) = outcome {
            return refreshed
        }
        return nil
    }

    // MARK: Region and platform

    func setRegion(_ code: String) {
        region = code.uppercased()
        UserDefaults.standard.set(region, forKey: "IPAShelf.region")
        syncFilter()
    }

    func setPlatform(_ value: StorePlatform) {
        platform = value
        UserDefaults.standard.set(value.rawValue, forKey: "IPAShelf.platform")
        syncFilter()
    }

    func syncFilter() {
        filter.region = region
        filter.platform = platform
    }

    func regionName(_ code: String) -> String {
        storefronts.first { $0.code.caseInsensitiveCompare(code) == .orderedSame }?.name ?? code
    }

    // MARK: Search

    func search(term: String) async {
        isSearching = true
        defer { isSearching = false }
        filter.term = term
        filter.region = region
        filter.platform = platform
        do {
            let results = try await service.search(filter: filter)
            searchResults = results
            hasSearched = true
            remember(term: term)
        } catch {
            hasSearched = true
            present(error)
        }
    }

    private func remember(term: String) {
        var updated = recentSearches.filter { $0.caseInsensitiveCompare(term) != .orderedSame }
        updated.insert(term, at: 0)
        recentSearches = Array(updated.prefix(8))
        UserDefaults.standard.set(recentSearches, forKey: "IPAShelf.recentSearches")
    }

    func clearRecentSearches() {
        recentSearches = []
        UserDefaults.standard.removeObject(forKey: "IPAShelf.recentSearches")
    }

    func clearSearch() {
        searchText = ""
        searchResults = []
        hasSearched = false
    }

    // MARK: Details

    func loadVersions(for app: StoreApp, force: Bool = false) async {
        if !force, versions[app.appID] != nil { return }
        guard var account = activeAccount else {
            versionErrors[app.appID] = "Sign in to list every version Apple has issued for this app."
            return
        }
        do {
            let list = try await service.listVersions(app: app,
                                                      account: account,
                                                      platform: platform)
            versions[app.appID] = list
            versionErrors[app.appID] = nil
        } catch {
            let storeError = StoreError(error)
            if storeError.code == .tokenExpired,
               let refreshed = await refreshActiveSilently() {
                account = refreshed
                if let list = try? await service.listVersions(app: app,
                                                              account: account,
                                                              platform: platform) {
                    versions[app.appID] = list
                    versionErrors[app.appID] = nil
                    return
                }
            }
            versionErrors[app.appID] = storeError.message
        }
    }

    /// Fills in a build's marketing version and release date from the pinned
    /// download response. Called lazily as rows appear; failures simply
    /// leave the raw build identifier in place.
    func enrichVersion(_ version: StoreVersion, for app: StoreApp) async {
        guard version.displayVersion.isEmpty, let account = activeAccount else { return }
        guard let metadata = try? await service.versionMetadata(
            app: app,
            account: account,
            externalVersionID: version.externalVersionID,
            platform: platform) else { return }
        guard var list = versions[app.appID],
              let index = list.firstIndex(where: {
                  $0.externalVersionID == version.externalVersionID
              }) else { return }
        list[index].displayVersion = metadata.displayVersion
        list[index].releaseDate = metadata.releaseDate
        versions[app.appID] = list
    }

    func purchase(_ app: StoreApp) async {
        guard let account = activeAccount else {
            showLogin = true
            return
        }
        do {
            try await service.purchase(app: app, account: account, platform: platform)
            presentMessage("Purchased", "\(app.name) was added to \(account.email).")
            ownedPage = 0
            hasLoadedOwned = false
        } catch {
            let storeError = StoreError(error)
            if storeError.code == .tokenExpired,
               let refreshed = await refreshActiveSilently() {
                do {
                    try await service.purchase(app: app, account: refreshed, platform: platform)
                    presentMessage("Purchased", "\(app.name) was added to \(refreshed.email).")
                    return
                } catch {
                    present(error)
                    return
                }
            }
            present(error)
        }
    }

    // MARK: Purchase history

    var ownedHasMore: Bool { ownedApps.count < ownedTotal }

    var visibleOwnedApps: [StoreApp] {
        let needle = ownedSearch.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !needle.isEmpty else { return ownedApps }
        return ownedApps.filter {
            $0.name.localizedCaseInsensitiveContains(needle)
                || $0.bundleID.localizedCaseInsensitiveContains(needle)
        }
    }

    func isOwned(_ appID: Int64) -> Bool {
        ownedApps.contains { $0.appID == appID }
    }

    func loadOwned(reset: Bool = false) async {
        guard var account = activeAccount else {
            ownedApps = []
            ownedTotal = 0
            ownedPage = 0
            ownedError = nil
            ownedNeedsSignIn = true
            hasLoadedOwned = false
            return
        }
        if isLoadingOwned { return }
        if !reset, hasLoadedOwned, !ownedHasMore { return }
        isLoadingOwned = true
        defer { isLoadingOwned = false }
        let nextPage = reset ? 1 : ownedPage + 1
        do {
            let result: StoreOwnedAppsPage
            do {
                result = try await service.ownedApps(account: account,
                                                      platform: ownedPlatformFilter,
                                                      page: nextPage,
                                                      limit: 100)
            } catch {
                // The purchase-history endpoint rejects an expired token
                // the same way as the others; rotate once and retry.
                guard StoreError(error).code == .tokenExpired,
                      let refreshed = await refreshActiveSilently() else {
                    throw error
                }
                account = refreshed
                result = try await service.ownedApps(account: account,
                                                      platform: ownedPlatformFilter,
                                                      page: nextPage,
                                                      limit: 100)
            }
            if reset {
                ownedApps = result.apps
            } else {
                let known = Set(ownedApps.map(\.appID))
                ownedApps.append(contentsOf: result.apps.filter { !known.contains($0.appID) })
            }
            ownedTotal = result.total
            ownedPage = result.page
            ownedError = nil
            ownedNeedsSignIn = false
            hasLoadedOwned = true
        } catch {
            let storeError = StoreError(error)
            ownedError = storeError.message
            ownedNeedsSignIn = storeError.isSignInProblem
        }
    }

    func setOwnedPlatformFilter(_ platform: StorePlatform?) {
        ownedPlatformFilter = platform
        ownedPage = 0
        hasLoadedOwned = false
        Task { await loadOwned(reset: true) }
    }

    // MARK: Downloads

    func download(_ app: StoreApp, versionID: String? = nil) {
        guard let account = activeAccount else {
            showLogin = true
            return
        }
        let item = DownloadItem(app: app, versionID: versionID)
        downloads.insert(item, at: 0)
        selectedTab = .downloads

        Task {
            await runDownload(item, account: account, allowTokenRefresh: true)
        }
    }

    private func runDownload(_ item: DownloadItem,
                             account: StoreAccount,
                             allowTokenRefresh: Bool) async {
        // A foreground URLSession transfer dies when the process is
        // suspended; hold a background assertion so locking the screen or
        // switching apps briefly does not kill a multi-minute download the
        // moment it starts. The system decides how much time that buys.
        var backgroundTask = UIBackgroundTaskIdentifier.invalid
        backgroundTask = UIApplication.shared.beginBackgroundTask(
            withName: "IPAShelf download") {
            guard backgroundTask != .invalid else { return }
            UIApplication.shared.endBackgroundTask(backgroundTask)
            backgroundTask = .invalid
        }
        defer {
            if backgroundTask != .invalid {
                UIApplication.shared.endBackgroundTask(backgroundTask)
            }
        }
        do {
            let url = try await service.download(
                app: item.app,
                account: account,
                externalVersionID: item.versionID,
                platform: platform,
                progress: { fraction, received, expected in
                    Task { @MainActor in
                        item.update(received: received, expected: expected, fraction: fraction)
                        if fraction >= 1.0 { item.state = .processing }
                    }
                })
            item.savedPath = url.path
            item.state = .finished
            refreshLibrary()
        } catch {
            let storeError = StoreError(error)
            if storeError.code == .cancelled {
                item.state = .cancelled
                return
            }
            if storeError.code == .tokenExpired, allowTokenRefresh,
               let refreshed = await refreshActiveSilently() {
                item.errorText = nil
                await runDownload(item, account: refreshed, allowTokenRefresh: false)
                return
            }
            item.state = .failed
            item.errorText = storeError.message
            present(error)
        }
    }

    func retry(_ item: DownloadItem) {
        downloads.removeAll { $0.id == item.id }
        download(item.app, versionID: item.versionID)
    }

    /// Cancels a single transfer. Also reaches downloads whose store offer
    /// is still being negotiated: the bridge remembers the request and
    /// stops the transfer before it starts.
    func cancelDownload(_ item: DownloadItem) {
        service.cancelDownload(appID: item.app.appID,
                               externalVersionID: item.versionID)
        if item.state == .downloading || item.state == .processing {
            item.state = .cancelled
        }
    }

    func cancelDownloads() {
        for item in downloads
        where item.state == .downloading || item.state == .processing {
            service.cancelDownload(appID: item.app.appID,
                                   externalVersionID: item.versionID)
            item.state = .cancelled
        }
        service.cancelDownloads()
    }

    func clearFinishedDownloads() {
        downloads.removeAll { $0.state == .finished || $0.state == .cancelled || $0.state == .failed }
    }

    // MARK: Downloaded files

    func refreshLibrary() {
        let directory = service.libraryDirectory()
        let contents = (try? FileManager.default.contentsOfDirectory(
            at: directory,
            includingPropertiesForKeys: [.contentModificationDateKey],
            options: [.skipsHiddenFiles])) ?? []
        library = contents
            .filter { $0.pathExtension.lowercased() == "ipa" }
            .compactMap(LibraryItem.init(ipaURL:))
            .sorted { $0.downloadedAt > $1.downloadedAt }
    }

    func delete(_ item: LibraryItem) {
        try? FileManager.default.removeItem(at: item.fileURL)
        try? FileManager.default.removeItem(at: item.fileURL.appendingPathExtension("json"))
        if installService.item?.id == item.id {
            installService.stop()
        }
        refreshLibrary()
    }

    func deleteAllLibraryItems() {
        for item in library {
            try? FileManager.default.removeItem(at: item.fileURL)
            try? FileManager.default.removeItem(at: item.fileURL.appendingPathExtension("json"))
        }
        refreshLibrary()
    }

    func libraryItem(for download: DownloadItem) -> LibraryItem? {
        guard let path = download.savedPath else { return nil }
        return library.first { $0.fileURL.path == path }
    }

    /// Copies an externally provided IPA (Files app, share sheet) into the
    /// download folder so it can be installed or shared like a download.
    func importIPA(from url: URL) {
        let scoped = url.startAccessingSecurityScopedResource()
        defer {
            if scoped { url.stopAccessingSecurityScopedResource() }
        }
        let directory = service.libraryDirectory()
        var destination = directory.appendingPathComponent(url.lastPathComponent)
        var counter = 1
        while FileManager.default.fileExists(atPath: destination.path) {
            let stem = url.deletingPathExtension().lastPathComponent
            destination = directory.appendingPathComponent("\(stem) (\(counter)).ipa")
            counter += 1
        }
        do {
            try FileManager.default.copyItem(at: url, to: destination)
            refreshLibrary()
            selectedTab = .downloads
        } catch {
            present(error)
        }
    }

    // MARK: Installation

    func install(_ item: LibraryItem) {
        installService.start(item: item)
        installService.isPresented = true
    }

    // MARK: Errors

    func present(_ error: Error) {
        let storeError = StoreError(error)
        alert = AlertMessage(title: title(for: storeError), message: storeError.message)
    }

    func presentMessage(_ title: String, _ message: String) {
        alert = AlertMessage(title: title, message: message)
    }

    private func title(for error: StoreError) -> String {
        switch error.code {
        case .signerUnavailable: return "Signed-in features unavailable"
        case .unauthorized, .tokenExpired: return "Sign in again"
        case .network: return "Network error"
        case .appNotFound: return "App not found"
        case .licenseRequired: return "Purchase required"
        case .unsupported: return "Not supported"
        case .storage, .integrity: return "Could not save the package"
        default: return "Something went wrong"
        }
    }
}
