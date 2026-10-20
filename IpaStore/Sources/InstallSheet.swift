// SPDX-License-Identifier: MIT

import SwiftUI
import UIKit

/// One-screen installation status: the local server serves the manifest and
/// payload on loopback; iOS does the actual installing after the user
/// confirms its own prompt.
struct InstallSheet: View {
    @Environment(AppModel.self) private var model
    @Environment(\.dismiss) private var dismiss

    private var service: InstallService { model.installService }

    var body: some View {
        NavigationStack {
            ZStack {
                AppBackground()
                VStack(spacing: 24) {
                    Spacer()
                    Image(systemName: phaseSymbol)
                        .font(.system(size: 56, weight: .light))
                        .foregroundStyle(phaseColor)
                        .contentTransition(.symbolEffect(.replace))
                    VStack(spacing: 8) {
                        if let item = service.item {
                            Text(item.name)
                                .font(.title3.weight(.bold))
                                .multilineTextAlignment(.center)
                            Text("Version \(item.displayVersion) · \(item.sizeLabel)")
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        }
                        Text(statusText)
                            .font(.headline)
                            .multilineTextAlignment(.center)
                        if case .sending(let progress) = service.phase {
                            ProgressView(value: progress)
                                .frame(maxWidth: 260)
                        } else if service.phase == .preparing {
                            ProgressView()
                        }
                        if case .failed(let message) = service.phase {
                            Text(message)
                                .font(.caption)
                                .foregroundStyle(.red)
                                .multilineTextAlignment(.center)
                        }
                    }

                    if service.phase == .ready || isSending {
                        Button {
                            service.openInstallURL()
                        } label: {
                            Label(isSending ? "Open the Install Prompt Again" : "Install",
                                  systemImage: "arrow.down.app")
                                .frame(minWidth: 200)
                        }
                        .buttonStyle(.borderedProminent)
                        .controlSize(.large)
                    }

                    Spacer()

                    certificateHelp

                    Text("Keep IPAShelf open until iOS finishes downloading the package. The app icon appears on the Home Screen while it installs.")
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                        .multilineTextAlignment(.center)
                        .padding(.horizontal, 24)
                }
                .padding(.bottom, 20)
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
            }
            .task(id: service.phase == .ready) {
                // Kick off the system install prompt automatically when the
                // server is ready; the button above re-opens it on demand.
                if service.phase == .ready {
                    try? await Task.sleep(for: .milliseconds(350))
                    service.openInstallURL()
                }
            }
        }
        .presentationDetents([.large])
        .onDisappear { service.stop() }
    }

    private var isSending: Bool {
        if case .sending = service.phase { return true }
        return false
    }

    private var statusText: String {
        switch service.phase {
        case .preparing:
            return "Preparing…"
        case .ready:
            return "Ready to Install"
        case .sending(let progress) where progress >= 1:
            return "Sent — iOS is installing"
        case .sending:
            return "Sending to iOS…"
        case .failed:
            return "Installation Failed"
        case .idle, .stopped:
            return "Not Running"
        }
    }

    private var phaseSymbol: String {
        switch service.phase {
        case .preparing: return "gearshape.2"
        case .ready: return "app.gift"
        case .sending(let progress) where progress >= 1: return "app.badge.checkmark"
        case .sending: return "paperplane.fill"
        case .failed: return "exclamationmark.triangle.fill"
        case .idle, .stopped: return "app.dashed"
        }
    }

    private var phaseColor: Color {
        switch service.phase {
        case .failed: return .red
        case .idle, .stopped: return .secondary
        case .sending(let progress) where progress >= 1: return .green
        default: return .indigo
        }
    }

    private var certificateHelp: some View {
        VStack(spacing: 10) {
            Text("Nothing happens when you tap Install?")
                .font(.subheadline.weight(.semibold))
            Text("The root certificate has to be installed and trusted once: download the profile, install it under Settings › General › VPN & Device Management, then enable “IPAShelf Root CA” under Settings › General › About › Certificate Trust Settings.")
                .font(.caption)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
            Button {
                if !service.openCertificateProfile(),
                   let error = service.certificateError {
                    model.presentMessage("Certificate", error)
                }
            } label: {
                Label("Install Root Certificate", systemImage: "checkmark.shield")
            }
            .buttonStyle(.bordered)
            if service.hasCertificate {
                CopyableIdentifier(value: service.certificateFingerprintShort)
            }
        }
        .padding(16)
        .frame(maxWidth: .infinity)
        .background(
            RoundedRectangle(cornerRadius: 16, style: .continuous)
                .fill(Color(.secondarySystemGroupedBackground).opacity(0.75)))
        .padding(.horizontal, 20)
    }
}
