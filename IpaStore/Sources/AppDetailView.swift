// SPDX-License-Identifier: MIT

import SwiftUI

struct AppDetailView: View {
    @Environment(AppModel.self) private var model
    let app: StoreApp

    @State private var isPurchasing = false
    @State private var showAllDescription = false
    @State private var isLoadingVersions = false

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 22) {
                header
                actions
                if !app.gallery.isEmpty {
                    ScreenshotStrip(urls: app.gallery)
                        .padding(.horizontal, -20)
                }
                versionsSection
                if !app.description.isEmpty {
                    descriptionSection
                }
                if !app.releaseNotes.isEmpty {
                    releaseNotesSection
                }
                informationSection
            }
            .padding(.horizontal, 20)
            .padding(.vertical, 16)
        }
        .background(AppBackground())
        .navigationTitle(app.name)
        .navigationBarTitleDisplayMode(.inline)
    }

    // MARK: Header

    private var header: some View {
        HStack(alignment: .top, spacing: 16) {
            AppArtwork(url: app.artworkURL, size: 96, cornerRadius: 22)
            VStack(alignment: .leading, spacing: 5) {
                Text(app.name)
                    .font(.title3.weight(.bold))
                    .lineLimit(2)
                Text(app.artistName.isEmpty ? app.seller : app.artistName)
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                HStack(spacing: 8) {
                    if app.ratingCount > 0 {
                        StarRating(rating: app.averageRating, size: 11)
                        Text(app.ratingCountLabel)
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    } else {
                        Text(app.primaryGenre)
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                }
                HStack(spacing: 8) {
                    PlatformChip(title: model.platform.title, highlighted: true)
                    if model.isOwned(app.appID) {
                        Label("Purchased", systemImage: "checkmark.circle.fill")
                            .font(.caption2.weight(.semibold))
                            .foregroundStyle(.green)
                    }
                }
            }
        }
    }

    // MARK: Actions

    private var actions: some View {
        VStack(spacing: 10) {
            if model.activeAccount == nil {
                Button {
                    model.showLogin = true
                } label: {
                    Label("Sign In to Download", systemImage: "person.crop.circle")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.borderedProminent)
                .controlSize(.large)
            } else {
                Button {
                    model.download(app)
                } label: {
                    Label("Download\(app.version.isEmpty ? "" : " (v\(app.version))")",
                          systemImage: "arrow.down.circle.fill")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.borderedProminent)
                .controlSize(.large)
                .disabled(!app.isFree)

                if app.isFree, !model.isOwned(app.appID) {
                    Button {
                        Task {
                            isPurchasing = true
                            await model.purchase(app)
                            isPurchasing = false
                        }
                    } label: {
                        if isPurchasing {
                            ProgressView()
                                .frame(maxWidth: .infinity)
                        } else {
                            Label("Get (add to my purchases)", systemImage: "bag")
                                .frame(maxWidth: .infinity)
                        }
                    }
                    .buttonStyle(.bordered)
                    .disabled(isPurchasing)
                }
                if !app.isFree {
                    Text("Paid apps can only be downloaded after buying them in the App Store. Purchases made there appear under Purchased.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }
        }
    }

    // MARK: Version history

    private var versions: [StoreVersion] {
        model.versions[app.appID] ?? []
    }

    @ViewBuilder
    private var versionsSection: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Text("Version History")
                    .font(.headline)
                Spacer()
                if isLoadingVersions {
                    ProgressView().controlSize(.small)
                } else if versions.isEmpty {
                    Button("Load") {
                        Task { await loadVersions() }
                    }
                    .font(.subheadline)
                } else {
                    Button {
                        Task { await loadVersions(force: true) }
                    } label: {
                        Image(systemName: "arrow.clockwise")
                    }
                    .font(.subheadline)
                }
            }

            if let error = model.versionErrors[app.appID], versions.isEmpty {
                Text(error)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } else if versions.isEmpty {
                Text(model.activeAccount == nil
                     ? "Sign in to list every version Apple still serves for this app."
                     : "Load the list to download an older version.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } else {
                VStack(spacing: 0) {
                    ForEach(Array(versions.enumerated()), id: \.element.id) { index, version in
                        versionRow(version)
                        if index < versions.count - 1 {
                            Divider().padding(.leading, 4)
                        }
                    }
                }
                .padding(12)
                .background(
                    RoundedRectangle(cornerRadius: 16, style: .continuous)
                        .fill(Color(.secondarySystemGroupedBackground).opacity(0.75)))
            }
        }
    }

    private func versionRow(_ version: StoreVersion) -> some View {
        HStack(spacing: 10) {
            VStack(alignment: .leading, spacing: 2) {
                HStack(spacing: 6) {
                    Text(version.title)
                        .font(.subheadline.weight(.medium))
                    if version.isLatest {
                        Text("Latest")
                            .font(.caption2.weight(.semibold))
                            .padding(.horizontal, 6)
                            .padding(.vertical, 2)
                            .background(Capsule().fill(Color.indigo.opacity(0.15)))
                            .foregroundStyle(.indigo)
                    }
                }
                Text(version.releaseDateLabel ?? "Build \(version.externalVersionID)")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            Spacer()
            Button {
                model.download(app, versionID: version.externalVersionID)
            } label: {
                Image(systemName: "arrow.down.circle")
                    .font(.title3)
            }
            .buttonStyle(.plain)
            .foregroundStyle(.indigo)
            .disabled(model.activeAccount == nil || !app.isFree)
        }
        .padding(.vertical, 7)
        .task(id: version.externalVersionID) {
            await model.enrichVersion(version, for: app)
        }
    }

    private func loadVersions(force: Bool = false) async {
        isLoadingVersions = true
        defer { isLoadingVersions = false }
        await model.loadVersions(for: app, force: force)
    }

    // MARK: Description / notes / information

    private var descriptionSection: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Description")
                .font(.headline)
            Text(app.description)
                .font(.subheadline)
                .lineLimit(showAllDescription ? nil : 6)
            if app.description.count > 400 {
                Button(showAllDescription ? "Show less" : "Show more") {
                    showAllDescription.toggle()
                }
                .font(.caption)
            }
        }
    }

    private var releaseNotesSection: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("What's New")
                .font(.headline)
            Text(app.releaseNotes)
                .font(.subheadline)
                .foregroundStyle(.secondary)
        }
    }

    private var informationSection: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("Information")
                .font(.headline)
            VStack(spacing: 8) {
                InfoRow(label: "Bundle ID", value: app.bundleID)
                InfoRow(label: "App ID", value: String(app.appID))
                InfoRow(label: "Price", value: app.priceLabel)
                if app.byteSize > 0 {
                    InfoRow(label: "Size", value: app.sizeLabel)
                }
                if !app.minimumOSVersion.isEmpty {
                    InfoRow(label: "Requires", value: "iOS \(app.minimumOSVersion)+")
                }
                if !app.primaryGenre.isEmpty {
                    InfoRow(label: "Genre", value: app.primaryGenre)
                }
                InfoRow(label: "Rating", value: app.advisoryLabel)
                if !app.releaseDate.isEmpty {
                    InfoRow(label: "Released", value: String(app.releaseDate.prefix(10)))
                }
                if !app.currentVersionReleaseDate.isEmpty {
                    InfoRow(label: "Updated", value: String(app.currentVersionReleaseDate.prefix(10)))
                }
                if app.hasInAppPurchases {
                    InfoRow(label: "In-app purchases", value: "Yes")
                }
                if let purchaseDate = app.purchaseDate {
                    InfoRow(label: "Purchased",
                            value: purchaseDate.formatted(date: .abbreviated, time: .omitted))
                }
            }
            .padding(12)
            .background(
                RoundedRectangle(cornerRadius: 16, style: .continuous)
                    .fill(Color(.secondarySystemGroupedBackground).opacity(0.75)))
        }
    }
}
