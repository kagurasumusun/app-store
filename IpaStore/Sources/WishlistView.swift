// SPDX-License-Identifier: MIT

import SwiftUI

/// The saved apps that the user wants to keep an eye on.
///
/// Entries are stored as snapshots (id, name, bundle id, artwork) so the list
/// renders instantly and offline; the detail view re-resolves the full record
/// from the current storefront when it is opened.
struct WishlistView: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        NavigationStack {
            Group {
                if model.wishlist.isEmpty {
                    EmptyStateView(
                        symbol: "heart",
                        title: "No saved apps",
                        message: "Tap the heart on an app's page to keep it here. Saved apps stay available even when you switch storefronts.",
                        actionTitle: "Browse the App Store",
                        action: { model.selectedTab = .search })
                } else {
                    List {
                        Section {
                            ForEach(model.wishlist) { entry in
                                NavigationLink {
                                    WishlistDetailLoader(entry: entry)
                                } label: {
                                    WishlistRow(entry: entry)
                                }
                                .listRowBackground(Color(.secondarySystemGroupedBackground).opacity(0.65))
                                .swipeActions(edge: .trailing, allowsFullSwipe: true) {
                                    Button(role: .destructive) {
                                        model.removeWishlist(appID: entry.appID)
                                    } label: {
                                        Label("Remove", systemImage: "trash")
                                    }
                                }
                            }
                        } header: {
                            Text("\(model.wishlist.count) saved")
                        } footer: {
                            Text("Saved apps are kept on this device only and are not synced to your account.")
                        }
                    }
                    .scrollContentBackground(.hidden)
                }
            }
            .background(AppBackground())
            .navigationTitle("Wishlist")
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    if !model.wishlist.isEmpty {
                        Button(role: .destructive) {
                            model.clearWishlist()
                        } label: {
                            Label("Clear", systemImage: "trash")
                        }
                    }
                }
            }
        }
    }
}

private struct WishlistRow: View {
    let entry: WishlistEntry

    var body: some View {
        HStack(spacing: 14) {
            AppArtwork(url: entry.url, size: 56)
            VStack(alignment: .leading, spacing: 3) {
                Text(entry.name)
                    .font(.headline)
                    .lineLimit(1)
                Text(entry.bundleID)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                Text("Saved \(entry.addedAt.formatted(date: .abbreviated, time: .omitted))")
                    .font(.caption2)
                    .foregroundStyle(.tertiary)
            }
        }
    }
}
