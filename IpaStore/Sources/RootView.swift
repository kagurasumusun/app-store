// SPDX-License-Identifier: MIT

import SwiftUI

struct RootView: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        @Bindable var model = model

        TabView(selection: $model.selectedTab) {
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
            Tab(AppTab.accounts.title, systemImage: AppTab.accounts.systemImage, value: AppTab.accounts) {
                AccountsView()
            }
            Tab(AppTab.settings.title, systemImage: AppTab.settings.systemImage, value: AppTab.settings) {
                SettingsView()
            }
        }
        .sheet(isPresented: Binding(
            get: { model.installService.isPresented },
            set: { model.installService.isPresented = $0 })) {
            InstallSheet()
        }
        .sheet(isPresented: $model.showLogin) {
            LoginView()
        }
        .alert(model.alert?.title ?? "Error",
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
