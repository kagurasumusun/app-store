// SPDX-License-Identifier: MIT

import Foundation
import Observation
import UIKit

/// Drives the on-demand install server for one downloaded IPA.
///
/// The server itself lives in `IPInstallServer` (Objective-C++): it creates a
/// local root CA, serves the OTA manifest and the payload over TLS, and serves
/// the root certificate over plain HTTP so the user can trust it before the
/// first install.
@MainActor
@Observable
final class InstallService {
    var phase: InstallPhase = .idle
    var message: String = ""
    var installURL: URL?
    var pageURL: URL?
    var certificateURL: URL?
    var certificateFingerprint: String = ""
    var isPresented = false
    var item: LibraryItem?

    @ObservationIgnored private var server: IPInstallServer?
    @ObservationIgnored private var backgroundTask: UIBackgroundTaskIdentifier = .invalid

    var certificateFingerprintShort: String {
        guard certificateFingerprint.count > 23 else { return certificateFingerprint }
        return String(certificateFingerprint.prefix(23)) + "…"
    }

    func start(item: LibraryItem) {
        stop()
        self.item = item
        phase = .preparing
        message = "Creating the local certificate…"

        let server = makeServer(for: item)
        self.server = server
        server.stateHandler = { [weak self] state, progress, message in
            Task { @MainActor in
                self?.apply(state: state, progress: progress, message: message)
            }
        }
        guard server.start() else {
            phase = .failed(server.startupError.map { StoreError($0).message }
                ?? "The install server did not start.")
            return
        }
        installURL = server.installURL.flatMap(URL.init(string:))
        pageURL = server.pageURL.flatMap(URL.init(string:))
        certificateURL = server.certificateURL.flatMap(URL.init(string:))
        certificateFingerprint = server.certificate?.sha256Fingerprint ?? ""
        phase = .ready
        message = "Waiting for the device"
    }

    private func makeServer(for item: LibraryItem) -> IPInstallServer {
        // The manifest's icon endpoints are optional; using the packaged
        // install icon keeps the system banner recognisable.
        let icon = Bundle.main.path(forResource: "InstallIcon", ofType: "png")
        return IPInstallServer(bundleID: item.bundleID,
                               version: item.displayVersion,
                               title: item.name,
                               ipaAtPath: item.fileURL.path,
                               artworkPath: icon)
    }

    private func apply(state: IPInstallServerState, progress: Double, message: String) {
        switch state {
        case .idle:
            phase = .idle
        case .starting:
            phase = .preparing
        case .waiting:
            phase = .ready
        case .sending:
            phase = .sending(progress)
        case .failed:
            phase = .failed(message.isEmpty ? "The install server stopped." : message)
        case .stopped:
            phase = .stopped
        @unknown default:
            phase = .failed("Unknown install state.")
        }
        if !message.isEmpty {
            self.message = message
        }
    }

    /// Keeps the process (and therefore the listener) alive briefly after the
    /// user leaves the app to accept the install prompt.
    func beginBackgroundAssertion() {
        guard server != nil, backgroundTask == .invalid else { return }
        let identifier = UIApplication.shared.beginBackgroundTask(withName: "IPAShelf.Install") { [weak self] in
            Task { @MainActor in self?.endBackgroundAssertion() }
        }
        backgroundTask = identifier == .invalid ? .invalid : identifier
    }

    func endBackgroundAssertion() {
        guard backgroundTask != .invalid else { return }
        UIApplication.shared.endBackgroundTask(backgroundTask)
        backgroundTask = .invalid
    }

    func stop() {
        endBackgroundAssertion()
        server?.stop()
        server = nil
        if case .failed = phase { return }
        if phase != .idle {
            phase = .stopped
        }
        installURL = nil
        pageURL = nil
        certificateURL = nil
    }

    /// Loads (or creates) the root certificate so Settings can show the
    /// fingerprint and instructions before the first install.
    func prepareCertificate() {
        let result = IPInstallServer.prepareCertificate()
        if let certificate = result.certificate {
            certificateFingerprint = certificate.sha256Fingerprint
            return
        }
        if let error = result.error {
            phase = .failed(StoreError(error).message)
        }
    }
}
