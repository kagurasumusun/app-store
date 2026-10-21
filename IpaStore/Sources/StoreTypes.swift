// SPDX-License-Identifier: MIT

import Foundation

// MARK: - Platform

enum StorePlatform: String, CaseIterable, Identifiable {
    case iPhone
    case iPad
    case mac
    case tv
    case watch
    case vision

    var id: String { rawValue }

    var title: String {
        switch self {
        case .iPhone: return "iPhone"
        case .iPad: return "iPad"
        case .mac: return "Mac"
        case .tv: return "Apple TV"
        case .watch: return "Apple Watch"
        case .vision: return "Vision Pro"
        }
    }

    var symbol: String {
        switch self {
        case .iPhone: return "iphone"
        case .iPad: return "ipad"
        case .mac: return "macbook"
        case .tv: return "appletv"
        case .watch: return "applewatch"
        case .vision: return "visionpro"
        }
    }

    var bridge: IPStorePlatform {
        IPStorePlatform(rawValue: bridgeRawValue)!
    }

    /// Platform names as reported by the public catalog.
    var catalogName: String {
        switch self {
        case .iPhone: return "iphone"
        case .iPad: return "ipad"
        case .mac: return "mac"
        case .tv: return "appletv"
        case .watch: return "watch"
        case .vision: return "vision"
        }
    }

    private var bridgeRawValue: Int {
        switch self {
        case .iPhone: return 0
        case .iPad: return 1
        case .mac: return 2
        case .tv: return 3
        case .watch: return 4
        case .vision: return 5
        }
    }
}

// MARK: - Catalog

struct StoreApp: Identifiable, Hashable {
    let appID: Int64
    let bundleID: String
    let name: String
    let version: String
    let seller: String
    let artistName: String
    let sellerURL: URL?
    let artworkURL: URL?
    let description: String
    let releaseNotes: String
    let releaseDate: String
    let currentVersionReleaseDate: String
    let minimumOSVersion: String
    let primaryGenre: String
    let contentAdvisoryRating: String
    let currency: String
    let trackViewURL: URL?
    let languageCodes: [String]
    let screenshots: [URL]
    let iPadScreenshots: [URL]
    let supportedDevices: [String]
    let platforms: [String]
    let averageRating: Double
    let ratingCount: Int
    let currentVersionAverageRating: Double
    let currentVersionRatingCount: Int
    let byteSize: Int64
    let price: Double
    let hasInAppPurchases: Bool
    let isUniversal: Bool
    /// When the signed-in account acquired the app (purchase history only).
    let purchaseDate: Date?

    let handle: IPStoreApp?

    var id: Int64 { appID }
    var isFree: Bool { price <= 0.0001 }

    var priceLabel: String {
        guard !isFree else { return String(localized: "Free") }
        guard let formatted = formattedPrice else {
            return "\(Int(price.rounded())) \(currency)"
        }
        return formatted
    }

    private var formattedPrice: String? {
        let formatter = NumberFormatter()
        formatter.numberStyle = .currency
        formatter.currencyCode = currency
        guard let text = formatter.string(from: NSNumber(value: price)) else { return nil }
        return text
    }

    var ratingLabel: String {
        guard ratingCount > 0 else { return String(localized: "No ratings") }
        return String(format: "%.1f", averageRating)
    }

    /// Compact count only ("1.2K"); views add the localized unit.
    var ratingCountCompact: String {
        if ratingCount >= 100_000 {
            return "\(ratingCount / 1_000)K"
        }
        if ratingCount >= 1_000 {
            return String(format: "%.1fK", Double(ratingCount) / 1_000)
        }
        return "\(ratingCount)"
    }

    var ratingCountLabel: String {
        String(localized: "\(ratingCountCompact) ratings")
    }

    var sizeLabel: String {
        guard byteSize > 0 else { return "—" }
        return ByteCountFormatter.string(fromByteCount: byteSize, countStyle: .file)
    }

    /// Localized abbreviated date for the raw ISO timestamps the store
    /// returns, e.g. "Oct 20, 2026" instead of "2026-10-20".
    var releaseDateFormatted: String { Self.abbreviatedDate(releaseDate) }
    var currentVersionDateFormatted: String {
        Self.abbreviatedDate(currentVersionReleaseDate)
    }

    private static func abbreviatedDate(_ raw: String) -> String {
        guard !raw.isEmpty else { return raw }
        let formatter = ISO8601DateFormatter()
        formatter.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
        var parsed = formatter.date(from: raw)
        if parsed == nil {
            formatter.formatOptions = [.withInternetDateTime]
            parsed = formatter.date(from: raw)
        }
        guard let parsed else { return String(raw.prefix(10)) }
        return parsed.formatted(date: .abbreviated, time: .omitted)
    }

