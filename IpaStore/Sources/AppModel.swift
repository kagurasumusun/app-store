// SPDX-License-Identifier: MIT

import Foundation
import Observation

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
    case downloads
    case library
    case wishlist
    case settings

    var id: String { rawValue }

    var title: String {
        switch self {
        case .search: return "Search"
        case .downloads: return "Downloads"
        case .library: return "Library"
        case .wishlist: return "Wishlist"
        case .settings: return "Settings"
        }
    }

    var systemImage: String {
        switch self {
        case .search: return "magnifyingglass"
        case .downloads: return "arrow.down.circle"
        case .library: return "square.stack.3d.up.fill"
        case .wishlist: return "heart"
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
    var selectedVersion: [Int64: String] = [:]
    var reviews: [Int64: [StoreReview]] = [:]
    var reviewPages: [Int64: Int] = [:]
    var reviewHasMore: [Int64: Bool] = [:]
    var isLoadingReviews: Set<Int64> = []

    // Downloads and library
    var downloads: [DownloadItem] = []
    var library: [LibraryItem] = []
    var librarySearch = ""
    var librarySort: LibrarySort = .name

    // Wishlist
    var wishlist: [WishlistEntry] = []

    // Installation
    var installService = InstallService()

    // Presentation
    var selectedTab: AppTab = .search
    var alert: AlertMessage?
    var showLogin = false
    var showAccounts = false
    var showRegionPicker = false

    let service = StoreService()

    enum LibrarySort: String, CaseIterable, Identifiable {
        case name
        case recent
        case size

        var id: String { rawValue }
        var title: String {
            switch self {
            case .name: return "Name"
            case .recent: return "Recent"
            case .size: return "Size"
            }
        }
    }

    var deviceIdentifier: String { service.deviceIdentifier }

    var appVersion: String {
        let version = Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String
        let build = Bundle.main.object(forInfoDictionaryKey: "CFBundleVersion") as? String
        return "\(version ?? "0.1.0") (\(build ?? "1"))"
    }

    var currentStorefront: StoreStorefront? {
        storefronts.first { $0.code.caseInsensitiveCompare(region) == .orderedSame }
    }

    var libraryGroups: [LibraryGroup] {
        let filtered: [LibraryItem]
        let needle = librarySearch.trimmingCharacters(in: .whitespacesAndNewlines)
        if needle.isEmpty {
            filtered = library
        } else {
            filtered = library.filter {
                $0.name.localizedCaseInsensitiveContains(needle)
                    || $0.bundleID.localizedCaseInsensitiveContains(needle)
                    || $0.version.localizedCaseInsensitiveContains(needle)
            }
        }
        let groups = LibraryGroup.groups(from: filtered)
        switch librarySort {
        case .name:
            return groups
        case .recent:
            return groups.sorted {
                ($0.latest?.downloadedAt ?? .distantPast) > ($1.latest?.downloadedAt ?? .distantPast)
            }
        case .size:
            return groups.sorted { $0.totalSize > $1.totalSize }
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
        loadWishlist()
        reloadAccounts()
        refreshLibrary()
        syncFilter()
    }

    // MARK: Accounts

    func reloadAccounts() {
        accounts = service.accounts
        activeAccount = service.activeAccount
        if let account = activeAccount, !account.region.isEmpty {
            region = account.region
            syncFilter()
        }
    }

    func setActive(_ account: StoreAccount) {
        service.setActiveAccount(account)
        reloadAccounts()
        presentMessage("Signed in", "New downloads use \(account.email).")
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

    func searchIfNeeded() async {
        let term = searchText.trimmingCharacters(in: .whitespacesAndNewlines)
        guard term.count >= 2 else {
            searchResults = []
            hasSearched = false
            return
        }
        try? await Task.sleep(for: .milliseconds(350))
        guard !Task.isCancelled else { return }
        await search(term: term)
    }

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

    func applyFilter(_ update: (inout SearchFilter) -> Void) {
        update(&filter)
        filter.region = region
        filter.platform = platform
        let term = searchText.trimmingCharacters(in: .whitespacesAndNewlines)
        guard term.count >= 2 else { return }
        Task { await search(term: term) }
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
        guard let account = activeAccount else {
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
            versionErrors[app.appID] = StoreError(error).message
        }
    }

    func latestVersion(for app: StoreApp) async -> String? {
        do {
            return try await service.latestExternalVersion(app: app,
                                                           region: region,
                                                           platform: platform)
        } catch {
            return nil
        }
    }

    func loadReviews(for app: StoreApp, nextPage: Bool = false) async {
        if isLoadingReviews.contains(app.appID) { return }
        let page = nextPage ? (reviewPages[app.appID] ?? 1) + 1 : 1
        if nextPage, reviewHasMore[app.appID] == false { return }
        isLoadingReviews.insert(app.appID)
        defer { isLoadingReviews.remove(app.appID) }
        do {
            let result = try await service.reviews(appID: app.appID,
                                                   region: region,
                                                   page: page)
            if nextPage {
                reviews[app.appID, default: []].append(contentsOf: result.reviews)
            } else {
                reviews[app.appID] = result.reviews
            }
            reviewPages[app.appID] = result.page
            reviewHasMore[app.appID] = result.hasMore
        } catch {
            // Review feeds are optional: an app may have none, or the
            // storefront may not expose one.
            if !nextPage {
                reviews[app.appID] = []
                reviewHasMore[app.appID] = false
            }
        }
    }

    // MARK: Downloads

    func download(_ app: StoreApp, versionID: String? = nil) {
        guard let account = activeAccount else {
            showLogin = true
            return
        }
        let item = DownloadItem(app: app, versionID: versionID)
        downloads.insert(item, at: 0)

        Task {
            do {
                let url = try await service.download(
                    app: app,
                    account: account,
                    externalVersionID: versionID,
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
                } else {
                    item.state = .failed
                    item.errorText = storeError.message
                    present(error)
                }
            }
        }
    }

    func retry(_ item: DownloadItem) {
        downloads.removeAll { $0.id == item.id }
        download(item.app, versionID: item.versionID)
    }

    func cancelDownloads() {
        service.cancelDownloads()
        for item in downloads where item.state == .downloading {
            item.state = .cancelled
        }
    }

    func clearFinishedDownloads() {
        downloads.removeAll { $0.state == .finished || $0.state == .cancelled || $0.state == .failed }
    }

    // MARK: Library

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

    func delete(_ group: LibraryGroup) {
        for item in group.items { delete(item) }
    }

    func libraryItem(for download: DownloadItem) -> LibraryItem? {
        guard let path = download.savedPath else { return nil }
        return library.first { $0.fileURL.path == path }
    }

    // MARK: Installation

    func install(_ item: LibraryItem) {
        installService.start(item: item)
        installService.isPresented = true
    }

    func presentInstallSheet() {
        installService.prepareCertificate()
        installService.isPresented = true
    }

    // MARK: Wishlist

    func isWishlisted(_ app: StoreApp) -> Bool {
        wishlist.contains { $0.appID == app.appID }
    }

    func toggleWishlist(_ app: StoreApp) {
        if let index = wishlist.firstIndex(where: { $0.appID == app.appID }) {
            wishlist.remove(at: index)
        } else {
            wishlist.insert(WishlistEntry(app: app), at: 0)
        }
        saveWishlist()
    }

    func removeWishlist(appID: Int64) {
        wishlist.removeAll { $0.appID == appID }
        saveWishlist()
    }

    func clearWishlist() {
        wishlist.removeAll()
        saveWishlist()
    }

    private func loadWishlist() {
        guard let data = UserDefaults.standard.data(forKey: "IPAShelf.wishlist"),
              let decoded = try? JSONDecoder().decode([WishlistEntry].self, from: data) else {
            return
        }
        wishlist = decoded
    }

    private func saveWishlist() {
        guard let data = try? JSONEncoder().encode(wishlist) else { return }
        UserDefaults.standard.set(data, forKey: "IPAShelf.wishlist")
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
