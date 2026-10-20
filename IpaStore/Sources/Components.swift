// SPDX-License-Identifier: MIT

import ImageIO
import SwiftUI
import UIKit

// MARK: - Image loading

/// Shared artwork loader with an in-memory cache and downsampling, so list
/// rows never flash spinners or decode full-resolution icons.
final class ArtworkCache: @unchecked Sendable {
    static let shared = ArtworkCache()

    private let cache = NSCache<NSURL, UIImage>()

    private init() {
        cache.countLimit = 400
    }

    func load(_ url: URL, maxPixel: CGFloat) async -> UIImage? {
        if let cached = cache.object(forKey: url as NSURL) { return cached }
        guard let (data, _) = try? await URLSession.shared.data(from: url),
              let image = Self.downsample(data: data, maxPixel: maxPixel) else {
            return nil
        }
        cache.setObject(image, forKey: url as NSURL)
        return image
    }

    private static func downsample(data: Data, maxPixel: CGFloat) -> UIImage? {
        let sourceOptions: [CFString: Any] = [kCGImageSourceShouldCache: false]
        guard let source = CGImageSourceCreateWithData(data as CFData,
                                                        sourceOptions as CFDictionary) else {
            return UIImage(data: data)
        }
        let thumbOptions: [CFString: Any] = [
            kCGImageSourceCreateThumbnailFromImageAlways: true,
            kCGImageSourceShouldCacheImmediately: true,
            kCGImageSourceCreateThumbnailWithTransform: true,
            kCGImageSourceThumbnailMaxPixelSize: maxPixel,
        ]
        guard let cgImage = CGImageSourceCreateThumbnailAtIndex(
            source, 0, thumbOptions as CFDictionary) else {
            return UIImage(data: data)
        }
        return UIImage(cgImage: cgImage)
    }
}

/// App icon with the system squircle proportions and hairline border,
/// exactly as the App Store renders artwork.
struct AppArtwork: View {
    let url: URL?
    var size: CGFloat = 60

    @State private var image: UIImage?

    private var radius: CGFloat { size * 0.2237 }

    var body: some View {
        Group {
            if let image {
                Image(uiImage: image)
                    .resizable()
                    .scaledToFill()
            } else {
                ZStack {
                    Color(.secondarySystemFill)
                    Image(systemName: "app.fill")
                        .font(.system(size: size * 0.44, weight: .regular))
                        .foregroundStyle(.tertiary)
                }
            }
        }
        .frame(width: size, height: size)
        .clipShape(RoundedRectangle(cornerRadius: radius, style: .continuous))
        .overlay {
            RoundedRectangle(cornerRadius: radius, style: .continuous)
                .strokeBorder(Color.primary.opacity(0.08), lineWidth: 0.5)
        }
        .task(id: url) {
            guard let url, image == nil else { return }
            image = await ArtworkCache.shared.load(url, maxPixel: size * 3)
        }
    }
}

struct ScreenshotView: View {
    let url: URL
    var height: CGFloat = 400

    @State private var image: UIImage?

    var body: some View {
        Group {
            if let image {
                Image(uiImage: image)
                    .resizable()
                    .scaledToFit()
            } else {
                ZStack {
                    Color(.secondarySystemFill)
                    Image(systemName: "photo")
                        .font(.title2)
                        .foregroundStyle(.tertiary)
                }
                .frame(width: height * 0.46)
            }
        }
        .frame(height: height)
        .clipShape(RoundedRectangle(cornerRadius: 16, style: .continuous))
        .overlay {
            RoundedRectangle(cornerRadius: 16, style: .continuous)
                .strokeBorder(Color.primary.opacity(0.08), lineWidth: 0.5)
        }
        .task(id: url) {
            if image == nil {
                image = await ArtworkCache.shared.load(url, maxPixel: 1000)
            }
        }
    }
}

/// Snap-scrolling preview strip, like the App Store product page.
struct ScreenshotStrip: View {
    let urls: [URL]
    var height: CGFloat = 400

