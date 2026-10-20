// SPDX-License-Identifier: MIT

import SwiftUI
import UIKit

struct LibraryView: View {
    @Environment(AppModel.self) private var model
    @State private var expanded: Set<String> = []
    @State private var pendingDeletion: LibraryGroup?

    var body: some View {
        @Bindable var model = model

        NavigationStack {
            ZStack {
                AppBackground()
                content
            }
            .navigationTitle("Library")
            .navigationBarTitleDisplayMode(.large)
            .searchable(text: $model.librarySearch, prompt: "Name, bundle ID or version")
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    Menu {
                        Picker("Sort", selection: $model.librarySort) {
                            ForEach(AppModel.LibrarySort.allCases) { sort in
                                Text(sort.title).tag(sort)
                            }
                        }
                        Divider()
                        Button {
                            model.refreshLibrary()
                        } label: {
                            Label("Refresh", systemImage: "arrow.clockwise")
                        }
                    } label: {
                        Image(systemName: "line.3.horizontal.decrease.circle")
                    }
                }
            }
            .confirmationDialog("Delete every version of this app?",
                                isPresented: Binding(
                                    get: { pendingDeletion != nil },
                                    set: { if !$0 { pendingDeletion = nil } }),
                                titleVisibility: .visible) {
                Button("Delete all versions", role: .destructive) {
                    if let group = pendingDeletion { model.delete(group) }
                    pendingDeletion = nil
                }
                Button("Cancel", role: .cancel) { pendingDeletion = nil }
            } message: {
                Text(pendingDeletion?.name ?? "")
            }
        }
    }

    @ViewBuilder
    private var content: some View {
        if model.library.isEmpty {
            EmptyStateView(
                symbol: "square.stack.3d.up.slash",
                title: "Nothing downloaded",
                message: "Packages you download appear here. Each app keeps every version you saved, so you can install an older build later.",
                actionTitle: "Browse the App Store",
                action: nil)
        } else {
            List {
                Section {
                    HStack {
                        Label("\(model.library.count) packages", systemImage: "shippingbox")
                        Spacer()
                        Text(ByteCountFormatter.string(fromByteCount: model.libraryTotalSize,
                                                       countStyle: .file))
                            .foregroundStyle(.secondary)
                    }
                    .font(.footnote)
                }

                ForEach(model.libraryGroups) { group in
                    Section {
                        groupHeader(group)
                        if expanded.contains(group.id) {
                            ForEach(group.items) { item in
                                versionRow(item: item)
                            }
                        }
                    }
                }
            }
            .listStyle(.insetGrouped)
            .scrollContentBackground(.hidden)
        }
    }

    private func groupHeader(_ group: LibraryGroup) -> some View {
        Button {
            withAnimation(.snappy(duration: 0.2)) {
                if expanded.contains(group.id) {
                    expanded.remove(group.id)
                } else {
                    expanded.insert(group.id)
                }
            }
        } label: {
            HStack(spacing: 12) {
                AppArtwork(url: group.latest?.artworkURL, size: 50, cornerRadius: 12)
                VStack(alignment: .leading, spacing: 3) {
                    Text(group.name)
                        .font(.headline)
                        .lineLimit(1)
                    Text(group.bundleID.isEmpty ? group.versionsLabel : group.bundleID)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                    Text("\(group.versionsLabel) · \(group.sizeLabel)")
                        .font(.caption2)
                        .foregroundStyle(.tertiary)
                }
                Spacer()
                Image(systemName: expanded.contains(group.id) ? "chevron.up" : "chevron.down")
                    .font(.caption.weight(.semibold))
                    .foregroundStyle(.tertiary)
            }
        }
        .buttonStyle(.plain)
        .swipeActions(edge: .trailing, allowsFullSwipe: false) {
            Button(role: .destructive) {
                pendingDeletion = group
            } label: {
                Label("Delete all", systemImage: "trash")
            }
        }
    }

    private func versionRow(item: LibraryItem) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(alignment: .firstTextBaseline) {
                Text(item.displayVersion)
                    .font(.subheadline.weight(.semibold))
                Spacer()
                Text(item.sizeLabel)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            Text("Build \(item.externalVersionID.isEmpty ? "unknown" : item.externalVersionID) · \(item.downloadedAt.formatted(date: .abbreviated, time: .shortened))")
                .font(.caption2)
                .foregroundStyle(.tertiary)
            HStack(spacing: 8) {
                Button {
                    model.install(item)
                } label: {
                    Label("Install", systemImage: "square.and.arrow.down.on.square")
                }
                .buttonStyle(.bordered)
                .controlSize(.small)

                ShareLink(item: item.fileURL) {
                    Label("Share", systemImage: "square.and.arrow.up")
                        .font(.caption)
                }
                .buttonStyle(.bordered)
                .controlSize(.small)

                Spacer()

                NavigationLink {
                    PackageInspector(item: item)
                } label: {
                    Text("Details")
                        .font(.caption)
                }
                .buttonStyle(.bordered)
                .controlSize(.small)
            }
        }
        .padding(.vertical, 2)
        .swipeActions(edge: .trailing) {
            Button(role: .destructive) {
                model.delete(item)
            } label: {
                Label("Delete", systemImage: "trash")
            }
        }
    }
}

/// Shows the sidecar metadata stored next to an IPA.
struct PackageInspector: View {
    let item: LibraryItem

    var body: some View {
        List {
            Section("Package") {
                InfoRow(label: "Name", value: item.name)
                InfoRow(label: "Bundle ID", value: item.bundleID.isEmpty ? "—" : item.bundleID)
                InfoRow(label: "Version", value: item.displayVersion)
                InfoRow(label: "Build", value: item.externalVersionID.isEmpty ? "—" : item.externalVersionID)
                InfoRow(label: "App ID", value: item.appID.map { String($0) } ?? "—")
                InfoRow(label: "Size", value: item.sizeLabel)
                InfoRow(label: "Downloaded", value: item.downloadedAt.formatted(date: .abbreviated, time: .shortened))
            }
            Section("Location") {
                Text(item.fileURL.path)
                    .font(.caption.monospaced())
                    .textSelection(.enabled)
                ShareLink(item: item.fileURL) {
                    Label("Share IPA", systemImage: "square.and.arrow.up")
                }
            }
        }
        .navigationTitle(item.name)
        .navigationBarTitleDisplayMode(.inline)
    }
}
