// SPDX-License-Identifier: MIT

import SwiftUI
import UIKit

struct AppBackground: View {
    var body: some View {
        LinearGradient(
            colors: [
                Color(.systemGroupedBackground),
                Color.indigo.opacity(0.08),
                Color(.systemGroupedBackground)
            ],
            startPoint: .topLeading,
            endPoint: .bottomTrailing)
        .ignoresSafeArea()
    }
}

struct AppArtwork: View {
    let url: URL?
    var size: CGFloat = 58
    var cornerRadius: CGFloat = 14

    var body: some View {
        AsyncImage(url: url, transaction: Transaction(animation: .easeOut(duration: 0.2))) { phase in
            switch phase {
            case .success(let image):
                image
                    .resizable()
                    .scaledToFill()
            case .failure:
                placeholder
            case .empty:
                placeholder
                    .overlay(ProgressView().controlSize(.small))
            @unknown default:
                placeholder
            }
        }
        .frame(width: size, height: size)
        .clipShape(RoundedRectangle(cornerRadius: cornerRadius, style: .continuous))
        .overlay {
            RoundedRectangle(cornerRadius: cornerRadius, style: .continuous)
                .strokeBorder(.white.opacity(0.12), lineWidth: 0.5)
        }
        .shadow(color: .black.opacity(0.16), radius: 6, y: 3)
    }

    private var placeholder: some View {
        ZStack {
            LinearGradient(colors: [.indigo.opacity(0.85), .purple.opacity(0.65)],
                           startPoint: .topLeading,
                           endPoint: .bottomTrailing)
            Image(systemName: "app.dashed")
                .font(.system(size: size * 0.36, weight: .semibold))
                .foregroundStyle(.white.opacity(0.9))
        }
    }
}

/// Human-readable name for the platform identifiers the core reports
/// ("iphone", "appletv", "visionos", …).
func platformDisplayName(_ identifier: String) -> String {
    switch identifier.lowercased() {
    case "iphone", "iphoneos": return "iPhone"
    case "ipad", "ipados": return "iPad"
    case "mac", "macos": return "Mac"
    case "appletv", "tvos": return "tvOS"
    case "watch", "watchos": return "watchOS"
    case "xros", "vision", "visionos": return "visionOS"
    default: return identifier
    }
}

struct PlatformChip: View {
    let title: String
    var highlighted = false

    var body: some View {
        Text(title)
            .font(.caption.weight(.semibold))
            .padding(.horizontal, 9)
            .padding(.vertical, 4)
            .background(
                Capsule().fill(highlighted ? Color.indigo.opacity(0.18)
                                           : Color(.tertiarySystemFill)))
            .foregroundStyle(highlighted ? Color.indigo : Color.secondary)
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
        .font(.subheadline)
    }
}

struct EmptyStateView: View {
    let symbol: String
    let title: String
    let message: String
    var actionTitle: String?
    var action: (() -> Void)?

    var body: some View {
        VStack(spacing: 14) {
            Image(systemName: symbol)
                .font(.system(size: 46, weight: .thin))
                .foregroundStyle(.indigo.opacity(0.8))
            Text(title)
                .font(.title3.weight(.semibold))
            Text(message)
                .font(.subheadline)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
                .frame(maxWidth: 340)
            if let actionTitle, let action {
                Button(actionTitle, action: action)
                    .buttonStyle(.borderedProminent)
                    .padding(.top, 4)
            }
        }
        .padding(28)
    }
}

/// Five stars with half-star support.
struct StarRating: View {
    let rating: Double
    var size: CGFloat = 12

