// SPDX-License-Identifier: MIT

import SwiftUI
import UniformTypeIdentifiers

struct DownloadsView: View {
    @Environment(AppModel.self) private var model
    @Environment(ThemeStore.self) private var theme
    @State private var showImporter = false
    @State private var itemToDelete: LibraryItem?

    private var isFiltering: Bool {
        !model.librarySearch.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
    }

    var body: some View {
        @Bindable var model = model

        NavigationStack {
            content
                .navigationTitle("Downloads")
                .searchable(text: $model.librarySearch,
                            placement: .navigationBarDrawer(displayMode: .automatic),
                            prompt: "Filter downloaded files")
                .toolbar {
                    ToolbarItemGroup(placement: .topBarTrailing) {
                        Button {
                            showImporter = true
                        } label: {
                            Image(systemName: "square.and.arrow.down")
                        }
                        .accessibilityLabel(String(localized: "Import an IPA file"))
                        AvatarButton()
                    }
                }
                .fileImporter(isPresented: $showImporter,
                              allowedContentTypes: [UTType(filenameExtension: "ipa") ?? .zip],
                              allowsMultipleSelection: true) { result in
                    if case .success(let urls) = result {
                        for url in urls {
                            model.importIPA(from: url)
                        }
                    }
                }
                .confirmationDialog("Delete This Package?",
                                    isPresented: Binding(
                                        get: { itemToDelete != nil },
                                        set: { if !$0 { itemToDelete = nil } }),
                                    titleVisibility: .visible) {
                    Button("Delete", role: .destructive) {
                        if let itemToDelete { model.delete(itemToDelete) }
                    }
                } message: {
                    if let itemToDelete {
                        Text(String(localized: "\(itemToDelete.name) (\(itemToDelete.sizeLabel)) will be removed from this device."))
                    }
                }
                .onAppear {
                    model.refreshLibrary()
                }
                .sensoryFeedback(
                    .success,
                    trigger: model.downloads.filter { $0.state == .finished }.count)
        }
    }

    @ViewBuilder
    private var content: some View {
        if isFiltering && model.downloads.isEmpty && model.visibleLibrary.isEmpty {
            ContentUnavailableView.search(text: model.librarySearch)
        } else if model.downloads.isEmpty && model.visibleLibrary.isEmpty {
            ContentUnavailableView {
                Label("No Downloads Yet", systemImage: "arrow.down.circle")
            } description: {
                Text("Packages you download — or import from the Files app — appear here. Install them on this device or share them elsewhere.")
            } actions: {
                Button("Import an IPA") { showImporter = true }
                    .buttonStyle(.borderedProminent)
                Button("Find Apps") { model.selectedTab = .search }
            }
        } else {
            list
        }
    }

    private var list: some View {
        List {
            if !model.library.isEmpty {
                storageCard
                    .listRowSeparator(.hidden)
                    .listRowInsets(EdgeInsets(top: 8, leading: 20, bottom: 8, trailing: 20))
            }

            if !model.downloads.isEmpty {
                Section {
                    ForEach(model.downloads) { item in
                        DownloadRow(item: item)
                    }
                } header: {
                    HStack {
                        Text("Transfers")
                        Spacer()
                        if model.runningDownloadCount > 0 {
                            Button(String(localized: "Cancel All"), role: .destructive) {
                                model.cancelDownloads()
                            }
                            .font(.system(size: 13))
                            .textCase(nil)
                        } else {
                            Button(String(localized: "Clear")) {
                                model.clearFinishedDownloads()
                            }
                            .font(.system(size: 13))
                            .textCase(nil)
                        }
                    }
                }
            }

            if !model.visibleLibrary.isEmpty {
                Section {
                    ForEach(model.visibleLibrary) { item in
                        LibraryRow(item: item) {
                            itemToDelete = item
                        }
                        .swipeActions(edge: .trailing, allowsFullSwipe: false) {
                            Button(role: .destructive) {
                                itemToDelete = item
                            } label: {
                                Label("Delete", systemImage: "trash")
                            }
                            ShareLink(item: item.fileURL) {
                                Label("Share", systemImage: "square.and.arrow.up")
                            }
                            .tint(theme.accentColor)
                        }
                        .swipeActions(edge: .leading) {
                            Button {
                                model.install(item)
                            } label: {
                                Label("Install", systemImage: "arrow.down.app")
                            }
                            .tint(theme.accentColor)
                        }
                    }
                } header: {
                    Text("Downloaded Packages")
                } footer: {
                    Text("Packages are also visible in the Files app under IPAShelf.")
                }
            }
        }
        .listStyle(.plain)
    }

    private var storageCard: some View {
        HStack(spacing: 12) {
            Image(systemName: "internaldrive")
                .font(.system(size: 22, weight: .medium))
                .foregroundStyle(theme.accentColor)
            VStack(alignment: .leading, spacing: 2) {
                Text(String(localized: "\(model.library.count) packages"))
                    .font(.system(size: 15, weight: .semibold))
                Text(String(localized: "\(ByteCountFormatter.string(fromByteCount: model.libraryTotalSize, countStyle: .file)) on this device"))
                    .font(.system(size: 12))
                    .foregroundStyle(.secondary)
            }
            Spacer()
            Button {
                showImporter = true
            } label: {
                Text("Import")
                    .font(.system(size: 13, weight: .semibold))
            }
            .buttonStyle(.bordered)
            .buttonBorderShape(.capsule)
            .controlSize(.small)
        }
        .padding(14)
        .background {
            RoundedRectangle(cornerRadius: 16, style: .continuous)
                .fill(Color(.secondarySystemBackground))
        }
    }
}

