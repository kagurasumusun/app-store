// SPDX-License-Identifier: MIT

import SwiftUI

struct SearchView: View {
    @Environment(AppModel.self) private var model
    @Environment(ThemeStore.self) private var theme
    @State private var showRegionPicker = false
    @State private var searchTask: Task<Void, Never>?

    var body: some View {
        @Bindable var model = model

        NavigationStack {
            VStack(spacing: 0) {
                controlsBar
                content
            }
            .navigationTitle("Search")
            .searchable(text: $model.searchText,
                        placement: .navigationBarDrawer(displayMode: .always),
                        prompt: "Games, Apps, and More")
            .onChange(of: model.searchText) { _, text in
                scheduleSearch(for: text)
            }
            .onChange(of: model.region) { _, _ in
                rerunSearch()
            }
            .onSubmit(of: .search) {
                let term = model.searchText.trimmingCharacters(in: .whitespacesAndNewlines)
                if term.count >= 2 {
                    Task { await model.search(term: term) }
                }
            }
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    AvatarButton()
                }
            }
            .sheet(isPresented: $showRegionPicker) {
                RegionPickerView()
            }
            .navigationDestination(for: StoreApp.self) { app in
                AppDetailView(app: app)
            }
        }
    }

    // MARK: Controls

    private var controlsBar: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: 8) {
                ForEach(StorePlatform.allCases) { platform in
                    FilterChip(title: platform.title,
                               systemImage: platform.symbol,
                               selected: model.platform == platform) {
                        model.setPlatform(platform)
                        rerunSearch()
                    }
                }
                Divider().frame(height: 22)
                FilterChip(title: "\(model.currentStorefront?.flag ?? "🌐") \(model.region)",
                           selected: false) {
                    showRegionPicker = true
                }
                filtersMenu
            }
            .padding(.horizontal, 20)
            .padding(.vertical, 8)
        }
    }

    private var filtersMenu: some View {
        Menu {
            Picker(String(localized: "Sort by"), selection: sortBinding) {
                ForEach(SearchSort.allCases) { sort in
                    Text(sort.title).tag(sort)
                }
            }
            Toggle(String(localized: "Free only"), isOn: freeOnlyBinding)
            Menu(String(localized: "Category")) {
                Button {
                    setGenre(0)
                } label: {
                    if model.filter.genreID == 0 {
                        Label(String(localized: "All"), systemImage: "checkmark")
                    } else {
                        Text(String(localized: "All"))
                    }
                }
                Divider()
                ForEach(StoreGenre.all) { genre in
                    Button {
                        setGenre(genre.id)
                    } label: {
                        if model.filter.genreID == genre.id {
                            Label(NSLocalizedString(genre.name, comment: "genre"),
                                  systemImage: "checkmark")
                        } else {
                            Text(NSLocalizedString(genre.name, comment: "genre"))
                        }
                    }
                }
            }
            if model.filter.isActive {
                Divider()
                Button(role: .destructive) {
                    resetFilters()
                } label: {
                    Label(String(localized: "Reset Filters"), systemImage: "xmark.circle")
                }
            }
        } label: {
            HStack(spacing: 5) {
                Image(systemName: "slider.horizontal.3")
                    .font(.system(size: 12, weight: .semibold))
                Text("Filters")
                    .font(.system(size: 14, weight: .semibold))
                if model.filter.isActive {
                    Circle()
                        .fill(theme.accentColor)
                        .frame(width: 7, height: 7)
                }
            }
            .padding(.horizontal, 14)
            .frame(height: 32)
            .background {
                Capsule()
                    .strokeBorder(Color.primary.opacity(0.22), lineWidth: 1)
            }
            .foregroundStyle(Color.primary)
        }
    }

    private var sortBinding: Binding<SearchSort> {
        Binding(get: { model.filter.sort },
                set: { model.filter.sort = $0; rerunSearch() })
    }

    private var freeOnlyBinding: Binding<Bool> {
        Binding(get: { model.filter.freeOnly == true },
                set: { model.filter.freeOnly = $0 ? true : nil; rerunSearch() })
    }

    private func setGenre(_ id: Int64) {
        model.filter.genreID = id
        rerunSearch()
    }

    private func resetFilters() {
        model.filter.freeOnly = nil
        model.filter.genreID = 0
        model.filter.sort = .relevance
        rerunSearch()
    }

    // MARK: Content

    @ViewBuilder
    private var content: some View {
        if model.isSearching && model.searchResults.isEmpty {
            List {
                ForEach(0..<6, id: \.self) { _ in
                    SkeletonAppRow()
                }
            }
            .listStyle(.plain)
        } else if !model.searchResults.isEmpty {
            resultsList
        } else if model.hasSearched {
            ContentUnavailableView.search(text: model.searchText)
        } else {
            idleView
        }
    }

    private var resultsList: some View {
        List {
            // The App Store shows bare results — no count header.
            ForEach(model.searchResults) { app in
                AppListRow(app: app)
            }
        }
        .listStyle(.plain)
        .animation(.default, value: model.searchResults.map(\.appID))
    }

    private var idleView: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 28) {
                if model.activeAccount == nil {
                    signInCard
                }
                if !model.recentSearches.isEmpty {
                    recentSearches
                }
                discoverSection
            }
            .padding(.horizontal, 20)
            .padding(.top, 8)
            .padding(.bottom, 24)
        }
    }

    private var signInCard: some View {
        Button {
            model.showLogin = true
        } label: {
            VStack(alignment: .leading, spacing: 10) {
                Image(systemName: "person.crop.circle.badge.plus")
                    .font(.system(size: 30, weight: .medium))
                Text("Sign in with your Apple ID")
                    .font(.system(size: 20, weight: .bold))
                Text("Required to get free apps and download packages — including older versions.")
                    .font(.system(size: 14))
                    .foregroundStyle(.white.opacity(0.85))
                    .multilineTextAlignment(.leading)
                Text("Sign In")
                    .font(.system(size: 15, weight: .bold))
                    .foregroundStyle(theme.accentColor)
                    .padding(.horizontal, 18)
                    .frame(height: 34)
                    .background(Capsule().fill(Color.white))
                    .padding(.top, 2)
            }
            .foregroundStyle(.white)
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(20)
            .background {
                RoundedRectangle(cornerRadius: 20, style: .continuous)
                    .fill(LinearGradient(
                        colors: [theme.accentColor, theme.accentColor.opacity(0.65)],
                        startPoint: .topLeading, endPoint: .bottomTrailing))
            }
        }
        .buttonStyle(.plain)
    }

    private var recentSearches: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(alignment: .firstTextBaseline) {
                SectionHeader(title: "Recent Searches")
                Spacer()
                Button(String(localized: "Clear")) {
                    model.clearRecentSearches()
                }
                .font(.system(size: 14))
            }
            VStack(spacing: 0) {
                ForEach(Array(model.recentSearches.enumerated()), id: \.element) { index, term in
                    Button {
                        model.searchText = term
                        Task { await model.search(term: term) }
                    } label: {
                        HStack(spacing: 10) {
                            Image(systemName: "clock.arrow.circlepath")
                                .foregroundStyle(.secondary)
                            Text(term)
                                .foregroundStyle(.primary)
                                .lineLimit(1)
                            Spacer()
                            Image(systemName: "arrow.up.backward")
                                .font(.caption2.weight(.semibold))
                                .foregroundStyle(.tertiary)
                        }
                        .padding(.vertical, 10)
                        .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                    if index < model.recentSearches.count - 1 {
                        Divider().padding(.leading, 30)
                    }
                }
            }
        }
    }

    /// Browse shortcuts in the App Store Search-tab style, replacing the
    /// old marketing card: the storefront charts and the purchase
    /// history are one tap away, and the one power feature worth
    /// surfacing (exact-identifier search) is a single footnote.
    private var discoverSection: some View {
        VStack(alignment: .leading, spacing: 6) {
            SectionHeader(title: "Discover")
            VStack(spacing: 0) {
                discoverRow(symbol: "chart.bar.fill",
                            title: String(localized: "Top Charts")) {
                    model.selectedTab = .apps
                }
                Divider().padding(.leading, 42)
                discoverRow(symbol: "bag.fill",
                            title: String(localized: "Purchased")) {
                    model.selectedTab = .purchased
                }
            }
            Text("Tip: search by bundle ID or paste an App Store link. Older versions live on each app's page.")
                .font(.system(size: 12))
                .foregroundStyle(.tertiary)
                .padding(.top, 10)
        }
    }

    private func discoverRow(symbol: String,
                             title: String,
                             action: @escaping () -> Void) -> some View {
        Button(action: action) {
            HStack(spacing: 12) {
                Image(systemName: symbol)
                    .font(.system(size: 16, weight: .medium))
                    .foregroundStyle(theme.accentColor)
                    .frame(width: 30)
                Text(title)
                    .font(.system(size: 16))
                    .foregroundStyle(.primary)
                Spacer()
                Image(systemName: "chevron.right")
                    .font(.system(size: 12, weight: .semibold))
                    .foregroundStyle(.tertiary)
            }
            .padding(.vertical, 11)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
    }

    // MARK: Search scheduling

    private func scheduleSearch(for text: String) {
        searchTask?.cancel()
        let term = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard term.count >= 2 else {
            if term.isEmpty { model.clearSearch() }
            return
        }
        searchTask = Task {
            try? await Task.sleep(for: .milliseconds(400))
            guard !Task.isCancelled else { return }
            await model.search(term: term)
        }
    }

    private func rerunSearch() {
        let term = model.searchText.trimmingCharacters(in: .whitespacesAndNewlines)
        guard model.hasSearched, term.count >= 2 else { return }
        Task { await model.search(term: term) }
    }
}