    var body: some View {
        HStack(spacing: 1.5) {
            ForEach(0..<5, id: \.self) { index in
                Image(systemName: symbol(for: index))
                    .font(.system(size: size, weight: .semibold))
                    .foregroundStyle(Color.orange)
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

struct RatingBadge: View {
    let app: StoreApp

    var body: some View {
        VStack(spacing: 2) {
            Text(app.ratingLabel)
                .font(.system(size: 22, weight: .bold, design: .rounded))
            StarRating(rating: app.averageRating, size: 10)
            Text(app.ratingCountLabel)
                .font(.caption2)
                .foregroundStyle(.secondary)
        }
        .frame(minWidth: 84)
        .padding(.vertical, 10)
        .padding(.horizontal, 8)
        .background(
            RoundedRectangle(cornerRadius: 14, style: .continuous)
                .fill(Color(.tertiarySystemFill).opacity(0.8)))
    }
}

/// Horizontal, paging screenshot strip.
struct ScreenshotStrip: View {
    let urls: [URL]
    var height: CGFloat = 320

    var body: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            LazyHStack(spacing: 12) {
                ForEach(Array(urls.enumerated()), id: \.offset) { _, url in
                    AsyncImage(url: url) { phase in
                        switch phase {
                        case .success(let image):
                            image
                                .resizable()
                                .scaledToFill()
                        case .failure:
                            placeholder
                        case .empty:
                            placeholder.overlay(ProgressView())
                        @unknown default:
                            placeholder
                        }
                    }
                    .frame(width: height * 0.52, height: height)
                    .clipShape(RoundedRectangle(cornerRadius: 18, style: .continuous))
                    .overlay {
                        RoundedRectangle(cornerRadius: 18, style: .continuous)
                            .strokeBorder(Color.primary.opacity(0.08))
                    }
                }
            }
            .padding(.horizontal, 20)
        }
    }

    private var placeholder: some View {
        ZStack {
            Color(.secondarySystemBackground)
            Image(systemName: "photo")
                .font(.title2)
                .foregroundStyle(.tertiary)
        }
    }
}

struct CardBackground: ViewModifier {
    func body(content: Content) -> some View {
        content
            .padding(16)
            .background(
                RoundedRectangle(cornerRadius: 18, style: .continuous)
                    .fill(Color(.secondarySystemGroupedBackground).opacity(0.75)))
    }
}

extension View {
    func card() -> some View { modifier(CardBackground()) }
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

/// One catalog row, shared by Search and Purchased.
struct AppRow: View {
    let app: StoreApp

    var body: some View {
        HStack(spacing: 14) {
            AppArtwork(url: app.artworkURL, size: 58)
            VStack(alignment: .leading, spacing: 3) {
                Text(app.name)
                    .font(.headline)
                    .lineLimit(1)
                Text(app.artistName.isEmpty ? app.seller : app.artistName)
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                HStack(spacing: 6) {
                    if app.ratingCount > 0 {
                        StarRating(rating: app.averageRating, size: 9)
                        Text(app.ratingLabel)
                        Text("·")
                    }
                    Text(app.primaryGenre)
                    if app.byteSize > 0 {
                        Text("·")
                        Text(app.sizeLabel)
                    }
                }
                .font(.caption2)
                .foregroundStyle(.tertiary)
                .lineLimit(1)
            }
            Spacer(minLength: 8)
            Text(app.priceLabel)
                .font(.subheadline.weight(.semibold))
                .foregroundStyle(app.isFree ? Color.indigo : Color.secondary)
                .padding(.horizontal, 10)
                .padding(.vertical, 5)
                .background(Capsule().fill(Color(.tertiarySystemFill)))
        }
        .padding(.vertical, 6)
        .contentShape(Rectangle())
    }
}

/// Storefront chooser used by the search toolbar.
struct RegionPickerView: View {
    @Environment(AppModel.self) private var model
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
                            Text("\(storefront.code) · \(storefront.currency)")
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        }
                        Spacer()
                        if storefront.code == model.region {
                            Image(systemName: "checkmark").foregroundStyle(.indigo)
                        }
                    }
                }
                .buttonStyle(.plain)
            }
            .navigationTitle("Storefront")
            .navigationBarTitleDisplayMode(.inline)
            .searchable(text: $query, prompt: "Country or region")
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { dismiss() }
                }
            }
        }
    }
}