    var body: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            LazyHStack(spacing: 10) {
                ForEach(Array(urls.enumerated()), id: \.offset) { _, url in
                    ScreenshotView(url: url, height: height)
                }
            }
            .scrollTargetLayout()
            .padding(.horizontal, 20)
        }
        .scrollTargetBehavior(.viewAligned)
    }
}

// MARK: - Ratings

/// Five stars with half-star support, in the App Store's gray.
struct StarRating: View {
    let rating: Double
    var size: CGFloat = 11

    var body: some View {
        HStack(spacing: 1.5) {
            ForEach(0..<5, id: \.self) { index in
                Image(systemName: symbol(for: index))
                    .font(.system(size: size, weight: .semibold))
                    .foregroundStyle(.secondary)
            }
        }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(String(format: "%.1f out of 5 stars", rating))
    }

    private func symbol(for index: Int) -> String {
        let value = rating - Double(index)
        if value >= 0.75 { return "star.fill" }
        if value >= 0.25 { return "star.leadinghalf.filled" }
        return "star"
    }
}

// MARK: - Download progress and GET

struct CircularProgressRing: View {
    @Environment(ThemeStore.self) private var theme
    var fraction: Double
    var size: CGFloat = 30
    var lineWidth: CGFloat = 2.5
    var showsStop = true

    var body: some View {
        ZStack {
            Circle()
                .stroke(Color.secondary.opacity(0.25), lineWidth: lineWidth)
            Circle()
                .trim(from: 0, to: max(0.02, min(1, fraction)))
                .stroke(theme.accentColor,
                        style: StrokeStyle(lineWidth: lineWidth, lineCap: .round))
                .rotationEffect(.degrees(-90))
            if showsStop {
                RoundedRectangle(cornerRadius: size * 0.06, style: .continuous)
                    .fill(theme.accentColor)
                    .frame(width: size * 0.3, height: size * 0.3)
            }
        }
        .frame(width: size, height: size)
        .animation(.linear(duration: 0.2), value: fraction)
    }
}

/// The App Store GET pill: price capsule for paid apps, a cloud for owned
/// apps and older builds, and a cancellable progress ring while a transfer
/// is running — all without leaving the current screen.
struct GetButton: View {
    @Environment(AppModel.self) private var model
    @Environment(ThemeStore.self) private var theme

    let app: StoreApp
    var versionID: String? = nil
    var ownedContext = false

    @State private var tapCount = 0

    private var active: DownloadItem? {
        model.activeDownload(for: app.appID, versionID: versionID)
    }

    private var owned: Bool {
        ownedContext || model.isOwned(app.appID)
    }

