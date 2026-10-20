// SPDX-License-Identifier: MIT

import SwiftUI

@main
struct IpaStoreApp: App {
    @State private var model = AppModel()
    @Environment(\.scenePhase) private var scenePhase

    var body: some Scene {
        WindowGroup {
            RootView()
                .environment(model)
                .tint(.indigo)
                .onOpenURL { url in
                    // "Open in IPAShelf" from Files / share sheets.
                    if url.pathExtension.lowercased() == "ipa" {
                        model.importIPA(from: url)
                    }
                }
                .task {
                    // Reuse the persisted root CA so the fingerprint shown in
                    // Settings matches the one already trusted on the device.
                    model.installService.prepareCertificate()
                }
        }
        .onChange(of: scenePhase) { _, phase in
            // The install listener has to survive the switch to SpringBoard
            // while iOS downloads the payload, so hold a background assertion
            // for as long as the system allows.
            switch phase {
            case .background:
                model.installService.beginBackgroundAssertion()
            case .active:
                model.installService.endBackgroundAssertion()
            default:
                break
            }
        }
    }
}