    var advisoryLabel: String {
        contentAdvisoryRating.isEmpty ? "—" : contentAdvisoryRating
    }

    var gallery: [URL] {
        screenshots.isEmpty ? iPadScreenshots : screenshots
    }

    var supportsIPad: Bool {
        platforms.contains("ipad") || !iPadScreenshots.isEmpty || isUniversal
    }

    init(handle: IPStoreApp) {
        self.handle = handle
        appID = handle.appID
        bundleID = handle.bundleID
        name = handle.name
        version = handle.version
        seller = handle.seller
        artistName = handle.artistName
        sellerURL = URL(string: handle.sellerURL)
        artworkURL = URL(string: handle.artworkURL)
        description = handle.appDescription
        releaseNotes = handle.releaseNotes
        releaseDate = handle.releaseDate
        currentVersionReleaseDate = handle.currentVersionReleaseDate
        minimumOSVersion = handle.minimumOSVersion
        primaryGenre = handle.primaryGenre
        contentAdvisoryRating = handle.contentAdvisoryRating
        currency = handle.currency
        trackViewURL = URL(string: handle.trackViewURL)
        languageCodes = handle.languageCodes
            .split(separator: ",")
            .map { String($0).trimmingCharacters(in: .whitespaces) }
            .filter { !$0.isEmpty }
        screenshots = handle.screenshots.compactMap(URL.init(string:))
        iPadScreenshots = handle.iPadScreenshots.compactMap(URL.init(string:))
        supportedDevices = handle.supportedDevices
        platforms = handle.platforms
        averageRating = handle.averageRating
        ratingCount = Int(handle.ratingCount)
        currentVersionAverageRating = handle.currentVersionAverageRating
        currentVersionRatingCount = Int(handle.currentVersionRatingCount)
        byteSize = handle.byteSize
        price = handle.price
        hasInAppPurchases = handle.hasInAppPurchases
        isUniversal = handle.isUniversal
        purchaseDate = handle.purchaseDate
    }

    init(appID: Int64, bundleID: String, name: String, version: String) {
        handle = nil
        self.appID = appID
        self.bundleID = bundleID
        self.name = name
        self.version = version
        seller = ""
        artistName = ""
        sellerURL = nil
        artworkURL = nil
        description = ""
        releaseNotes = ""
        releaseDate = ""
        currentVersionReleaseDate = ""
        minimumOSVersion = ""
        primaryGenre = ""
        contentAdvisoryRating = ""
        currency = "USD"
        trackViewURL = nil
        languageCodes = []
        screenshots = []
        iPadScreenshots = []
        supportedDevices = []
        platforms = []
        averageRating = 0
        ratingCount = 0
        currentVersionAverageRating = 0
        currentVersionRatingCount = 0
        byteSize = 0
        price = 0
        hasInAppPurchases = false
        isUniversal = false
        purchaseDate = nil
    }

    static func == (lhs: StoreApp, rhs: StoreApp) -> Bool {
        lhs.appID == rhs.appID
    }

    func hash(into hasher: inout Hasher) {
        hasher.combine(appID)
    }
}

// The Objective-C handle is populated by the bridge initializer and is
// read-only afterwards, so sharing a StoreApp across isolation domains
// cannot race.
extension StoreApp: @unchecked Sendable {}

struct StoreStorefront: Identifiable, Hashable {
    let code: String
    let name: String
    let storefrontID: String
    let currency: String

    var id: String { code }

    var flag: String {
        code.unicodeScalars.reduce(into: "") { result, scalar in
            if let flagScalar = UnicodeScalar(127_397 + scalar.value) {
                result.unicodeScalars.append(flagScalar)
            }
        }
    }

    var label: String {
        "\(flag) \(name)"
    }
}

// MARK: - Accounts

struct StoreAccount: Identifiable, Hashable {
    let handle: IPStoreAccount
    let email: String
    let name: String
    let region: String
    let storefront: String
    let directoryServicesID: String
    /// Whether the Keychain holds this account's password, enabling token
    /// rotation without asking for it again.
    let hasSavedPassword: Bool

    var id: String { email }

    var initial: String {
        let source = name.isEmpty ? email : name
        return String(source.prefix(1)).uppercased()
    }

    init(handle: IPStoreAccount) {
        self.handle = handle
        email = handle.email
        name = handle.name
        region = handle.region.uppercased()
        storefront = handle.storefront
        directoryServicesID = handle.directoryServicesID
        hasSavedPassword = handle.hasSavedPassword
    }

