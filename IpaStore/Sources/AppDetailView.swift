// SPDX-License-Identifier: MIT

import SwiftUI

struct AppDetailView: View {
    @Environment(AppModel.self) private var model
    @Environment(ThemeStore.self) private var theme
    let app: StoreApp

    @State private var showAllDescription = false
    @State private var showAllNotes = false
    @State private var showAllVersions = false
    @State private var isLoadingVersions = false
    @State private var showNavHeader = false

    var body: some View {
        ScrollView {
            VStack(spacing: 0) {
                header
                    .padding(.horizontal, 20)
                    .padding(.top, 14)
                statsStrip
                    .padding(.top, 20)
                Divider().padding(.top, 18).padding(.horizontal, 20)
                versionsSection
                    .padding(.horizontal, 20)
                    .padding(.vertical, 18)
                if !app.releaseNotes.isEmpty {
                    Divider().padding(.horizontal, 20)
                    whatsNewSection
                        .padding(.horizontal, 20)
                        .padding(.vertical, 18)
                }
                if !app.gallery.isEmpty {
                    Divider().padding(.horizontal, 20)
                    previewSection
                        .padding(.vertical, 18)
                }
                if !app.description.isEmpty {
                    Divider().padding(.horizontal, 20)
                    descriptionSection
                        .padding(.horizontal, 20)
                        .padding(.vertical, 18)
                }
                Divider().padding(.horizontal, 20)
                informationSection
                    .padding(.horizontal, 20)
                    .padding(.vertical, 18)
            }
            .padding(.bottom, 24)
        }
        .onScrollGeometryChange(for: Bool.self) { geometry in
            geometry.contentOffset.y > 120
        } action: { _, newValue in
            showNavHeader = newValue
        }
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .principal) {
                if showNavHeader {
                    HStack(spacing: 8) {
                        AppArtwork(url: app.artworkURL, size: 28)
                        Text(app.name)
                            .font(.system(size: 15, weight: .semibold))
                            .lineLimit(1)
                    }
                    .transition(.opacity)
                }
            }
            ToolbarItem(placement: .topBarTrailing) {
                if showNavHeader {
                    GetButton(app: app)
                        .transition(.opacity)
                }
            }
        }
        .animation(.easeInOut(duration: 0.18), value: showNavHeader)
    }

    // MARK: Header

    private var header: some View {
        HStack(alignment: .top, spacing: 16) {
            AppArtwork(url: app.artworkURL, size: 104)
            VStack(alignment: .leading, spacing: 4) {
                Text(app.name)
                    .font(.system(size: 21, weight: .bold))
                    .lineLimit(3)
                Text(app.artistName.isEmpty ? app.seller : app.artistName)
                    .font(.system(size: 14))
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                if !app.platforms.isEmpty {
                    Text(app.platforms.map(platformDisplayName).joined(separator: " · "))
                        .font(.system(size: 12))
                        .foregroundStyle(.tertiary)
                        .lineLimit(1)
                }
                HStack(alignment: .center, spacing: 12) {
                    GetButton(app: app)
                    Spacer(minLength: 0)
                    if let url = app.trackViewURL {
                        ShareLink(item: url) {
                            Image(systemName: "square.and.arrow.up")
                                .font(.system(size: 20, weight: .medium))
                                .foregroundStyle(theme.accentColor)
                        }
                    }
                }
                .padding(.top, 8)
                if !app.isFree && !model.isOwned(app.appID) {
                    Text("Paid apps can only be downloaded after buying them in the App Store. Purchases made there appear under Purchased.")
                        .font(.system(size: 12))
                        .foregroundStyle(.secondary)
                        .padding(.top, 4)
                } else if app.hasInAppPurchases {
                    Text("Offers In-App Purchases")
                        .font(.system(size: 12))
                        .foregroundStyle(.tertiary)
                        .padding(.top, 4)
                }
            }
        }
    }

    // MARK: Statistics strip

    private var statsStrip: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: 20) {
                if app.ratingCount > 0 {
                    StatColumn(value: app.ratingLabel,
                               caption: app.ratingCountLabel,
                               rating: app.averageRating)
                }
                if app.contentAdvisoryRating.isEmpty == false {
                    VStack(spacing: 3) {
                        Text(app.contentAdvisoryRating)
                            .font(.system(size: 15, weight: .bold))
                            .foregroundStyle(.primary.opacity(0.82))
                            .padding(.horizontal, 7)
                            .padding(.vertical, 2)
                            .overlay {
                                RoundedRectangle(cornerRadius: 5, style: .continuous)
                                    .strokeBorder(Color.secondary, lineWidth: 1.2)
                            }
                            .frame(height: 23)
                        Text("Age")
                            .font(.system(size: 12))
                            .foregroundStyle(.secondary)
                    }
                    .frame(minWidth: 60)
                }
                if !app.primaryGenre.isEmpty {
                    StatColumn(value: app.primaryGenre,
                               caption: String(localized: "Category"))
                }
                if app.byteSize > 0 {
                    StatColumn(value: app.sizeLabel,
                               caption: String(localized: "Size"))
                }
                if !app.languageCodes.isEmpty {
                    StatColumn(value: app.languageCodes[0].uppercased(),
                               caption: app.languageCodes.count > 1
                                   ? String(localized: "+ \(app.languageCodes.count - 1) More")
                                   : String(localized: "Language"))
                }
                if !app.minimumOSVersion.isEmpty {
                    StatColumn(value: "\(app.minimumOSVersion)+",
                               caption: String(localized: "Minimum iOS"))
                }
            }
            .padding(.horizontal, 20)
        }
    }

    // MARK: Version history

    private var versions: [StoreVersion] {
        model.versions[app.appID] ?? []
    }

    private var visibleVersions: [StoreVersion] {
        showAllVersions ? versions : Array(versions.prefix(5))
    }

    @ViewBuilder
    private var versionsSection: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(alignment: .firstTextBaseline) {
                SectionHeader(title: "Version History")
                Spacer()
                if isLoadingVersions {
                    ProgressView().controlSize(.small)
                } else if !versions.isEmpty {
                    Text(String(localized: "\(versions.count) versions"))
                        .font(.system(size: 13))
                        .foregroundStyle(.secondary)
                    Button {
                        Task { await loadVersions(force: true) }
                    } label: {
                        Image(systemName: "arrow.clockwise")
                            .font(.system(size: 14, weight: .semibold))
                    }
                    .accessibilityLabel(String(localized: "Reload versions"))
                }
            }

            if isLoadingVersions && versions.isEmpty {
                VStack(spacing: 14) {
                    ForEach(0..<3, id: \.self) { _ in
                        HStack {
                            VStack(alignment: .leading, spacing: 6) {
                                RoundedRectangle(cornerRadius: 4, style: .continuous)
                                    .fill(Color(.secondarySystemFill))
                                    .frame(width: 130, height: 12)
                                RoundedRectangle(cornerRadius: 4, style: .continuous)
                                    .fill(Color(.secondarySystemFill))
                                    .frame(width: 84, height: 9)
                            }
                            Spacer()
                            Circle().fill(Color(.secondarySystemFill)).frame(width: 28, height: 28)
                        }
                    }
                }
                .redacted(reason: .placeholder)
            } else if versions.isEmpty {
                versionsEmptyCard
            } else {
                VStack(spacing: 0) {
                    ForEach(Array(visibleVersions.enumerated()), id: \.element.id) { index, version in
                        versionRow(version)
                        if index < visibleVersions.count - 1 {
                            Divider()
                        }
                    }
                }
                if versions.count > 5 {
                    Button {
                        showAllVersions.toggle()
                    } label: {
                        Text(showAllVersions
                             ? String(localized: "Show Less")
                             : String(localized: "See All \(versions.count) Versions"))
                            .font(.system(size: 15, weight: .semibold))
                    }
                    .padding(.top, 2)
                }
            }
        }
    }

    private var versionsEmptyCard: some View {
        VStack(spacing: 12) {
            Image(systemName: "clock.arrow.circlepath")
                .font(.system(size: 26, weight: .medium))
                .foregroundStyle(theme.accentColor)
            Text("Every version Apple still serves for this app — not just the latest — can be downloaded here.")
                .font(.system(size: 13))
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
            if let error = model.versionErrors[app.appID] {
                Text(error)
                    .font(.system(size: 12))
                    .foregroundStyle(.red)
                    .multilineTextAlignment(.center)
            }
            if model.activeAccount == nil {
                Button {
                    model.showLogin = true
                } label: {
                    Text("Sign In")
                        .font(.system(size: 15, weight: .semibold))
                }
                .buttonStyle(.borderedProminent)
            } else {
                Button {
                    Task { await loadVersions(force: true) }
                } label: {
                    Text("Load Version History")
                        .font(.system(size: 15, weight: .semibold))
                }
                .buttonStyle(.borderedProminent)
                .disabled(isLoadingVersions)
            }
        }
        .frame(maxWidth: .infinity)
        .padding(18)
        .background {
            RoundedRectangle(cornerRadius: 16, style: .continuous)
                .fill(Color(.secondarySystemBackground))
        }
    }

    private func versionRow(_ version: StoreVersion) -> some View {
        HStack(spacing: 12) {
            VStack(alignment: .leading, spacing: 2) {
                HStack(spacing: 6) {
                    Text(version.title)
                        .font(.system(size: 15, weight: .semibold))
                    if version.isLatest {
                        Text("Latest")
                            .font(.system(size: 10, weight: .bold))
                            .padding(.horizontal, 6)
                            .padding(.vertical, 2)
                            .background(Capsule().fill(theme.accentColor.opacity(0.14)))
                            .foregroundStyle(theme.accentColor)
                    }
                }
                Text(version.releaseDateLabel ?? String(localized: "Build \(version.externalVersionID)"))
                    .font(.system(size: 12))
                    .foregroundStyle(.secondary)
            }
            Spacer()
            GetButton(app: app, versionID: version.externalVersionID)
        }
        .padding(.vertical, 9)
        .task(id: version.externalVersionID) {
            await model.enrichVersion(version, for: app)
        }
    }

    private func loadVersions(force: Bool = false) async {
        isLoadingVersions = true
        defer { isLoadingVersions = false }
        await model.loadVersions(for: app, force: force)
    }

    // MARK: What's New / Preview / Description

    private var whatsNewSection: some View {
        VStack(alignment: .leading, spacing: 8) {
            SectionHeader(title: "What's New")
            HStack {
                Text(String(localized: "Version \(app.version)"))
                Spacer()
                if !app.currentVersionReleaseDate.isEmpty {
                    Text(String(app.currentVersionReleaseDate.prefix(10)))
                }
            }
            .font(.system(size: 13))
            .foregroundStyle(.secondary)
            Text(app.releaseNotes)
                .font(.system(size: 15))
                .lineLimit(showAllNotes ? nil : 4)
            if app.releaseNotes.count > 280 {
                Button {
                    showAllNotes.toggle()
                } label: {
                    Text(showAllNotes
                         ? String(localized: "Show Less")
                         : String(localized: "Show More"))
                        .font(.system(size: 14, weight: .medium))
                }
            }
        }
    }

    private var previewSection: some View {
        VStack(alignment: .leading, spacing: 12) {
            SectionHeader(title: "Preview")
                .padding(.horizontal, 20)
            ScreenshotStrip(urls: app.gallery)
        }
    }

    private var descriptionSection: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(app.description)
                .font(.system(size: 15))
                .lineLimit(showAllDescription ? nil : 5)
            if app.description.count > 320 {
                Button {
                    showAllDescription.toggle()
                } label: {
                    Text(showAllDescription
                         ? String(localized: "Show Less")
                         : String(localized: "Show More"))
                        .font(.system(size: 14, weight: .medium))
                }
            }
        }
    }

    // MARK: Information

    private var informationRows: [(label: String, value: String)] {
        var rows: [(String, String)] = []
        let seller = app.seller.isEmpty ? app.artistName : app.seller
        if !seller.isEmpty {
            rows.append((String(localized: "Seller"), seller))
        }
        if app.byteSize > 0 {
            rows.append((String(localized: "Size"), app.sizeLabel))
        }
        if !app.primaryGenre.isEmpty {
            rows.append((String(localized: "Category"), app.primaryGenre))
        }
        if !app.minimumOSVersion.isEmpty {
            rows.append((String(localized: "Compatibility"),
                         String(localized: "Requires iOS \(app.minimumOSVersion) or later")))
        }
        if !app.languageCodes.isEmpty {
            let names = app.languageCodes.prefix(4).map { code in
                Locale.current.localizedString(forLanguageCode: code) ?? code.uppercased()
            }
            var value = names.joined(separator: ", ")
            if app.languageCodes.count > 4 {
                value += String(localized: " and \(app.languageCodes.count - 4) more")
            }
            rows.append((String(localized: "Languages"), value))
        }
        if !app.contentAdvisoryRating.isEmpty {
            rows.append((String(localized: "Age Rating"), app.contentAdvisoryRating))
        }
        rows.append((String(localized: "Price"), app.priceLabel))
        if app.hasInAppPurchases {
            rows.append((String(localized: "In-App Purchases"), String(localized: "Yes")))
        }
        if !app.releaseDate.isEmpty {
            rows.append((String(localized: "Released"), String(app.releaseDate.prefix(10))))
        }
        if !app.currentVersionReleaseDate.isEmpty {
            rows.append((String(localized: "Updated"), String(app.currentVersionReleaseDate.prefix(10))))
        }
        if let purchaseDate = app.purchaseDate {
            rows.append((String(localized: "Purchased"),
                         purchaseDate.formatted(date: .abbreviated, time: .omitted)))
        }
        rows.append((String(localized: "Bundle ID"), app.bundleID))
        rows.append((String(localized: "App ID"), String(app.appID)))
        return rows
    }

    private var informationSection: some View {
        VStack(alignment: .leading, spacing: 4) {
            SectionHeader(title: "Information")
                .padding(.bottom, 6)
            VStack(spacing: 0) {
                ForEach(Array(informationRows.enumerated()), id: \.offset) { index, row in
                    InfoRow(label: row.label, value: row.value)
                        .padding(.vertical, 9)
                    if index < informationRows.count - 1 {
                        Divider()
                    }
                }
            }
        }
    }
}
