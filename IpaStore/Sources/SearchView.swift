// SPDX-License-Identifier: MIT

import SwiftUI
import UIKit

struct SearchView: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        @Bindable var model = model

        NavigationStack {
            ZStack {
                AppBackground()
                VStack(spacing: 0) {
                    filterBar
                    content
                }
            }
            .navigationTitle("App Store")
            .navigationBarTitleDisplayMode(.large)
            .searchable(text: $model.searchText,
                        placement: .navigationBarDrawer(displayMode: .always),
                        prompt: "Apps, games and more")
            .autocorrectionDisabled()
            .textInputAutocapitalization(.never)
            .task(id: model.searchText) {
                await model.searchIfNeeded()
            }
            .onChange(of: model.platform) { _, _ in
                Task { await model.searchIfNeeded() }
            }
            .onChange(of: model.region) { _, _ in
                Task { await model.searchIfNeeded() }
            }
            .toolbar {
                ToolbarItem(placement: .topBarLeading) {
                    platformMenu
                }
                ToolbarItem(placement: .topBarTrailing) {
                    accountButton
                }
            }
            .sheet(isPresented: $model.showRegionPicker) {
                RegionPickerView()
            }
        }
    }

    private var platformMenu: some View {
        Menu {
            Picker("Platform", selection: Binding(
                get: { model.platform },
                set: { model.setPlatform($0) })) {
                ForEach(StorePlatform.allCases) { platform in
                    Label(platform.title, systemImage: platform.symbol)
                        .tag(platform)
                }
            }
            Divider()
            Button {
                model.showRegionPicker = true
            } label: {
                Label("Storefront: \(model.regionName(model.region))",
                      systemImage: "globe")
            }
        } label: {
            HStack(spacing: 4) {
                Image(systemName: model.platform.symbol)
                Text(model.currentStorefront?.flag ?? model.region)
            }
            .font(.subheadline.weight(.semibold))
        }
    }

    private var accountButton: some View {
        Button {
            model.showAccounts = true
        } label: {
            if let account = model.activeAccount {
                Label(account.name.isEmpty ? account.email : account.name,
                      systemImage: "person.crop.circle.fill")
                    .labelStyle(.iconOnly)
                    .foregroundStyle(.indigo)
            } else {
                Text("Sign In")
                    .font(.subheadline.weight(.semibold))
            }
        }
    }

    private var filterBar: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: 8) {
                Menu {
                    Button("Any price") { model.applyFilter { $0.freeOnly = nil } }
                    Button("Free only") { model.applyFilter { $0.freeOnly = true } }
                    Button("Paid only") { model.applyFilter { $0.freeOnly = false } }
                } label: {
                    filterChip(title: priceFilterTitle,
                               symbol: "tag",
                               active: model.filter.freeOnly != nil)
                }

                Menu {
                    Button("All categories") { model.applyFilter { $0.genreID = 0 } }
                    Divider()
                    ForEach(StoreGenre.all) { genre in
                        Button(genre.name) { model.applyFilter { $0.genreID = genre.id } }
                    }
                } label: {
                    filterChip(title: genreTitle,
                               symbol: "square.grid.2x2",
                               active: model.filter.genreID != 0)
                }

                Menu {
                    Picker("Sort", selection: Binding(
                        get: { model.filter.sort },
                        set: { value in model.applyFilter { $0.sort = value } })) {
                        ForEach(SearchSort.allCases) { sort in
                            Text(sort.title).tag(sort)
                        }
                    }
                } label: {
                    filterChip(title: "Sort: \(model.filter.sort.title)",
                               symbol: "arrow.up.arrow.down",
                               active: model.filter.sort != .relevance)
                }

                if model.filter.isActive {
                    Button {
                        model.applyFilter { filter in
                            filter.freeOnly = nil
                            filter.genreID = 0
                            filter.sort = .relevance
                        }
                    } label: {
                        filterChip(title: "Reset", symbol: "xmark", active: false)
                    }
                }
            }
            .padding(.horizontal, 16)
            .padding(.vertical, 8)
        }
    }

    private var priceFilterTitle: String {
        switch model.filter.freeOnly {
        case .some(true): return "Free only"
        case .some(false): return "Paid only"
        case .none: return "Any price"
        }
    }

    private var genreTitle: String {
        StoreGenre.all.first { $0.id == model.filter.genreID }?.name ?? "All categories"
    }

    private func filterChip(title: String, symbol: String, active: Bool) -> some View {
        Label(title, systemImage: symbol)
            .font(.caption.weight(.semibold))
            .padding(.horizontal, 11)
            .padding(.vertical, 6)
            .background(
                Capsule().fill(active ? Color.indigo.opacity(0.16)
                                      : Color(.tertiarySystemFill)))
            .foregroundStyle(active ? Color.indigo : Color.secondary)
    }

    @ViewBuilder
    private var content: some View {
        if model.isSearching && model.searchResults.isEmpty {
            ProgressView("Searching the App Store…")
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        } else if model.hasSearched && model.searchResults.isEmpty {
            EmptyStateView(
                symbol: "magnifyingglass",
                title: "No results",
                message: "Nothing matched “\(model.searchText)” in \(model.regionName(model.region)). Try another spelling, clear the filters, or switch the storefront.")
        } else if model.searchResults.isEmpty {
            ScrollView {
                VStack(alignment: .leading, spacing: 22) {
                    if !model.recentSearches.isEmpty {
                        recentSearches
                    }
                    if !model.wishlist.isEmpty {
                        wishlistSection
                    }
                    EmptyStateView(
                        symbol: "sparkle.magnifyingglass",
                        title: "Find an app",
                        message: "Search any storefront, inspect screenshots, ratings and reviews, download a version you own, then install it on this device.",
                        actionTitle: model.activeAccount == nil ? "Sign In" : nil,
                        action: model.activeAccount == nil ? { model.showLogin = true } : nil)
                }
                .padding(.top, 12)
            }
        } else {
            List(model.searchResults) { app in
                NavigationLink {
                    AppDetailView(app: app)
                } label: {
                    AppRow(app: app)
                }
                .listRowBackground(Color(.secondarySystemGroupedBackground).opacity(0.65))
            }
            .listStyle(.plain)
            .scrollContentBackground(.hidden)
        }
    }

    private var recentSearches: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Text("Recent searches")
                    .font(.headline)
                Spacer()
                Button("Clear") { model.clearRecentSearches() }
                    .font(.caption)
            }
            .padding(.horizontal, 20)
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: 8) {
                    ForEach(model.recentSearches, id: \.self) { term in
                        Button {
                            model.searchText = term
                        } label: {
                            Label(term, systemImage: "clock.arrow.circlepath")
                                .font(.caption.weight(.semibold))
                                .padding(.horizontal, 11)
                                .padding(.vertical, 6)
                                .background(Capsule().fill(Color(.tertiarySystemFill)))
                                .foregroundStyle(.secondary)
                        }
                    }
                }
                .padding(.horizontal, 20)
            }
        }
    }

    private var wishlistSection: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("Wishlist")
                .font(.headline)
                .padding(.horizontal, 20)
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: 14) {
                    ForEach(model.wishlist) { entry in
                        NavigationLink {
                            WishlistDetailLoader(entry: entry)
                        } label: {
                            VStack(alignment: .leading, spacing: 6) {
                                AppArtwork(url: entry.url, size: 64, cornerRadius: 15)
                                Text(entry.name)
                                    .font(.caption.weight(.semibold))
                                    .lineLimit(1)
                                    .frame(width: 76, alignment: .leading)
                            }
                        }
                        .buttonStyle(.plain)
                    }
                }
                .padding(.horizontal, 20)
            }
        }
    }
}

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
                            .font(.caption2)
                            .foregroundStyle(.secondary)
                        Text("·")
                            .foregroundStyle(.tertiary)
                    }
                    Text(app.primaryGenre)
                    if app.byteSize > 0 {
                        Text("·")
                            .foregroundStyle(.tertiary)
                        Text(app.sizeLabel)
                    }
                }
                .font(.caption2)
                .foregroundStyle(.tertiary)
                .lineLimit(1)
            }
            Spacer(minLength: 8)
            VStack(alignment: .trailing, spacing: 6) {
                Text(app.priceLabel)
                    .font(.subheadline.weight(.semibold))
                    .foregroundStyle(app.isFree ? Color.indigo : Color.secondary)
                Image(systemName: "chevron.down.circle")
                    .font(.caption)
                    .foregroundStyle(.tertiary)
            }
            .padding(.horizontal, 10)
            .padding(.vertical, 5)
            .background(Capsule().fill(Color(.tertiarySystemFill)))
        }
        .padding(.vertical, 6)
        .contentShape(Rectangle())
    }
}

/// Resolves a wishlist entry back into a full catalog record before pushing
/// the detail view, because the wishlist only stores a snapshot.
struct WishlistDetailLoader: View {
    let entry: WishlistEntry
    @Environment(AppModel.self) private var model
    @State private var app: StoreApp?

    var body: some View {
        Group {
            if let app {
                AppDetailView(app: app)
            } else {
                ProgressView("Loading \(entry.name)…")
                    .task {
                        app = try? await model.service.lookup(appID: entry.appID,
                                                             bundleID: entry.bundleID,
                                                             region: model.region,
                                                             platform: model.platform)
                    }
            }
        }
    }
}

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