    static func == (lhs: StoreAccount, rhs: StoreAccount) -> Bool {
        lhs.email.caseInsensitiveCompare(rhs.email) == .orderedSame
    }

    func hash(into hasher: inout Hasher) {
        hasher.combine(email.lowercased())
    }
}

// Same reasoning as StoreApp: the bridge writes the handle's fields in its
// initializer and never mutates them afterwards.
extension StoreAccount: @unchecked Sendable {}

// MARK: - Search

enum SearchSort: String, CaseIterable, Identifiable {
    case relevance
    case name
    case rating
    case recent

    var id: String { rawValue }

    var title: String {
        switch self {
        case .relevance: return String(localized: "Relevance")
        case .name: return String(localized: "Name")
        case .rating: return String(localized: "Rating")
        case .recent: return String(localized: "Newest")
        }
    }
}

struct SearchFilter: Equatable {
    var term: String = ""
    var region: String = "US"
    var platform: StorePlatform = .iPhone
    /// nil means "any price".
    var freeOnly: Bool?
    var genreID: Int64 = 0
    var sort: SearchSort = .relevance
    var limit: Int = 40

    var isActive: Bool {
        freeOnly != nil || genreID != 0 || sort != .relevance
    }
}

struct StoreGenre: Identifiable, Hashable {
    let id: Int64
    let name: String

    static let all: [StoreGenre] = [
        .init(id: 6000, name: "Business"),
        .init(id: 6001, name: "Weather"),
        .init(id: 6002, name: "Utilities"),
        .init(id: 6003, name: "Travel"),
        .init(id: 6004, name: "Sports"),
        .init(id: 6005, name: "Social Networking"),
        .init(id: 6006, name: "Reference"),
        .init(id: 6007, name: "Productivity"),
        .init(id: 6008, name: "Photo & Video"),
        .init(id: 6009, name: "News"),
        .init(id: 6010, name: "Navigation"),
        .init(id: 6011, name: "Music"),
        .init(id: 6012, name: "Lifestyle"),
        .init(id: 6013, name: "Health & Fitness"),
        .init(id: 6014, name: "Finance"),
        .init(id: 6015, name: "Entertainment"),
        .init(id: 6016, name: "Education"),
        .init(id: 6017, name: "Books"),
        .init(id: 6018, name: "Medical"),
        .init(id: 6020, name: "Games"),
        .init(id: 6021, name: "Developer Tools"),
        .init(id: 6022, name: "Graphics & Design"),
    ]
}

// MARK: - Versions and library

struct StoreVersion: Identifiable, Hashable {
    let externalVersionID: String
    /// Marketing version read from the package itself; empty until the
    /// package metadata for this build has been loaded.
    var displayVersion: String
    var releaseDate: Date?
    let isLatest: Bool

    var id: String { externalVersionID }
    var title: String {
        displayVersion.isEmpty
            ? String(localized: "Build \(externalVersionID)")
            : String(localized: "Version \(displayVersion)")
    }
    var releaseDateLabel: String? {
        releaseDate?.formatted(date: .abbreviated, time: .omitted)
    }

    init(handle: IPStoreVersion) {
        externalVersionID = handle.externalVersionID
        displayVersion = handle.displayVersion
        releaseDate = handle.releaseDate
        isLatest = handle.isLatest
    }

    init(externalVersionID: String, displayVersion: String = "",
         releaseDate: Date? = nil, isLatest: Bool = false) {
        self.externalVersionID = externalVersionID
        self.displayVersion = displayVersion
        self.releaseDate = releaseDate
        self.isLatest = isLatest
    }
}

/// Display version and release date of one build, read from its package.
struct StoreVersionMetadata {
    let displayVersion: String
    let releaseDate: Date?
}

/// One page of the account's purchase history.
struct StoreOwnedAppsPage {
    let apps: [StoreApp]
    let total: Int
    let page: Int
    let limit: Int
}

enum LoginOutcome {
    case signedIn(StoreAccount)
    case twoFactorRequired
}

enum StoreErrorCode: Int {
    case cancelled = 1
    case network
    case httpStatus
    case invalidResponse
    case unauthorized
    case tokenExpired
    case twoFactorRequired
    case licenseRequired
    case unavailable
    case appNotFound
    case unsupported
    case signerUnavailable
    case storage
    case integrity
    case purchaseFailed
    case internalError
}

struct StoreError: LocalizedError {
    let code: StoreErrorCode
    let message: String

    var errorDescription: String? { message }

