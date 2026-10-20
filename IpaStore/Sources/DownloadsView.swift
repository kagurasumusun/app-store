// SPDX-License-Identifier: MIT

import SwiftUI
import UIKit

struct DownloadsView: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        NavigationStack {
            ZStack {
                AppBackground()
                content
            }
            .navigationTitle("Downloads")
            .navigationBarTitleDisplayMode(.large)
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    Menu {
                        Button("Cancel active downloads", systemImage: "xmark.circle") {
                            model.cancelDownloads()
                        }
                        Button("Clear finished", systemImage: "trash") {
                            model.clearFinishedDownloads()
                        }
                    } label: {
                        Image(systemName: "ellipsis.circle")
                    }
                }
            }
        }
    }

    @ViewBuilder
    private var content: some View {
        if model.downloads.isEmpty {
            EmptyStateView(
                symbol: "arrow.down.circle",
                title: "No downloads yet",
                message: "Downloads run in the background. You can keep browsing while a package is fetched, verified and patched.")
        } else {
            List {
                ForEach(model.downloads) { item in
                    DownloadRow(item: item)
                        .listRowBackground(Color(.secondarySystemGroupedBackground).opacity(0.65))
                }
            }
            .listStyle(.plain)
            .scrollContentBackground(.hidden)
        }
    }
}

private struct DownloadRow: View {
    let item: DownloadItem
    @Environment(AppModel.self) private var model

    var body: some View {
        VStack(alignment: .leading, spacing: 9) {
            HStack(spacing: 12) {
                AppArtwork(url: item.app.artworkURL, size: 46, cornerRadius: 11)
                VStack(alignment: .leading, spacing: 2) {
                    Text(item.app.name)
                        .font(.headline)
                        .lineLimit(1)
                    Text(subtitle)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                }
                Spacer()
                stateBadge
            }

            if item.state == .downloading || item.state == .processing {
                ProgressView(value: min(max(item.fraction, 0), 1))
                    .tint(.indigo)
                HStack {
                    Text(item.progressLabel)
                    Spacer()
                    Text(item.speedLabel)
                }
                .font(.caption2)
                .foregroundStyle(.secondary)
            }

            if let error = item.errorText, item.state == .failed {
                Text(error)
                    .font(.caption)
                    .foregroundStyle(.red)
            }

            HStack(spacing: 8) {
                if item.state == .failed {
                    Button {
                        model.retry(item)
                    } label: {
                        Label("Retry", systemImage: "arrow.clockwise")
                    }
                    .buttonStyle(.bordered)
                    .controlSize(.small)
                }
                if item.state == .finished, let library = model.libraryItem(for: item) {
                    Button {
                        model.install(library)
                    } label: {
                        Label("Install", systemImage: "square.and.arrow.down.on.square")
                    }
                    .buttonStyle(.borderedProminent)
                    .controlSize(.small)
                }
                Spacer()
                Text(item.startedAt.formatted(date: .omitted, time: .shortened))
                    .font(.caption2)
                    .foregroundStyle(.tertiary)
            }
        }
        .padding(.vertical, 6)
    }

    private var subtitle: String {
        var parts: [String] = []
        if let versionID = item.versionID, !versionID.isEmpty {
            parts.append("Build \(versionID)")
        } else {
            parts.append("Latest build")
        }
        if !item.app.bundleID.isEmpty { parts.append(item.app.bundleID) }
        return parts.joined(separator: " · ")
    }

    @ViewBuilder
    private var stateBadge: some View {
        switch item.state {
        case .downloading:
            Label("Downloading", systemImage: "arrow.down")
                .labelStyle(.titleAndIcon)
        case .processing:
            Label("Preparing", systemImage: "gearshape.2")
        case .finished:
            Label("Done", systemImage: "checkmark.circle.fill")
                .foregroundStyle(.green)
        case .failed:
            Label("Failed", systemImage: "exclamationmark.triangle.fill")
                .foregroundStyle(.red)
        case .cancelled:
            Label("Cancelled", systemImage: "xmark.circle")
                .foregroundStyle(.secondary)
        }
    }
}
