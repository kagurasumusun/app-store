// SPDX-License-Identifier: MIT

import SwiftUI

/// Purchase history in the App Store's shape: whose purchases these are
/// (with account switching for multi-account use), an
/// All / Downloaded / Not Downloaded scope, and the app rows
/// themselves. Platform and sort live in the toolbar filter menu where
/// they are one tap away without crowding the list.
struct PurchasedView: View {
    @Environment(AppModel.self) private var model
    @Environment(ThemeStore.self) private var theme

    var body: some View {
        @Bindable var model = model

        NavigationStack {
            VStack(spacing: 0) {
                if model.activeAccount != nil {
                    Picker("Scope", selection: $model.ownedSegment) {
                        ForEach(OwnedSegment.allCases) { segment in
                            Text(segment.title).tag(segment)
                        }
                    }
                    .pickerStyle(.segmented)
                    .padding(.horizontal, 20)
                    .padding(.vertical, 8)
                }
                content
            }
            .navigationTitle("Purchased")
            .searchable(text: $model.ownedSearch,
                        placement: .navigationBarDrawer(displayMode: .automatic),
                        prompt: "Search purchased apps")
            .toolbar {
                ToolbarItemGroup(placement: .topBarTrailing) {
                    if model.activeAccount != nil {
                        filterMenu
                    }
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

    // MARK: Filter menu

    private var filtersActive: Bool {
        model.ownedPlatformFilter != nil || model.ownedSort != .recent
    }

    private var platformBinding: Binding<StorePlatform?> {
        Binding(get: { model.ownedPlatformFilter },
                set: { model.setOwnedPlatformFilter($0) })
    }

    private var sortBinding: Binding<OwnedSort> {
        Binding(get: { model.ownedSort },
                set: { model.ownedSort = $0 })
    }

    private var filterMenu: some View {
        Menu {
            Picker("Platform", selection: platformBinding) {
                Text("All Platforms").tag(StorePlatform?.none)
                ForEach(StorePlatform.allCases) { platform in
                    Label(platform.title, systemImage: platform.symbol)
                        .tag(StorePlatform?.some(platform))
                }
            }
            Picker("Sort", selection: sortBinding) {
                ForEach(OwnedSort.allCases) { sort in
                    Text(sort.title).tag(sort)
                }
            }
        } label: {
            Image(systemName: filtersActive
                  ? "line.3.horizontal.decrease.circle.fill"
                  : "line.3.horizontal.decrease.circle")
        }
        .accessibilityLabel(String(localized: "Filter"))
    }

    // MARK: Content

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
            emptyState
        } else {
            List {
                accountHeaderSection
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
                } footer: {
                    Text(String(localized: "\(model.visibleOwnedApps.count) apps"))
                        .font(.footnote)
                        .frame(maxWidth: .infinity, alignment: .center)
                        .padding(.top, 4)
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

    // MARK: Account header

    @ViewBuilder
    private var accountHeaderSection: some View {
        if let account = model.activeAccount {
            Section {
                Menu {
                    ForEach(model.accounts) { candidate in
                        Button {
                            model.setActive(candidate)
                        } label: {
                            if candidate == account {
                                Label(candidate.name.isEmpty ? candidate.email : candidate.name,
                                      systemImage: "checkmark")
                            } else {
                                Text(candidate.name.isEmpty ? candidate.email : candidate.name)
                            }
                        }
                    }
                    Divider()
                    Button {
                        model.showLogin = true
                    } label: {
                        Label("Add Account", systemImage: "plus")
                    }
                } label: {
                    HStack(spacing: 12) {
                        Text(account.initial)
                            .font(.system(size: 17, weight: .bold))
                            .foregroundStyle(.white)
                            .frame(width: 42, height: 42)
                            .background {
                                Circle().fill(LinearGradient(
                                    colors: [theme.accentColor, theme.accentColor.opacity(0.6)],
                                    startPoint: .top, endPoint: .bottom))
                            }
                        VStack(alignment: .leading, spacing: 2) {
                            Text(account.name.isEmpty ? account.email : account.name)
                                .font(.system(size: 16, weight: .semibold))
                                .foregroundStyle(.primary)
                                .lineLimit(1)
                            Text(account.email)
                                .font(.system(size: 12))
                                .foregroundStyle(.secondary)
                                .lineLimit(1)
                        }
                        Spacer()
                        Image(systemName: "chevron.up.chevron.down")
                            .font(.system(size: 12, weight: .semibold))
                            .foregroundStyle(.tertiary)
                    }
                    .padding(.vertical, 4)
                    .contentShape(Rectangle())
                }
                .listRowInsets(EdgeInsets(top: 4, leading: 20, bottom: 4, trailing: 20))
            }
        }
    }

    // MARK: Empty states

    @ViewBuilder
    private var emptyState: some View {
        if !model.ownedSearch.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            ContentUnavailableView.search(text: model.ownedSearch)
        } else if model.ownedHasMore {
            // Everything loaded so far is outside the selected scope;
            // keep paging until something matches or history runs out.
            VStack(spacing: 12) {
                ProgressView()
                Text("Loading…")
                    .font(.system(size: 13))
                    .foregroundStyle(.secondary)
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .task(id: model.ownedApps.count) {
                if model.ownedHasMore {
                    await model.loadOwned()
                }
            }
        } else {
            switch model.ownedSegment {
            case .all:
                ContentUnavailableView {
                    Label("No Apps", systemImage: "bag")
                } description: {
                    Text("This account owns no apps on the selected platform.")
                }
            case .downloaded:
                ContentUnavailableView {
                    Label("No Downloaded Apps", systemImage: "arrow.down.circle")
                } description: {
                    Text("Apps you download from your purchases will appear here.")
                }
            case .notDownloaded:
                ContentUnavailableView {
                    Label("All Downloaded", systemImage: "checkmark.circle")
                } description: {
                    Text("Every purchased app is already in your library.")
                }
            }
        }
    }
}