    var isSignInProblem: Bool {
        code == .unauthorized || code == .tokenExpired
    }

    init(_ error: Error) {
        if error is CancellationError {
            code = .cancelled
            message = String(localized: "Cancelled.")
            return
        }
        let nsError = error as NSError
        message = nsError.localizedDescription
        // Codes only mean something inside their own domain. Reading the
        // raw code without checking the domain (as this used to) turns any
        // foreign error whose code happens to be 1 into "cancelled".
        if nsError.domain == IPStoreErrorDomain {
            code = StoreErrorCode(rawValue: nsError.code) ?? .internalError
        } else if nsError.domain == NSURLErrorDomain {
            code = nsError.code == NSURLErrorCancelled ? .cancelled : .network
        } else {
            code = .internalError
        }
    }

    init(code: StoreErrorCode, message: String) {
        self.code = code
        self.message = message
    }
}

struct LibraryItem: Identifiable, Hashable {
    let id: String
    let appID: Int64?
    let bundleID: String
    let name: String
    let version: String
    let externalVersionID: String
    let downloadedAt: Date
    let byteSize: Int64
    let fileURL: URL

    var sizeLabel: String {
        ByteCountFormatter.string(fromByteCount: byteSize, countStyle: .file)
    }

    var displayVersion: String {
        version.isEmpty ? String(localized: "Unknown version") : version
    }

    var artworkURL: URL? {
        guard let artwork = sidecar["artwork"] as? String else { return nil }
        return URL(string: artwork)
    }

    /// The Apple ID the package's FairPlay license belongs to, as
    /// recorded at download time. iOS only installs the package while
    /// the device's App Store (Media & Purchases) account is this one.
    var accountEmail: String? {
        guard let account = sidecar["account"] as? String,
              !account.isEmpty else { return nil }
        return account
    }

    private var sidecar: [String: Any] {
        let url = fileURL.appendingPathExtension("json")
        guard let data = try? Data(contentsOf: url),
              let document = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else {
            return [:]
        }
        return document
    }

    init?(ipaURL: URL) {
        let sidecarURL = ipaURL.appendingPathExtension("json")
        let fallbackName = ipaURL.deletingPathExtension().lastPathComponent
        let attributes = try? FileManager.default.attributesOfItem(atPath: ipaURL.path)
        let size = (attributes?[.size] as? NSNumber)?.int64Value ?? 0
        let modified = attributes?[.modificationDate] as? Date ?? Date()

        if let data = try? Data(contentsOf: sidecarURL),
           let document = try? JSONSerialization.jsonObject(with: data) as? [String: Any] {
            appID = (document["appID"] as? NSNumber)?.int64Value
            bundleID = document["bundleID"] as? String ?? ""
            name = (document["name"] as? String).flatMap { $0.isEmpty ? nil : $0 } ?? fallbackName
            version = document["version"] as? String ?? ""
            externalVersionID = document["externalVersionID"] as? String ?? ""
            let interval = (document["downloadedAt"] as? NSNumber)?.doubleValue
                ?? modified.timeIntervalSince1970
            downloadedAt = Date(timeIntervalSince1970: interval)
            byteSize = (document["size"] as? NSNumber)?.int64Value ?? size
        } else {
            appID = nil
            bundleID = ""
            name = fallbackName
            version = ""
            externalVersionID = ""
            downloadedAt = modified
            byteSize = size
        }
        fileURL = ipaURL
        id = ipaURL.path
    }

    init(appID: Int64, bundleID: String, name: String, version: String,
         externalVersionID: String, downloadedAt: Date, byteSize: Int64, fileURL: URL) {
        id = fileURL.path
        self.appID = appID
        self.bundleID = bundleID
        self.name = name
        self.version = version
        self.externalVersionID = externalVersionID
        self.downloadedAt = downloadedAt
        self.byteSize = byteSize
        self.fileURL = fileURL
    }
}

// MARK: - Installation state

enum InstallPhase: Equatable {
    case idle
    case preparing
    case ready
    case sending(Double)
    case failed(String)
    case stopped

    var title: String {
        switch self {
        case .idle: return String(localized: "Not running")
        case .preparing: return String(localized: "Preparing")
        case .ready: return String(localized: "Ready to install")
        case .sending(let progress): return String(localized: "Sending… \(Int(progress * 100))%")
        case .failed: return String(localized: "Installation failed")
        case .stopped: return String(localized: "Stopped")
        }
    }

    var isBusy: Bool {
        switch self {
        case .preparing, .ready, .sending: return true
        default: return false
        }
    }
}