    var body: some View {
        Group {
            if let active {
                Button {
                    model.cancelDownload(active)
                } label: {
                    CircularProgressRing(
                        fraction: active.state == .processing ? 1 : active.fraction,
                        size: 30)
                    .frame(width: 44, height: 32)
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .accessibilityLabel(String(localized: "Cancel download"))
            } else if owned || versionID != nil {
                Button {
                    tapCount += 1
                    model.get(app, versionID: versionID)
                } label: {
                    Image(systemName: "icloud.and.arrow.down")
                        .font(.system(size: 23, weight: .medium))
                        .foregroundStyle(theme.accentColor)
                        .frame(width: 44, height: 32)
                        .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .accessibilityLabel(String(localized: "Download"))
            } else {
                Button {
                    tapCount += 1
                    model.get(app, versionID: versionID)
                } label: {
                    Text(app.isFree ? String(localized: "GET") : app.priceLabel)
                        .font(.system(size: 15, weight: .bold))
                        .foregroundStyle(theme.accentColor)
                        .frame(minWidth: 52)
                        .padding(.horizontal, 10)
                        .frame(height: 30)
                        .background(Capsule().fill(Color(.tertiarySystemFill)))
                }
                .buttonStyle(.plain)
                .accessibilityLabel(String(localized: "Get \(app.name)"))
            }
        }
        .sensoryFeedback(.impact(weight: .light), trigger: tapCount)
    }
}

// MARK: - Rows and chips

/// One catalog row, shared by Search and Purchased. The navigation link is
/// hidden behind the row so the GET button keeps its own tap and no
/// disclosure chevron shows, like the App Store.
struct AppListRow: View {
    let app: StoreApp
    var ownedContext = false

    var body: some View {
        HStack(spacing: 12) {
            AppArtwork(url: app.artworkURL, size: 60)
            VStack(alignment: .leading, spacing: 3) {
                Text(app.name)
                    .font(.system(size: 16, weight: .semibold))
                    .lineLimit(1)
                Text(subtitle)
                    .font(.system(size: 13))
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                if app.ratingCount > 0 {
                    HStack(spacing: 4) {
                        StarRating(rating: app.averageRating, size: 9)
                        Text(app.ratingCountCompact)
                    }
                    .font(.caption2)
                    .foregroundStyle(.tertiary)
                }
            }
            Spacer(minLength: 8)
            GetButton(app: app, ownedContext: ownedContext)
        }
        .padding(.vertical, 6)
        .contentShape(Rectangle())
        .background {
            NavigationLink(value: app) { EmptyView() }
                .opacity(0)
        }
    }

    private var subtitle: String {
        if ownedContext, let date = app.purchaseDate {
            return date.formatted(date: .abbreviated, time: .omitted)
        }
        if !app.primaryGenre.isEmpty { return app.primaryGenre }
        return app.artistName.isEmpty ? app.seller : app.artistName
    }
}

struct SkeletonAppRow: View {
    var body: some View {
        HStack(spacing: 12) {
            RoundedRectangle(cornerRadius: 13, style: .continuous)
                .fill(Color(.secondarySystemFill))
                .frame(width: 60, height: 60)
            VStack(alignment: .leading, spacing: 7) {
                RoundedRectangle(cornerRadius: 4, style: .continuous)
                    .fill(Color(.secondarySystemFill))
                    .frame(width: 150, height: 13)
                RoundedRectangle(cornerRadius: 4, style: .continuous)
                    .fill(Color(.secondarySystemFill))
                    .frame(width: 100, height: 10)
                RoundedRectangle(cornerRadius: 4, style: .continuous)
                    .fill(Color(.secondarySystemFill))
                    .frame(width: 70, height: 8)
            }
            Spacer()
            Capsule()
                .fill(Color(.secondarySystemFill))
                .frame(width: 72, height: 30)
        }
        .padding(.vertical, 6)
        .redacted(reason: .placeholder)
        .accessibilityHidden(true)
    }
}

struct FilterChip: View {
    @Environment(ThemeStore.self) private var theme

    let title: String
    var systemImage: String?
    let selected: Bool
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            HStack(spacing: 5) {
                if let systemImage {
                    Image(systemName: systemImage)
                        .font(.system(size: 12, weight: .semibold))
                }
                Text(title)
                    .font(.system(size: 14, weight: .semibold))
                    .lineLimit(1)
            }
            .padding(.horizontal, 14)
            .frame(height: 32)
            .background(Capsule().fill(selected ? theme.accentColor : Color(.secondarySystemFill)))
            .foregroundStyle(selected ? Color.white : Color.primary)
        }
        .buttonStyle(.plain)
    }
}

/// Bold section title in the App Store product-page style.
struct SectionHeader: View {
    let title: LocalizedStringKey

    var body: some View {
        Text(title)
            .font(.system(size: 20, weight: .bold))
            .frame(maxWidth: .infinity, alignment: .leading)
    }
}

/// One column of the product-page statistics strip.
struct StatColumn: View {
    let value: String
    let caption: String
    var rating: Double?

    var body: some View {
        VStack(spacing: 3) {
            Text(value)
                .font(.system(size: 19, weight: .bold))
                .foregroundStyle(.primary.opacity(0.82))
                .lineLimit(1)
            if let rating {
                StarRating(rating: rating, size: 9)
            }
            Text(caption)
                .font(.system(size: 12))
                .foregroundStyle(.secondary)
                .lineLimit(1)
        }
        .frame(minWidth: 78)
    }
}

