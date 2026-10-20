// SPDX-License-Identifier: MIT

import SwiftUI

/// The signed-in account's purchase history, straight from Apple's DAAP
/// endpoint (the same list the App Store's Purchased page shows). Rows open
/// the app's detail page, where every issued build can be downloaded; the
/// row button downloads the current build immediately.
struct PurchasedView: View {
    @Environment(AppModel.self) private var model
    @State private var path = NavigationPath()

    var body: some View {
        @Bindable var model = model

        NavigationStack(path: $path) {
            Group {
                if model.activeAccount == nil {
                    EmptyStateView(
                        symbol: "bag",
                        title: "Not signed in",
                        message: "Sign in with your Apple ID to browse every app you have ever purchased or downloaded for free, including ones Apple no longer lists in search.",
                        actionTitle: "Sign In",
                        action: { model.showLogin = true })
                } else {
                    list
                }
            }
            .background(AppBackground())
            .navigationTitle("Purchased")
            .navigationDestination(for: StoreApp.self) { app in
                AppDetailView(app: app)
            }
            .searchable(text: $model.ownedSearch,
                        placement: .navigationBarDrawer(displayMode: .always),
                        prompt: "Name or bundle ID")
            .toolbar { filterMenu }
            .task(id: model.activeAccount?.email) {
                if !model.hasLoadedOwned {
                    await model.loadOwned(reset: true)
                }
            }
            .refreshable {
                await model.loadOwned(reset: true)
            }
        }
    }

    private var list: some View {
        List {
            if let message = model.ownedError {
                Section {
                    VStack(alignment: .leading, spacing: 10) {
                        Label("Could not load your purchase history",
                              systemImage: "exclamationmark.triangle")
                            .font(.subheadline.weight(.semibold))
                        Text(message)
                            .font(.footnote)
                            .foregroundStyle(.secondary)
                        HStack(spacing: 10) {
                            Button("Retry") {
                                Task { await model.loadOwned(reset: true) }
                            }
                            .buttonStyle(.bordered)
                            .controlSize(.small)
                            if model.ownedNeedsSignIn {
                                Button("Sign In Again") { model.showLogin = true }
                                    .buttonStyle(.borderedProminent)
                                    .controlSize(.small)
                            }
                        }
                    }
                    .padding(.vertical, 4)
                }
                .listRowBackground(Color(.secondarySystemGroupedBackground).opacity(0.65))
            }

            Section {
                if model.isLoadingOwned, model.ownedApps.isEmpty {
                    HStack(spacing: 10) {
                        ProgressView()
                        Text("Loading your purchases…")
                            .foregroundStyle(.secondary)
                    }
                } else if model.hasLoadedOwned, model.visibleOwnedApps.isEmpty {
                    Text(model.ownedSearch.isEmpty
                         ? "No purchases match this filter."
                         : "No purchases match “\(model.ownedSearch)”.")
                        .foregroundStyle(.secondary)
                }
                ForEach(model.visibleOwnedApps) { app in
                    OwnedAppRow(app: app) {
                        path.append(app)
                    }
                    .listRowBackground(Color(.secondarySystemGroupedBackground).opacity(0.65))
                    .swipeActions(edge: .leading, allowsFullSwipe: true) {
                        Button {
                            model.download(app)
                        } label: {
                            Label("Download", systemImage: "arrow.down.circle")
                        }
                        .tint(.indigo)
                    }
                    .onAppear {
                        if app.appID == model.ownedApps.last?.appID,
                           model.ownedSearch.isEmpty {
                            Task { await model.loadOwned() }
                        }
                    }
                }
                if model.isLoadingOwned, !model.ownedApps.isEmpty {
                    HStack(spacing: 10) {
                        ProgressView()
                        Text("Loading more…")
                            .foregroundStyle(.secondary)
                    }
                }
            } header: {
                Text(headerTitle)
            } footer: {
                if model.hasLoadedOwned {
                    Text("Every app \(model.activeAccount?.email ?? "this account") has acquired\(model.ownedPlatformFilter.map { " for \($0.title)" } ?? ""), newest first. Apps Apple has since delisted can still be re-downloaded from here.")
                }
            }
        }
        .scrollContentBackground(.hidden)
    }

    private var headerTitle: String {
        if model.ownedTotal > 0 {
            return "\(model.ownedApps.count) of \(model.ownedTotal) apps"
        }
        return "Purchase history"
    }

    @ToolbarContentBuilder
    private var filterMenu: some ToolbarContent {
        ToolbarItem(placement: .topBarTrailing) {
            Menu {
                Picker("Platform", selection: filterBinding) {
                    Text("All Platforms").tag(StorePlatform?.none)
                    ForEach(Self.filterablePlatforms, id: \.self) { platform in
                        Text(platform.title).tag(StorePlatform?.some(platform))
                    }
                }
            } label: {
                Label(model.ownedPlatformFilter?.title ?? "Filter",
                      systemImage: "line.3.horizontal.decrease.circle")
            }
        }
    }

    /// Apple Watch purchases cannot be listed on their own (the history
    /// endpoint has no watch media kind), so it is not offered as a filter.
    private static let filterablePlatforms: [StorePlatform] =
        [.iPhone, .iPad, .mac, .tv, .vision]

    private var filterBinding: Binding<StorePlatform?> {
        Binding(
            get: { model.ownedPlatformFilter },
            set: { model.setOwnedPlatformFilter($0) })
    }
}

private struct OwnedAppRow: View {
    let app: StoreApp
    let open: () -> Void
    @Environment(AppModel.self) private var model

    var body: some View {
        HStack(spacing: 14) {
            AppArtwork(url: app.artworkURL, size: 56)
            VStack(alignment: .leading, spacing: 3) {
                Text(app.name)
                    .font(.headline)
                    .lineLimit(1)
                Text(subtitle)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                if !platformNames.isEmpty {
                    HStack(spacing: 4) {
                        ForEach(platformNames.prefix(3), id: \.self) { name in
                            PlatformChip(title: name)
                        }
                    }
                }
            }
            Spacer(minLength: 8)
            Button {
                model.download(app)
            } label: {
                Image(systemName: "arrow.down.circle.fill")
                    .font(.title3)
                    .foregroundStyle(.indigo)
            }
            .buttonStyle(.borderless)
            .accessibilityLabel("Download \(app.name)")
            Image(systemName: "chevron.right")
                .font(.caption.weight(.semibold))
                .foregroundStyle(.tertiary)
        }
        .padding(.vertical, 4)
        .contentShape(Rectangle())
        .onTapGesture { open() }
    }

    private var subtitle: String {
        var parts: [String] = []
        if !app.version.isEmpty { parts.append("Version \(app.version)") }
        if let date = app.purchaseDate {
            parts.append("Purchased \(date.formatted(date: .abbreviated, time: .omitted))")
        }
        if parts.isEmpty { parts.append(app.bundleID) }
        return parts.joined(separator: " · ")
    }

    private var platformNames: [String] {
        app.platforms
            .map(platformDisplayName)
            .filter { !$0.isEmpty && $0.lowercased() != "unknown" }
    }
}
