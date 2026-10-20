// SPDX-License-Identifier: MIT

import SwiftUI
import UniformTypeIdentifiers

struct DownloadsView: View {
    @Environment(AppModel.self) private var model
    @State private var showImporter = false
    @State private var itemToDelete: LibraryItem?

    var body: some View {
        @Bindable var model = model

        NavigationStack {
            ZStack {
                AppBackground()
                if model.downloads.isEmpty && model.visibleLibrary.isEmpty && !model.isSearchingFiles {
                    EmptyStateView(symbol: "arrow.down.circle",
                                   title: "No downloads yet",
                                   message: "Packages you download — or import from the Files app — appear here. Install them on this device or share them elsewhere.",
                                   actionTitle: "Import an IPA",
                                   action: { showImporter = true })
                } else {
                    list
                }
            }
            .navigationTitle("Downloads")
            .searchable(text: $model.librarySearch,
                        placement: .navigationBarDrawer(displayMode: .automatic),
                        prompt: "Filter downloaded files")
            .toolbar {
                ToolbarItemGroup(placement: .topBarTrailing) {
                    Button {
                        showImporter = true
                    } label: {
                        Image(systemName: "square.and.arrow.down.on.square")
                    }
                    .accessibilityLabel("Import an IPA file")
                    if model.runningDownloadCount > 0 {
                        Button("Cancel All", role: .destructive) {
                            model.cancelDownloads()
                        }
                    } else if !model.downloads.isEmpty {
                        Button("Clear") {
                            model.clearFinishedDownloads()
                        }
                    }
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
            .confirmationDialog("Delete this package?",
                                isPresented: Binding(
                                    get: { itemToDelete != nil },
                                    set: { if !$0 { itemToDelete = nil } }),
                                titleVisibility: .visible) {
                Button("Delete", role: .destructive) {
                    if let itemToDelete { model.delete(itemToDelete) }
                }
            } message: {
                if let itemToDelete {
                    Text("\(itemToDelete.name) (\(itemToDelete.sizeLabel)) will be removed from this device.")
                }
            }
            .onAppear {
                model.refreshLibrary()
            }
        }
    }

    private var list: some View {
        List {
            if !model.downloads.isEmpty {
                Section {
                    ForEach(model.downloads) { item in
                        DownloadRow(item: item)
                    }
                } header: {
                    Text("Transfers")
                }
            }

            if !model.visibleLibrary.isEmpty {
                Section {
                    ForEach(model.visibleLibrary) { item in
                        LibraryRow(item: item)
                            .swipeActions(edge: .trailing, allowsFullSwipe: false) {
                                Button(role: .destructive) {
                                    itemToDelete = item
                                } label: {
                                    Label("Delete", systemImage: "trash")
                                }
                                ShareLink(item: item.fileURL) {
                                    Label("Share", systemImage: "square.and.arrow.up")
                                }
                                .tint(.indigo)
                            }
                            .swipeActions(edge: .leading) {
                                Button {
                                    model.install(item)
                                } label: {
                                    Label("Install", systemImage: "arrow.down.app")
                                }
                                .tint(.green)
                            }
                    }
                } header: {
                    Text("Downloaded packages")
                } footer: {
                    Text("\(model.library.count) packages · \(ByteCountFormatter.string(fromByteCount: model.libraryTotalSize, countStyle: .file)) on this device. Files are also visible in the Files app under IPAShelf.")
                }
            }
        }
        .listStyle(.insetGrouped)
        .scrollContentBackground(.hidden)
    }
}

private extension AppModel {
    var isSearchingFiles: Bool {
        !librarySearch.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
    }
}

private struct DownloadRow: View {
    @Environment(AppModel.self) private var model
    @Bindable var item: DownloadItem

    var body: some View {
        HStack(spacing: 12) {
            AppArtwork(url: item.app.artworkURL, size: 46, cornerRadius: 11)
            VStack(alignment: .leading, spacing: 4) {
                HStack(spacing: 6) {
                    Text(item.app.name)
                        .font(.subheadline.weight(.semibold))
                        .lineLimit(1)
                    if let versionID = item.versionID {
                        Text("build \(versionID)")
                            .font(.caption2)
                            .foregroundStyle(.tertiary)
                    }
                }
                switch item.state {
                case .downloading, .processing:
                    ProgressView(value: item.state == .processing ? 1 : item.fraction)
                        .tint(.indigo)
                    HStack(spacing: 6) {
                        Text(item.progressLabel)
                        if !item.speedLabel.isEmpty {
                            Text("·")
                            Text(item.speedLabel)
                        }
                    }
                    .font(.caption2)
                    .foregroundStyle(.secondary)
                case .finished:
                    Label("Saved to Downloads", systemImage: "checkmark.circle.fill")
                        .font(.caption)
                        .foregroundStyle(.green)
                case .failed:
                    Text(item.errorText ?? "Download failed")
                        .font(.caption)
                        .foregroundStyle(.red)
                        .lineLimit(2)
                case .cancelled:
                    Text("Cancelled")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }
            Spacer(minLength: 8)
            switch item.state {
            case .downloading, .processing:
                Button {
                    model.cancelDownloads()
                } label: {
                    Image(systemName: "xmark.circle.fill")
                        .font(.title3)
                        .foregroundStyle(.tertiary)
                }
                .buttonStyle(.plain)
            case .failed, .cancelled:
                Button {
                    model.retry(item)
                } label: {
                    Image(systemName: "arrow.clockwise.circle.fill")
                        .font(.title3)
                }
                .buttonStyle(.plain)
                .foregroundStyle(.indigo)
            case .finished:
                if let libraryItem = model.libraryItem(for: item) {
                    Button {
                        model.install(libraryItem)
                    } label: {
                        Text("Install")
                            .font(.subheadline.weight(.semibold))
                    }
                    .buttonStyle(.bordered)
                    .controlSize(.small)
                }
            }
        }
        .padding(.vertical, 4)
    }
}

private struct LibraryRow: View {
    @Environment(AppModel.self) private var model
    let item: LibraryItem

    var body: some View {
        HStack(spacing: 12) {
            AppArtwork(url: item.artworkURL, size: 46, cornerRadius: 11)
            VStack(alignment: .leading, spacing: 3) {
                Text(item.name)
                    .font(.subheadline.weight(.semibold))
                    .lineLimit(1)
                Text("v\(item.displayVersion) · \(item.sizeLabel)")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                Text(item.bundleID.isEmpty
                     ? item.downloadedAt.formatted(date: .abbreviated, time: .shortened)
                     : item.bundleID)
                    .font(.caption2)
                    .foregroundStyle(.tertiary)
                    .lineLimit(1)
            }
            Spacer(minLength: 8)
            Button {
                model.install(item)
            } label: {
                Text("Install")
                    .font(.subheadline.weight(.semibold))
            }
            .buttonStyle(.borderedProminent)
            .controlSize(.small)
            Menu {
                ShareLink(item: item.fileURL) {
                    Label("Share IPA…", systemImage: "square.and.arrow.up")
                }
                Button(role: .destructive) {
                    model.delete(item)
                } label: {
                    Label("Delete", systemImage: "trash")
                }
            } label: {
                Image(systemName: "ellipsis.circle")
                    .font(.title3)
                    .foregroundStyle(.secondary)
            }
        }
        .padding(.vertical, 4)
    }
}
