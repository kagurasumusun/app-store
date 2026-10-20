// SPDX-License-Identifier: MIT

import SwiftUI

struct SearchView: View {
    @Environment(AppModel.self) private var model
    @State private var showRegionPicker = false
    @State private var searchTask: Task<Void, Never>?

    var body: some View {
        @Bindable var model = model

        NavigationStack {
            ZStack {
                AppBackground()
                content
            }
            .navigationTitle("Search")
            .searchable(text: $model.searchText,
                        placement: .navigationBarDrawer(displayMode: .always),
                        prompt: "App name or bundle ID")
            .onChange(of: model.searchText) { _, text in
                scheduleSearch(for: text)
            }
            .onSubmit(of: .search) {
                let term = model.searchText.trimmingCharacters(in: .whitespacesAndNewlines)
                if term.count >= 2 {
                    Task { await model.search(term: term) }
                }
            }
            .toolbar {
                ToolbarItem(placement: .topBarLeading) {
                    Menu {
                        Picker("Platform", selection: Binding(
                            get: { model.platform },
                            set: { model.setPlatform($0) })) {
                            ForEach(StorePlatform.allCases) { platform in
                                Label(platform.title, systemImage: platform.symbol)
                                    .tag(platform)
                            }
                        }
                    } label: {
                        Label(model.platform.title, systemImage: model.platform.symbol)
                    }
                }
                ToolbarItem(placement: .topBarTrailing) {
                    Button {
                        showRegionPicker = true
                    } label: {
                        Text("\(model.currentStorefront?.flag ?? "🌐") \(model.region)")
                    }
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

    @ViewBuilder
    private var content: some View {
        if model.isSearching && model.searchResults.isEmpty {
            ProgressView("Searching…")
        } else if !model.searchResults.isEmpty {
            resultsList
        } else if model.hasSearched {
            EmptyStateView(symbol: "magnifyingglass",
                           title: "No results",
                           message: "Nothing in the \(model.regionName(model.region)) storefront matches “\(model.searchText)” for \(model.platform.title).")
        } else {
            idleView
        }
    }

    private var resultsList: some View {
        List {
            Section {
                ForEach(model.searchResults) { app in
                    NavigationLink(value: app) {
                        AppRow(app: app)
                    }
                }
            } header: {
                Text("\(model.searchResults.count) results · \(model.regionName(model.region))")
            }
        }
        .listStyle(.insetGrouped)
        .scrollContentBackground(.hidden)
    }

    private var idleView: some View {
        List {
            if model.activeAccount == nil {
                Section {
                    Button {
                        model.showLogin = true
                    } label: {
                        HStack(spacing: 12) {
                            Image(systemName: "person.crop.circle.badge.plus")
                                .font(.title3)
                            VStack(alignment: .leading, spacing: 2) {
                                Text("Sign in with your Apple ID")
                                    .font(.headline)
                                Text("Required to purchase and download apps.")
                                    .font(.caption)
                                    .foregroundStyle(.secondary)
                            }
                            Spacer()
                            Image(systemName: "chevron.right")
                                .font(.caption)
                                .foregroundStyle(.tertiary)
                        }
                    }
                }
            }
            if !model.recentSearches.isEmpty {
                Section {
                    ForEach(model.recentSearches, id: \.self) { term in
                        Button {
                            model.searchText = term
                            Task { await model.search(term: term) }
                        } label: {
                            Label(term, systemImage: "clock.arrow.circlepath")
                        }
                    }
                    Button(role: .destructive) {
                        model.clearRecentSearches()
                    } label: {
                        Text("Clear recent searches")
                    }
                } header: {
                    Text("Recent")
                }
            }
            Section {
                VStack(alignment: .leading, spacing: 8) {
                    Text("Find any app in the App Store, download its package — including older versions — and install it on this device.")
                        .font(.subheadline)
                        .foregroundStyle(.secondary)
                }
                .padding(.vertical, 4)
            }
        }
        .listStyle(.insetGrouped)
        .scrollContentBackground(.hidden)
    }

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
}
