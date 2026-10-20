// SPDX-License-Identifier: MIT

import SwiftUI
import UIKit

struct AppDetailView: View {
    let app: StoreApp

    @Environment(AppModel.self) private var model
    @State private var showVersions = false
    @State private var isLoadingVersions = false
    @State private var galleryUsesIPad = false

    private var selectedVersion: String? {
        model.selectedVersion[app.appID]
    }

    private var localPackage: LibraryItem? {
        model.library.first { item in
            !item.bundleID.isEmpty && item.bundleID.caseInsensitiveCompare(app.bundleID) == .orderedSame
        }
    }

    var body: some View {
        ZStack {
            AppBackground()
            ScrollView {
                VStack(alignment: .leading, spacing: 22) {
                    header
                    ratingSummary
                    actionBar
                    gallery
                    aboutSection
                    whatsNewSection
                    informationSection
                    reviewsSection
                }
                .padding(.bottom, 32)
            }
        }
        .navigationTitle(app.name)
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .topBarTrailing) {
                ShareLink(item: shareURL) {
                    Image(systemName: "square.and.arrow.up")
                }
            }
        }
        .sheet(isPresented: $showVersions) {
            versionSheet
        }
        .task {
            if app.handle != nil, model.versions[app.appID] == nil {
                isLoadingVersions = true
                await model.loadVersions(for: app)
                isLoadingVersions = false
            }
            if model.reviews[app.appID] == nil {
                await model.loadReviews(for: app)
            }
        }
    }

    private var shareURL: URL {
        app.trackViewURL ?? URL(string: "https://apps.apple.com/app/id\(app.appID)")
            ?? URL(string: "https://apps.apple.com")!
    }

    // MARK: Header

    private var header: some View {
        HStack(alignment: .top, spacing: 16) {
            AppArtwork(url: app.artworkURL, size: 104, cornerRadius: 22)
            VStack(alignment: .leading, spacing: 6) {
                Text(app.name)
                    .font(.title2.weight(.bold))
                    .lineLimit(3)
                Text(app.artistName.isEmpty ? app.seller : app.artistName)
                    .font(.subheadline)
                    .foregroundStyle(Color.indigo)
                    .lineLimit(1)
                HStack(spacing: 8) {
                    Text(app.priceLabel)
                        .font(.subheadline.weight(.semibold))
                    if app.hasInAppPurchases {
                        Text("In-App Purchases")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                }
                HStack(spacing: 6) {
                    ForEach(app.platforms.prefix(4), id: \.self) { platform in
                        PlatformChip(title: platformDisplayName(platform),
                                     highlighted: platform == platformIdentifier)
                    }
                }
                .padding(.top, 2)
            }
            Spacer(minLength: 0)
        }
        .padding(.horizontal, 20)
        .padding(.top, 8)
    }

    private var ratingSummary: some View {
        HStack(alignment: .center, spacing: 16) {
            RatingBadge(app: app)
            VStack(alignment: .leading, spacing: 4) {
                Text(app.primaryGenre.isEmpty ? "App Store" : app.primaryGenre)
                    .font(.subheadline.weight(.semibold))
                Text(app.advisoryLabel == "—" ? "Rated for all ages" : "Rated \(app.advisoryLabel)")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                if app.ratingCount == 0 {
                    Text("This app has no ratings in this storefront yet.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                } else {
                    Text("\(app.ratingCountLabel) · current version \(String(format: "%.1f", app.currentVersionAverageRating))")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }
            Spacer()
        }
        .padding(.horizontal, 20)
    }

    // MARK: Actions

    private var actionBar: some View {
        VStack(spacing: 10) {
            HStack(spacing: 10) {
                Button {
                    if model.activeAccount == nil {
                        model.showLogin = true
                    } else {
                        model.download(app, versionID: selectedVersion)
                    }
                } label: {
                    Label(downloadTitle, systemImage: "arrow.down.circle.fill")
                        .font(.headline)
                        .frame(maxWidth: .infinity)
                        .padding(.vertical, 12)
                }
                .buttonStyle(.borderedProminent)
                .disabled(!app.isFree)

                Button {
                    showVersions = true
                    if model.versions[app.appID] == nil {
                        Task {
                            isLoadingVersions = true
                            await model.loadVersions(for: app)
                            isLoadingVersions = false
                        }
                    }
                } label: {
                    Image(systemName: "clock.arrow.circlepath")
                        .font(.headline)
                        .padding(.vertical, 12)
                        .padding(.horizontal, 16)
                }
                .buttonStyle(.bordered)
                .accessibilityLabel("Choose a version")

                Button {
                    model.toggleWishlist(app)
                } label: {
                    Image(systemName: model.isWishlisted(app) ? "star.fill" : "star")
                        .font(.headline)
                        .foregroundStyle(model.isWishlisted(app) ? Color.orange : Color.accentColor)
                        .padding(.vertical, 12)
                        .padding(.horizontal, 14)
                }
                .buttonStyle(.bordered)
                .accessibilityLabel(model.isWishlisted(app) ? "Remove from wishlist" : "Add to wishlist")
            }

            if let localPackage {
                Button {
                    model.install(localPackage)
                } label: {
                    Label("Install \(localPackage.displayVersion) on this device",
                          systemImage: "square.and.arrow.down.on.square")
                        .font(.subheadline.weight(.semibold))
                        .frame(maxWidth: .infinity)
                        .padding(.vertical, 10)
                }
                .buttonStyle(.bordered)
            }

            if !app.isFree {
                Text("Paid apps cannot be downloaded: Apple requires its own payment sheet, which is not available to third-party clients.")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                    .frame(maxWidth: .infinity, alignment: .leading)
            } else if let selectedVersion {
                Text("Build \(selectedVersion) will be requested from Apple.")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
        }
        .padding(.horizontal, 20)
    }

    private var downloadTitle: String {
        if model.activeAccount == nil { return "Sign In to Download" }
        if selectedVersion != nil { return "Download build \(selectedVersion ?? "")" }
        return "Download"
    }

    // MARK: Gallery

    @ViewBuilder
    private var gallery: some View {
        if !app.screenshots.isEmpty || !app.iPadScreenshots.isEmpty {
            VStack(alignment: .leading, spacing: 10) {
                HStack {
                    Text("Screenshots")
                        .font(.headline)
                    Spacer()
                    if !app.screenshots.isEmpty && !app.iPadScreenshots.isEmpty {
                        Picker("Device", selection: $galleryUsesIPad) {
                            Text("iPhone").tag(false)
                            Text("iPad").tag(true)
                        }
                        .pickerStyle(.segmented)
                        .frame(width: 160)
                    }
                }
                .padding(.horizontal, 20)
                ScreenshotStrip(urls: galleryUsesIPad ? app.iPadScreenshots : app.gallery,
                                height: galleryUsesIPad ? 360 : 320)
            }
        }
    }

    // MARK: Description

    private var aboutSection: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("About")
                .font(.headline)
            Text(app.description.isEmpty ? "No description was provided for this storefront." : app.description)
                .font(.body)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
        }
        .card()
        .padding(.horizontal, 20)
    }

    @ViewBuilder
    private var whatsNewSection: some View {
        if !app.releaseNotes.isEmpty {
            VStack(alignment: .leading, spacing: 8) {
                HStack {
                    Text("What’s New")
                        .font(.headline)
                    Spacer()
                    Text("Version \(app.version)")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                Text(app.releaseNotes)
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            .card()
            .padding(.horizontal, 20)
        }
    }

    private var informationSection: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Information")
                .font(.headline)
            InfoRow(label: "Seller", value: app.seller.isEmpty ? "—" : app.seller)
            InfoRow(label: "Bundle ID", value: app.bundleID)
            InfoRow(label: "Version", value: app.version)
            InfoRow(label: "Size", value: app.sizeLabel)
            InfoRow(label: "Minimum OS", value: app.minimumOSVersion.isEmpty ? "—" : "iOS \(app.minimumOSVersion)")
            InfoRow(label: "Updated", value: formattedDate(app.currentVersionReleaseDate))
            InfoRow(label: "Released", value: formattedDate(app.releaseDate))
            InfoRow(label: "Languages", value: app.languageCodes.isEmpty ? "—" : app.languageCodes.joined(separator: ", "))
            InfoRow(label: "Storefront", value: "\(model.currentStorefront?.flag ?? "") \(model.regionName(model.region))")
            if let sellerURL = app.sellerURL {
                Link(destination: sellerURL) {
                    Label("Developer website", systemImage: "safari")
                        .font(.subheadline)
                }
            }
            if let trackViewURL = app.trackViewURL {
                Link(destination: trackViewURL) {
                    Label("View on the App Store", systemImage: "arrow.up.right.square")
                        .font(.subheadline)
                }
            }
        }
        .card()
        .padding(.horizontal, 20)
    }

    // MARK: Reviews

    private var reviewsSection: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Text("Ratings & Reviews")
                    .font(.headline)
                Spacer()
                if model.isLoadingReviews.contains(app.appID) {
                    ProgressView().controlSize(.small)
                }
            }
            if let reviews = model.reviews[app.appID], !reviews.isEmpty {
                ForEach(reviews.prefix(6)) { review in
                    reviewRow(review)
                    if review.id != reviews.prefix(6).last?.id {
                        Divider()
                    }
                }
                if model.reviewHasMore[app.appID] == true {
                    Button {
                        Task { await model.loadReviews(for: app, nextPage: true) }
                    } label: {
                        Label("More reviews", systemImage: "ellipsis.circle")
                            .font(.subheadline.weight(.semibold))
                    }
                    .padding(.top, 2)
                }
            } else if model.isLoadingReviews.contains(app.appID) {
                Text("Loading reviews…")
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
            } else {
                Text("No written reviews are available in this storefront.")
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
            }
        }
        .card()
        .padding(.horizontal, 20)
    }

    private func reviewRow(_ review: StoreReview) -> some View {
        VStack(alignment: .leading, spacing: 5) {
            HStack {
                StarRating(rating: Double(review.rating), size: 10)
                Text(review.title)
                    .font(.subheadline.weight(.semibold))
                    .lineLimit(1)
                Spacer()
                Text(review.dateLabel)
                    .font(.caption2)
                    .foregroundStyle(.tertiary)
            }
            Text(review.body)
                .font(.subheadline)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
            Text("\(review.author) · v\(review.version)")
                .font(.caption2)
                .foregroundStyle(.tertiary)
        }
        .padding(.vertical, 4)
    }

    // MARK: Versions

    private var versionSheet: some View {
        NavigationStack {
            List {
                Section {
                    Button {
                        model.selectedVersion.removeValue(forKey: app.appID)
                        showVersions = false
                    } label: {
                        HStack {
                            Text("Latest available")
                            Spacer()
                            if selectedVersion == nil {
                                Image(systemName: "checkmark").foregroundStyle(.indigo)
                            }
                        }
                    }
                } footer: {
                    Text("Apple returns the newest build supported by the selected platform (\(model.platform.title)).")
                }

                Section("Version history") {
                    if isLoadingVersions {
                        HStack { ProgressView(); Text("Loading versions…").foregroundStyle(.secondary) }
                    } else if let versions = model.versions[app.appID], !versions.isEmpty {
                        ForEach(versions) { version in
                            Button {
                                model.selectedVersion[app.appID] = version.externalVersionID
                                showVersions = false
                                model.download(app, versionID: version.externalVersionID)
                            } label: {
                                HStack {
                                    VStack(alignment: .leading, spacing: 2) {
                                        HStack(spacing: 6) {
                                            Text(version.title)
                                            if version.isLatest {
                                                PlatformChip(title: "Latest", highlighted: true)
                                            }
                                        }
                                        Text(versionSubtitle(version))
                                            .font(.caption)
                                            .foregroundStyle(.secondary)
                                    }
                                    Spacer()
                                    if selectedVersion == version.externalVersionID {
                                        Image(systemName: "checkmark").foregroundStyle(.indigo)
                                    }
                                }
                            }
                            .task {
                                // Fill in the marketing version and release
                                // date from the package itself, on demand.
                                await model.enrichVersion(version, for: app)
                            }
                        }
                    } else if let message = model.versionErrors[app.appID] {
                        Text(message)
                            .font(.footnote)
                            .foregroundStyle(.secondary)
                    } else {
                        Text(model.activeAccount == nil
                             ? "Sign in to list every build Apple has issued for this app."
                             : "No older builds are available.")
                            .foregroundStyle(.secondary)
                    }
                }

                Section("Downloaded builds") {
                    let items = model.library.filter {
                        $0.bundleID.caseInsensitiveCompare(app.bundleID) == .orderedSame
                    }
                    if items.isEmpty {
                        Text("Nothing downloaded yet.")
                            .foregroundStyle(.secondary)
                    } else {
                        ForEach(items) { item in
                            VStack(alignment: .leading, spacing: 4) {
                                HStack {
                                    Text(item.displayVersion)
                                        .font(.subheadline.weight(.semibold))
                                    Spacer()
                                    Text(item.sizeLabel)
                                        .font(.caption)
                                        .foregroundStyle(.secondary)
                                }
                                Text("Build \(item.externalVersionID) · \(item.downloadedAt.formatted(date: .abbreviated, time: .shortened))")
                                    .font(.caption2)
                                    .foregroundStyle(.tertiary)
                                Button {
                                    showVersions = false
                                    model.install(item)
                                } label: {
                                    Label("Install", systemImage: "square.and.arrow.down.on.square")
                                        .font(.caption.weight(.semibold))
                                }
                                .buttonStyle(.bordered)
                                .controlSize(.small)
                            }
                        }
                    }
                }
            }
            .navigationTitle("Versions")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Done") { showVersions = false }
                }
            }
        }
        .presentationDetents([.medium, .large])
    }

    // MARK: Helpers

    private func versionSubtitle(_ version: StoreVersion) -> String {
        var parts = ["Build \(version.externalVersionID)"]
        if let date = version.releaseDateLabel {
            parts.append(date)
        }
        return parts.joined(separator: " · ")
    }

    private func formattedDate(_ raw: String) -> String {
        guard !raw.isEmpty else { return "—" }
        let formatter = ISO8601DateFormatter()
        if let date = formatter.date(from: raw) {
            return date.formatted(date: .abbreviated, time: .omitted)
        }
        return raw
    }

    private var platformIdentifier: String {
        switch model.platform {
        case .iPhone: return "iphone"
        case .iPad: return "ipad"
        case .mac: return "mac"
        case .tv: return "appletv"
        case .watch: return "watch"
        case .vision: return "xros"
        }
    }
}
