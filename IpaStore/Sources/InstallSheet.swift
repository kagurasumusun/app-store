// SPDX-License-Identifier: MIT

import SwiftUI
import UIKit

/// Installation progress, certificate trust instructions and diagnostics for
/// the local OTA server.
struct InstallSheet: View {
    @Environment(AppModel.self) private var model
    @Environment(\.dismiss) private var dismiss

    private var service: InstallService { model.installService }

    var body: some View {
        NavigationStack {
            List {
                statusSection
                if service.installURL != nil {
                    installSection
                } else {
                    packageSection
                }
                certificateSection
                diagnosticsSection
            }
            .navigationTitle("Install")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Close") {
                        service.stop()
                        dismiss()
                    }
                }
                ToolbarItem(placement: .confirmationAction) {
                    if service.phase.isBusy {
                        Button("Stop", role: .destructive) { service.stop() }
                    }
                }
            }
        }
        .presentationDetents([.medium, .large])
        .interactiveDismissDisabled(service.phase.isBusy)
        .onDisappear { service.stop() }
    }

    private var statusSection: some View {
        Section {
            if let item = service.item {
                HStack(spacing: 12) {
                    AppArtwork(url: item.artworkURL, size: 48, cornerRadius: 12)
                    VStack(alignment: .leading, spacing: 3) {
                        Text(item.name)
                            .font(.headline)
                        Text("Version \(item.displayVersion) · \(item.sizeLabel)")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                }
                .padding(.vertical, 2)
            }

            HStack {
                Label(service.phase.title, systemImage: phaseSymbol)
                    .foregroundStyle(phaseColor)
                Spacer()
                if case .sending(let progress) = service.phase {
                    ProgressView(value: progress)
                        .frame(width: 96)
                } else if case .preparing = service.phase {
                    ProgressView().controlSize(.small)
                }
            }

            if !service.message.isEmpty {
                Text(service.message)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            if case .failed(let message) = service.phase {
                Text(message)
                    .font(.caption)
                    .foregroundStyle(.red)
            }
        } header: {
            Text("Status")
        } footer: {
            Text("Keep IPAShelf open while iOS downloads and installs the package. iOS shows its own confirmation when the manifest is accepted.")
        }
    }

    @ViewBuilder
    private var installSection: some View {
        Section {
            Button {
                if let url = service.installURL {
                    UIApplication.shared.open(url)
                }
            } label: {
                Label("Install on this device", systemImage: "square.and.arrow.down.on.square")
                    .font(.headline)
            }
            if let pageURL = service.pageURL {
                Button {
                    UIApplication.shared.open(pageURL)
                } label: {
                    Label("Open the certificate page in Safari", systemImage: "safari")
                }
            }
        } header: {
            Text("Install")
        } footer: {
            Text("If iOS refuses the manifest, open the certificate page, install the root certificate, then enable it under Settings › General › About › Certificate Trust Settings.")
        }
    }

    private var packageSection: some View {
        Section {
            Text("No package is queued yet.")
                .font(.subheadline)
            Button {
                service.isPresented = false
                model.selectedTab = .library
            } label: {
                Label("Choose a package in Library", systemImage: "square.stack.3d.up.fill")
            }
        } header: {
            Text("Package")
        } footer: {
            Text("Downloads appear in the Library. Open one, tap Install, and the local server starts for that exact build.")
        }
    }

    private var certificateSection: some View {
        Section {
            HStack {
                Text("SHA-256")
                    .foregroundStyle(.secondary)
                Spacer()
                if service.certificateFingerprint.isEmpty {
                    Text("Not created")
                        .foregroundStyle(.tertiary)
                } else {
                    CopyableIdentifier(value: service.certificateFingerprint)
                }
            }
            .font(.subheadline)

            Button {
                service.prepareCertificate()
            } label: {
                Label("Create or refresh certificate", systemImage: "checkmark.shield")
            }

            VStack(alignment: .leading, spacing: 8) {
                Text("How to trust the certificate")
                    .font(.subheadline.weight(.semibold))
                Text("1. Tap “Open the certificate page in Safari”.")
                Text("2. Allow the profile download, then install it from Settings › General › VPN & Device Management.")
                Text("3. Open Settings › General › About › Certificate Trust Settings and switch on “IPAShelf Root CA”.")
                Text("4. Return here and tap Install.")
            }
            .font(.caption)
            .foregroundStyle(.secondary)

            if let certificateURL = service.certificateURL {
                ShareLink(item: certificateURL) {
                    Label("Share the certificate URL", systemImage: "square.and.arrow.up")
                }
            }
        } header: {
            Text("Certificate")
        } footer: {
            Text("The root certificate and both private keys are generated on this device and stored in the Keychain. Nothing is uploaded. Delete the profile in Settings when you no longer need it.")
        }
    }

    private var diagnosticsSection: some View {
        Section("Diagnostics") {
            if let installURL = service.installURL {
                InfoRow(label: "Install", value: installURL.absoluteString)
                    .font(.caption2)
            }
            if let pageURL = service.pageURL {
                InfoRow(label: "Page", value: pageURL.absoluteString)
                    .font(.caption2)
            }
            InfoRow(label: "Device ID", value: model.deviceIdentifier)
        }
    }

    private var phaseSymbol: String {
        switch service.phase {
        case .idle: return "power"
        case .preparing: return "gearshape.2"
        case .ready: return "checkmark.seal"
        case .sending: return "arrow.up.circle"
        case .failed: return "exclamationmark.triangle"
        case .stopped: return "stop.circle"
        }
    }

    private var phaseColor: Color {
        switch service.phase {
        case .failed: return .red
        case .stopped, .idle: return .secondary
        default: return .indigo
        }
    }
}
