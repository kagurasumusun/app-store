// SPDX-License-Identifier: MIT

import SwiftUI

struct PurchasedView: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        @Bindable var model = model

        NavigationStack {
            VStack(spacing: 0) {
                if model.activeAccount != nil {
                    platformBar
                }
                content
            }
            .navigationTitle("Purchased")
            .searchable(text: $model.ownedSearch,
                        placement: .navigationBarDrawer(displayMode: .automatic),
                        prompt: "Search purchased apps")
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    AvatarButton()
                }
            }
            .navigationDestination(for: StoreApp.self) { app in
                AppDetailView(app: app)
            }
            .task {
                if !model.hasLoadedOwned {
                    await model.loadOwned(reset: true)
                }
            }
            .onChange(of: model.activeAccount?.email) { _, _ in
                Task { await model.loadOwned(reset: true) }
            }
        }
    }

    private var platformBar: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: 8) {
                FilterChip(title: String(localized: "All"),
                           selected: model.ownedPlatformFilter == nil) {
                    model.setOwnedPlatformFilter(nil)
                }
                ForEach(StorePlatform.allCases) { platform in
                    FilterChip(title: platform.title,
                               systemImage: platform.symbol,
                               selected: model.ownedPlatformFilter == platform) {
                        model.setOwnedPlatformFilter(platform)
                    }
                }
            }
            .padding(.horizontal, 16)
            .padding(.vertical, 8)
        }
    }

    @ViewBuilder
    private var content: some View {
        if model.activeAccount == nil {
            ContentUnavailableView {
                Label("Sign In Required", systemImage: "person.crop.circle")
            } description: {
                Text("Your purchased apps are tied to your Apple ID.")
            } actions: {
                Button("Sign In") { model.showLogin = true }
                    .buttonStyle(.borderedProminent)
            }
        } else if model.isLoadingOwned && model.ownedApps.isEmpty {
            List {
                ForEach(0..<6, id: \.self) { _ in
                    SkeletonAppRow()
                }
            }
            .listStyle(.plain)
        } else if let error = model.ownedError, model.ownedApps.isEmpty {
            ContentUnavailableView {
                Label("Couldn't Load Purchases",
                      systemImage: model.ownedNeedsSignIn
                          ? "person.crop.circle.badge.exclamationmark"
                          : "exclamationmark.triangle")
            } description: {
                Text(error)
            } actions: {
                Button(model.ownedNeedsSignIn
                       ? String(localized: "Sign In Again")
                       : String(localized: "Retry")) {
                    if model.ownedNeedsSignIn {
                        model.showLogin = true
                    } else {
                        Task { await model.loadOwned(reset: true) }
                    }
                }
                .buttonStyle(.borderedProminent)
            }
        } else if model.visibleOwnedApps.isEmpty {
            if model.ownedSearch.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
                ContentUnavailableView {
                    Label("No Apps", systemImage: "bag")
                } description: {
                    Text("This account owns no apps on the selected platform.")
                }
            } else {
                ContentUnavailableView.search(text: model.ownedSearch)
            }
        } else {
            List {
                Section {
                    ForEach(model.visibleOwnedApps) { app in
                        AppListRow(app: app, ownedContext: true)
                            .task(id: app.appID) {
                                if app.appID == model.visibleOwnedApps.last?.appID,
                                   model.ownedHasMore {
                                    await model.loadOwned()
                                }
                            }
                    }
                } header: {
                    if model.ownedTotal > 0 {
                        Text(String(localized: "\(model.ownedTotal) apps"))
                            .font(.caption)
                            .textCase(nil)
                    }
                }
                if model.isLoadingOwned {
                    HStack {
                        Spacer()
                        ProgressView()
                        Spacer()
                    }
                    .listRowSeparator(.hidden)
                }
            }
            .listStyle(.plain)
            .refreshable {
                await model.loadOwned(reset: true)
            }
        }
    }
}