private struct DownloadRow: View {
    @Environment(AppModel.self) private var model
    @Environment(ThemeStore.self) private var theme
    let item: DownloadItem

    var body: some View {
        HStack(spacing: 12) {
            AppArtwork(url: item.app.artworkURL, size: 60)
            VStack(alignment: .leading, spacing: 4) {
                HStack(spacing: 6) {
                    Text(item.app.name)
                        .font(.system(size: 15, weight: .semibold))
                        .lineLimit(1)
                    if let versionID = item.versionID {
                        Text(String(localized: "Build \(versionID)"))
                            .font(.system(size: 11))
                            .foregroundStyle(.tertiary)
                            .lineLimit(1)
                    }
                }
                switch item.state {
                case .downloading, .processing:
                    ProgressView(value: item.state == .processing ? 1 : item.fraction)
                    HStack(spacing: 5) {
                        Text(item.progressLabel)
                        if !item.speedLabel.isEmpty {
                            Text("·")
                            Text(item.speedLabel)
                        }
                    }
                    .font(.system(size: 11))
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                case .finished:
                    Label("Downloaded", systemImage: "checkmark.circle.fill")
                        .font(.system(size: 12, weight: .medium))
                        .foregroundStyle(.green)
                case .failed:
                    Text(item.errorText ?? String(localized: "Download failed"))
                        .font(.system(size: 12))
                        .foregroundStyle(.red)
                        .lineLimit(3)
                case .cancelled:
                    Text("Cancelled")
                        .font(.system(size: 12))
                        .foregroundStyle(.secondary)
                }
            }
            Spacer(minLength: 8)
            trailingControl
        }
        .padding(.vertical, 6)
        .listRowInsets(EdgeInsets(top: 0, leading: 20, bottom: 0, trailing: 20))
        .alignmentGuide(.listRowSeparatorLeading) { dimensions in
            dimensions[.leading] + 72
        }
    }

    @ViewBuilder
    private var trailingControl: some View {
        switch item.state {
        case .downloading, .processing:
            Button {
                model.cancelDownload(item)
            } label: {
                CircularProgressRing(
                    fraction: item.state == .processing ? 1 : item.fraction,
                    size: 30)
                .frame(width: 44, height: 36)
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .accessibilityLabel(String(localized: "Cancel this download"))
        case .failed, .cancelled:
            Button {
                model.retry(item)
            } label: {
                Text("Retry")
                    .font(.system(size: 14, weight: .bold))
                    .foregroundStyle(theme.accentColor)
                    .padding(.horizontal, 12)
                    .frame(height: 30)
                    .background(Capsule().fill(Color(.tertiarySystemFill)))
            }
            .buttonStyle(.plain)
        case .finished:
            if let libraryItem = model.libraryItem(for: item) {
                Button {
                    model.install(libraryItem)
                } label: {
                    Text("Install")
                        .font(.system(size: 14, weight: .bold))
                        .foregroundStyle(theme.accentColor)
                        .padding(.horizontal, 14)
                        .frame(height: 30)
                        .background(Capsule().fill(Color(.tertiarySystemFill)))
                }
                .buttonStyle(.plain)
            }
        }
    }
}

private struct LibraryRow: View {
    @Environment(AppModel.self) private var model
    @Environment(ThemeStore.self) private var theme
    let item: LibraryItem
    let onDelete: () -> Void

    var body: some View {
        HStack(spacing: 12) {
            AppArtwork(url: item.artworkURL, size: 60)
            VStack(alignment: .leading, spacing: 3) {
                Text(item.name)
                    .font(.system(size: 15, weight: .semibold))
                    .lineLimit(1)
                Text("\(versionText) · \(item.sizeLabel)")
                    .font(.system(size: 12))
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                Text(item.downloadedAt.formatted(date: .abbreviated, time: .shortened))
                    .font(.system(size: 11))
                    .foregroundStyle(.tertiary)
                    .lineLimit(1)
            }
            Spacer(minLength: 8)
            Button {
                model.install(item)
            } label: {
                Text("Install")
                    .font(.system(size: 14, weight: .bold))
                    .foregroundStyle(theme.accentColor)
                    .padding(.horizontal, 14)
                    .frame(height: 30)
                    .background(Capsule().fill(Color(.tertiarySystemFill)))
            }
            .buttonStyle(.plain)
            Menu {
                ShareLink(item: item.fileURL) {
                    Label("Share IPA…", systemImage: "square.and.arrow.up")
                }
                if !item.bundleID.isEmpty {
                    Button {
                        UIPasteboard.general.string = item.bundleID
                    } label: {
                        Label("Copy Bundle ID", systemImage: "doc.on.doc")
                    }
                }
                Divider()
                Button(role: .destructive) {
                    onDelete()
                } label: {
                    Label("Delete", systemImage: "trash")
                }
            } label: {
                Image(systemName: "ellipsis.circle")
                    .font(.system(size: 22))
                    .foregroundStyle(.secondary)
            }
        }
        .padding(.vertical, 6)
        .listRowInsets(EdgeInsets(top: 0, leading: 20, bottom: 0, trailing: 20))
        .alignmentGuide(.listRowSeparatorLeading) { dimensions in
            dimensions[.leading] + 72
        }
    }

    private var versionText: String {
        item.version.isEmpty ? item.displayVersion : "v\(item.version)"
    }
}