// MARK: - Account avatar

/// Toolbar avatar that opens the account hub, like the App Store.
struct AvatarButton: View {
    @Environment(AppModel.self) private var model
    @Environment(ThemeStore.self) private var theme

    var body: some View {
        Button {
            model.showAccount = true
        } label: {
            if let account = model.activeAccount {
                Text(account.initial)
                    .font(.system(size: 15, weight: .bold))
                    .foregroundStyle(.white)
                    .frame(width: 32, height: 32)
                    .background {
                        Circle().fill(
                            LinearGradient(
                                colors: [theme.accentColor, theme.accentColor.opacity(0.6)],
                                startPoint: .top, endPoint: .bottom))
                    }
            } else {
                Image(systemName: "person.crop.circle")
                    .font(.system(size: 27))
                    .foregroundStyle(.secondary)
            }
        }
        .accessibilityLabel(String(localized: "Account"))
    }
}

// MARK: - Small utilities

/// Human-readable name for the platform identifiers the core reports
/// ("iphone", "appletv", "visionos", …).
func platformDisplayName(_ identifier: String) -> String {
    switch identifier.lowercased() {
    case "iphone", "iphoneos": return "iPhone"
    case "ipad", "ipados": return "iPad"
    case "mac", "macos": return "Mac"
    case "appletv", "tvos": return "Apple TV"
    case "watch", "watchos": return "Apple Watch"
    case "xros", "vision", "visionos": return "Apple Vision Pro"
    default: return identifier
    }
}

struct InfoRow: View {
    let label: String
    let value: String

    var body: some View {
        HStack(alignment: .firstTextBaseline, spacing: 12) {
            Text(label)
                .foregroundStyle(.secondary)
            Spacer(minLength: 12)
            Text(value)
                .multilineTextAlignment(.trailing)
                .textSelection(.enabled)
        }
        .font(.system(size: 15))
    }
}

struct CopyableIdentifier: View {
    let value: String

    @State private var copied = false

    var body: some View {
        Button {
            UIPasteboard.general.string = value
            copied = true
            Task {
                try? await Task.sleep(for: .seconds(1.2))
                copied = false
            }
        } label: {
            HStack(spacing: 6) {
                Text(value)
                    .font(.caption.monospaced())
                    .lineLimit(1)
                    .truncationMode(.middle)
                Image(systemName: copied ? "checkmark" : "doc.on.doc")
                    .font(.caption2)
            }
            .foregroundStyle(.secondary)
        }
        .buttonStyle(.plain)
    }
}

/// Storefront chooser presented from the search controls.
struct RegionPickerView: View {
    @Environment(AppModel.self) private var model
    @Environment(ThemeStore.self) private var theme
    @Environment(\.dismiss) private var dismiss
    @State private var query = ""

    private var filtered: [StoreStorefront] {
        let needle = query.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !needle.isEmpty else { return model.storefronts }
        return model.storefronts.filter {
            $0.name.localizedCaseInsensitiveContains(needle)
                || $0.code.localizedCaseInsensitiveContains(needle)
        }
    }

    var body: some View {
        NavigationStack {
            List(filtered) { storefront in
                Button {
                    model.setRegion(storefront.code)
                    dismiss()
                } label: {
                    HStack {
                        Text(storefront.flag)
                        VStack(alignment: .leading, spacing: 2) {
                            Text(storefront.name)
                                .foregroundStyle(.primary)
                            Text("\(storefront.code) · \(storefront.currency)")
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        }
                        Spacer()
                        if storefront.code == model.region {
                            Image(systemName: "checkmark")
                                .fontWeight(.semibold)
                                .foregroundStyle(theme.accentColor)
                        }
                    }
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
            }
            .listStyle(.plain)
            .navigationTitle(String(localized: "Storefront"))
            .navigationBarTitleDisplayMode(.inline)
            .searchable(text: $query, prompt: String(localized: "Country or region"))
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button(String(localized: "Cancel")) { dismiss() }
                }
            }
        }
    }
}
