// SPDX-License-Identifier: MIT

import SwiftUI
import UIKit

/// One-screen installation status: the local server serves the manifest and
/// payload on loopback; iOS does the actual installing after the user
/// confirms its own prompt.
struct InstallSheet: View {
    @Environment(AppModel.self) private var model
    @Environment(ThemeStore.self) private var theme
    @Environment(\.dismiss) private var dismiss
    @Environment(\.scenePhase) private var scenePhase

    private var service: InstallService { model.installService }

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(spacing: 24) {
                    header
                    statusBlock
                    stepsCard
                    primaryAction
                    Text("Keep IPAShelf open until iOS finishes downloading the package. The app icon appears on the Home Screen while it installs.")
                        .font(.system(size: 12))
                        .foregroundStyle(.secondary)
                        .multilineTextAlignment(.center)
                        .padding(.horizontal, 12)
                    diagnosticsFootnote
                }
                .padding(.horizontal, 20)
                .padding(.vertical, 20)
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
            .task(id: service.phase == .ready && service.rootCertificateTrusted == true) {
                // Kick off the system install prompt automatically once the
                // server is ready AND the root certificate is trusted;
                // opening it earlier just fails silently. When the user
                // comes back from enabling trust in Settings, the id flips
                // and the prompt fires on its own.
                if service.phase == .ready, service.rootCertificateTrusted == true {
                    try? await Task.sleep(for: .milliseconds(350))
                    service.openInstallURL()
                }
            }
            .onAppear { service.refreshTrustStatus() }
            .onChange(of: scenePhase) { _, phase in
                if phase == .active {
                    service.refreshTrustStatus()
                }
            }
        }
        .presentationDetents([.large])
        .onDisappear { service.stop() }
        .sensoryFeedback(.success, trigger: sentFully)
    }

    // MARK: Header and status

    private var header: some View {
        VStack(spacing: 10) {
            if let item = service.item {
                AppArtwork(url: item.artworkURL, size: 84)
                Text(item.name)
                    .font(.system(size: 20, weight: .bold))
                    .multilineTextAlignment(.center)
                    .lineLimit(2)
                Text(String(localized: "Version \(item.displayVersion) · \(item.sizeLabel)"))
                    .font(.system(size: 12))
                    .foregroundStyle(.secondary)
            }
        }
    }

    private var statusBlock: some View {
        VStack(spacing: 12) {
            if case .sending(let progress) = service.phase {
                ZStack {
                    CircularProgressRing(fraction: progress, size: 76,
                                         lineWidth: 6, showsStop: false)
                    Text("\(Int(progress * 100))%")
                        .font(.system(size: 17, weight: .bold, design: .rounded))
                }
            } else {
                Image(systemName: phaseSymbol)
                    .font(.system(size: 30, weight: .medium))
                    .foregroundStyle(phaseColor)
                    .frame(width: 76, height: 76)
                    .background {
                        Circle().fill(phaseColor.opacity(0.12))
                    }
                    .contentTransition(.symbolEffect(.replace))
            }
            Text(statusText)
                .font(.system(size: 17, weight: .semibold))
                .multilineTextAlignment(.center)
            if case .failed(let message) = service.phase {
                Text(message)
                    .font(.system(size: 12))
                    .foregroundStyle(.red)
                    .multilineTextAlignment(.center)
            }
        }
        .animation(.default, value: service.phase)
    }

    // MARK: Steps

    private enum StepState {
        case done, active, pending, actionNeeded
    }

    private var stepsCard: some View {
        VStack(spacing: 0) {
            stepRow(state: serverReady ? .done : (service.phase == .preparing ? .active : .pending),
                    title: String(localized: "Start the local install server"),
                    subtitle: nil)
            Divider().padding(.leading, 44)
            stepRow(state: trusted ? .done : .actionNeeded,
                    title: String(localized: "Trust the root certificate"),
                    subtitle: trusted
                        ? String(localized: "Trusted on this device")
                        : String(localized: "One-time setup in Settings"))
            if !trusted {
                certificateHelp
                    .padding(.leading, 44)
                    .padding(.bottom, 14)
            }
            Divider().padding(.leading, 44)
            stepRow(state: sentFully ? .done : (serverReady && trusted ? .active : .pending),
                    title: String(localized: "Confirm the system install prompt"),
                    subtitle: nil)
            Divider().padding(.leading, 44)
            stepRow(state: sentFully ? .done : .pending,
                    title: String(localized: "iOS downloads and installs the app"),
                    subtitle: sentFully ? String(localized: "Check the Home Screen") : nil)
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 6)
        .background {
            RoundedRectangle(cornerRadius: 16, style: .continuous)
                .fill(Color(.secondarySystemBackground))
        }
        .animation(.default, value: trusted)
    }

    private func stepRow(state: StepState, title: String, subtitle: String?) -> some View {
        HStack(spacing: 12) {
            Group {
                switch state {
                case .done:
                    Image(systemName: "checkmark.circle.fill")
                        .foregroundStyle(.green)
                case .active:
                    ProgressView().controlSize(.small)
                case .actionNeeded:
                    Image(systemName: "exclamationmark.circle.fill")
                        .foregroundStyle(.orange)
                case .pending:
                    Image(systemName: "circle")
                        .foregroundStyle(.tertiary)
                }
            }
            .font(.system(size: 22))
            .frame(width: 28, height: 28)
            VStack(alignment: .leading, spacing: 2) {
                Text(title)
                    .font(.system(size: 15, weight: .medium))
                    .foregroundStyle(state == .pending ? Color.secondary : Color.primary)
                if let subtitle {
                    Text(subtitle)
                        .font(.system(size: 12))
                        .foregroundStyle(.secondary)
                }
            }
            Spacer()
        }
        .padding(.vertical, 10)
    }

    private var certificateHelp: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("Install does nothing until iOS trusts the local certificate: download the profile, install it under Settings › General › VPN & Device Management, then enable “IPAShelf Root CA” under Settings › General › About › Certificate Trust Settings. When you come back here, the install prompt opens automatically.")
                .font(.system(size: 12))
                .foregroundStyle(.secondary)
            HStack(spacing: 10) {
                Button {
                    if !service.openCertificateProfile(),
                       let error = service.certificateError {
                        model.presentMessage(String(localized: "Certificate"), error)
                    }
                } label: {
                    Text("Install Root Certificate")
                        .font(.system(size: 13, weight: .semibold))
                }
                .buttonStyle(.borderedProminent)
                .buttonBorderShape(.capsule)
                .controlSize(.small)
                Button {
                    if let url = URL(string: UIApplication.openSettingsURLString) {
                        UIApplication.shared.open(url)
                    }
                } label: {
                    Text("Open Settings")
                        .font(.system(size: 13, weight: .semibold))
                }
                .buttonStyle(.bordered)
                .buttonBorderShape(.capsule)
                .controlSize(.small)
            }
            if service.hasCertificate {
                CopyableIdentifier(value: service.certificateFingerprintShort)
            }
        }
    }

    // MARK: Diagnostics

    /// The server's recent activity, verbatim. iOS reports every
    /// installation failure as the same opaque alert, so this is the one
    /// place that shows what actually happened on the wire.
    @ViewBuilder
    private var diagnosticsFootnote: some View {
        if !service.diagnosticLines.isEmpty {
            VStack(alignment: .leading, spacing: 6) {
                Text("Server Activity")
                    .font(.system(size: 11, weight: .semibold))
                    .foregroundStyle(.secondary)
                    .textCase(.uppercase)
                ForEach(Array(service.diagnosticLines.enumerated()), id: \.offset) { _, line in
                    Text(line)
                        .font(.system(size: 11, design: .monospaced))
                        .foregroundStyle(.secondary)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
            }
            .padding(.horizontal, 14)
            .padding(.vertical, 12)
            .background {
                RoundedRectangle(cornerRadius: 14, style: .continuous)
                    .fill(Color(.tertiarySystemBackground))
            }
        }
    }

    // MARK: Primary action

    @ViewBuilder
    private var primaryAction: some View {
        if service.phase == .ready || isSending {
            VStack(spacing: 12) {
                Button {
                    service.openInstallURL()
                } label: {
                    Label(isSending
                          ? String(localized: "Open the Install Prompt Again")
                          : String(localized: "Install"),
                          systemImage: "arrow.down.app")
                        .font(.system(size: 16, weight: .semibold))
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.borderedProminent)
                .buttonBorderShape(.capsule)
                .controlSize(.large)
                if let pageURL = service.pageURL {
                    // The classic path: the same server hosts a small web
                    // page whose Install link Safari hands to iOS. Kept as
                    // a fallback for when opening the prompt from inside
                    // the app does not take.
                    Button {
                        service.beginBackgroundAssertion()
                        UIApplication.shared.open(pageURL)
                    } label: {
                        Label(String(localized: "Open the Install Page Instead"),
                              systemImage: "safari")
                            .font(.system(size: 14, weight: .semibold))
                            .frame(maxWidth: .infinity)
                    }
                    .buttonStyle(.bordered)
                    .buttonBorderShape(.capsule)
                    .controlSize(.large)
                    Text("If the system prompt does not appear, open the install page in Safari and tap Install there.")
                        .font(.system(size: 12))
                        .foregroundStyle(.secondary)
                        .multilineTextAlignment(.center)
                }
            }
        } else if case .failed = service.phase, let item = service.item {
            Button {
                model.install(item)
            } label: {
                Text("Try Again")
                    .font(.system(size: 16, weight: .semibold))
                    .frame(maxWidth: .infinity)
            }
            .buttonStyle(.borderedProminent)
            .buttonBorderShape(.capsule)
            .controlSize(.large)
        }
    }

    // MARK: State helpers

    private var isSending: Bool {
        if case .sending = service.phase { return true }
        return false
    }

    private var serverReady: Bool {
        service.phase == .ready || isSending
    }

    private var trusted: Bool {
        service.rootCertificateTrusted == true
    }

    private var sentFully: Bool {
        if case .sending(let progress) = service.phase { return progress >= 1 }
        return false
    }

    private var statusText: String {
        switch service.phase {
        case .preparing:
            return String(localized: "Preparing…")
        case .ready where !trusted:
            return String(localized: "One more step: trust the certificate")
        case .ready:
            return String(localized: "Ready to Install")
        case .sending(let progress) where progress >= 1:
            return String(localized: "Sent — iOS is installing")
        case .sending:
            return String(localized: "Sending to iOS…")
        case .failed:
            return String(localized: "Installation Failed")
        case .idle, .stopped:
            return String(localized: "Not Running")
        }
    }

    private var phaseSymbol: String {
        switch service.phase {
        case .preparing: return "gearshape.2"
        case .ready: return "arrow.down.app"
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
        default: return theme.accentColor
        }
    }
}
