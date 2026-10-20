// SPDX-License-Identifier: MIT

import SwiftUI

struct PurchasedView: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        @Bindable var model = model

        NavigationStack {
            ZStack {
                AppBackground()
                content
            }
            .navigationTitle("Purchased")
            .searchable(text: $model.ownedSearch,
                        placement: .navigationBarDrawer(displayMode: .automatic),
                        prompt: "Filter my apps")
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    platformMenu
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

    private var platformMenu: some View {
        Menu {
            Button {
                model.setOwnedPlatformFilter(nil)
            } label: {
                if model.ownedPlatformFilter == nil {
                    Label("All platforms", systemImage: "checkmark")
                } else {
                    Text("All platforms")
                }
            }
            Divider()
            ForEach(StorePlatform.allCases) { platform in
                Button {
                    model.setOwnedPlatformFilter(platform)
                } label: {
                    if model.ownedPlatformFilter == platform {
                        Label(platform.title, systemImage: "checkmark")
                    } else {
                        Label(platform.title, systemImage: platform.symbol)
                    }
                }
            }
        } label: {
            Image(systemName: "line.3.horizontal.decrease.circle")
        }
    }

    @ViewBuilder
    private var content: some View {
        if model.activeAccount == nil {
            EmptyStateView(symbol: "person.crop.circle.badge.plus",
                           title: "Sign in required",
                           message: "Your purchased apps are tied to your Apple ID.",
                           actionTitle: "Sign In",
                           action: { model.showLogin = true })
        } else if model.isLoadingOwned && model.ownedApps.isEmpty {
            ProgressView("Loading purchase history…")
        } else if let error = model.ownedError, model.ownedApps.isEmpty {
            EmptyStateView(symbol: model.ownedNeedsSignIn ? "person.crop.circle.badge.exclamationmark" : "exclamationmark.triangle",
                           title: "Could not load purchases",
                           message: error,
                           actionTitle: model.ownedNeedsSignIn ? "Sign In Again" : "Retry",
                           action: {
                               if model.ownedNeedsSignIn {
                                   model.showLogin = true
                               } else {
                                   Task { await model.loadOwned(reset: true) }
                               }
                           })
        } else if model.visibleOwnedApps.isEmpty {
            EmptyStateView(symbol: "bag",
                           title: "No apps",
                           message: model.ownedSearch.isEmpty
                               ? "This account owns no apps on the selected platform."
                               : "Nothing matches “\(model.ownedSearch)”.")
        } else {
            List {
                Section {
                    ForEach(model.visibleOwnedApps) { app in
                        NavigationLink(value: app) {
                            AppRow(app: app)
                        }
                        .task(id: app.appID) {
                            if app.appID == model.visibleOwnedApps.last?.appID,
                               model.ownedHasMore {
                                await model.loadOwned()
                            }
                        }
                    }
                } header: {
                    if model.ownedTotal > 0 {
                        Text("\(model.ownedTotal) apps · \(model.activeAccount?.email ?? "")")
                    }
                }
                if model.isLoadingOwned {
                    Section {
                        HStack {
                            Spacer()
                            ProgressView()
                            Spacer()
                        }
                    }
                }
            }
            .listStyle(.insetGrouped)
            .scrollContentBackground(.hidden)
            .refreshable {
                await model.loadOwned(reset: true)
            }
        }
    }
}
