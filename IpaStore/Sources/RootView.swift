// SPDX-License-Identifier: MIT

import SwiftUI

struct RootView: View {
    @Environment(AppModel.self) private var model
    @Environment(ThemeStore.self) private var theme

    var body: some View {
        @Bindable var model = model

        TabView(selection: $model.selectedTab) {
            Tab(AppTab.apps.title, systemImage: AppTab.apps.systemImage, value: AppTab.apps) {
                HomeView()
            }
            Tab(AppTab.search.title, systemImage: AppTab.search.systemImage, value: AppTab.search) {
                SearchView()
            }
            Tab(AppTab.purchased.title, systemImage: AppTab.purchased.systemImage, value: AppTab.purchased) {
                PurchasedView()
            }
            Tab(AppTab.downloads.title, systemImage: AppTab.downloads.systemImage, value: AppTab.downloads) {
                DownloadsView()
            }
            .badge(model.runningDownloadCount)
        }
        .tabViewBottomAccessory(
            isEnabled: model.runningDownloadCount > 0 && model.selectedTab != .downloads
        ) {
            DownloadingAccessory()
        }
        .sheet(isPresented: Binding(
            get: { model.installService.isPresented },
            set: { model.installService.isPresented = $0 })) {
            InstallSheet()
        }
        .sheet(isPresented: $model.showLogin) {
            LoginView()
        }
        .sheet(isPresented: $model.showAccount) {
            AccountHubView()
        }
        .alert(model.alert?.title ?? String(localized: "Error"),
               isPresented: Binding(
                   get: { model.alert != nil },
                   set: { presented in
                       if !presented { model.alert = nil }
                   })) {
            Button("OK", role: .cancel) {}
        } message: {
            Text(model.alert?.message ?? "")
        }
    }
}

/// Compact now-downloading pill floating above the tab bar, so a transfer
/// started from Search stays visible without switching tabs.
private struct DownloadingAccessory: View {
    @Environment(AppModel.self) private var model
    @Environment(ThemeStore.self) private var theme

    private var firstActive: DownloadItem? {
        model.downloads.first { $0.state == .downloading || $0.state == .processing }
    }

    var body: some View {
        if let item = firstActive {
            Button {
                model.selectedTab = .downloads
            } label: {
                HStack(spacing: 10) {
                    AppArtwork(url: item.app.artworkURL, size: 30)
                    VStack(alignment: .leading, spacing: 1) {
                        Text(item.app.name)
                            .font(.system(size: 13, weight: .semibold))
                            .foregroundStyle(.primary)
                            .lineLimit(1)
                        Text(model.runningDownloadCount > 1
                             ? String(localized: "Downloading \(model.runningDownloadCount) apps…")
                             : item.progressLabel)
                            .font(.system(size: 11))
                            .foregroundStyle(.secondary)
                            .lineLimit(1)
                    }
                    Spacer(minLength: 8)
                    CircularProgressRing(
                        fraction: item.state == .processing ? 1 : item.fraction,
                        size: 22, lineWidth: 2, showsStop: false)
                }
                .padding(.horizontal, 12)
                .padding(.vertical, 6)
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .accessibilityLabel(String(localized: "Show downloads"))
        }
    }
}
