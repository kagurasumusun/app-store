// SPDX-License-Identifier: MIT

import Foundation
import Observation
import UIKit

/// Drives on-device OTA installation and the root-certificate profile
/// server.
///
/// The servers themselves live in Objective-C++ (`IPInstallServer` and
/// `IPInstallCertificateServer`): a locally generated root CA signs a leaf
/// for `localhost`, the manifest and IPA are served over TLS on loopback,
/// and the certificate profile is served over plain HTTP so it can be
/// fetched before the CA is trusted.
@MainActor
@Observable
final class InstallService {
    var phase: InstallPhase = .idle
    var message: String = ""
    var installURL: URL?
    var pageURL: URL?
    var profileURL: URL?
    var certificateFingerprint: String = ""
    var certificateError: String?
    var isPresented = false
    var item: LibraryItem?

    @ObservationIgnored private var server: IPInstallServer?
    @ObservationIgnored private var certificateServer: IPInstallCertificateServer?
    @ObservationIgnored private var backgroundTask: UIBackgroundTaskIdentifier = .invalid

    var certificateFingerprintShort: String {
        guard certificateFingerprint.count > 23 else { return certificateFingerprint }
        return String(certificateFingerprint.prefix(23)) + "…"
    }

    var hasCertificate: Bool { !certificateFingerprint.isEmpty }

    // MARK: Installation

    func start(item: LibraryItem) {
        stop()
        self.item = item
        phase = .preparing
        message = "Preparing the local install server…"

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
        profileURL = server.profileURL.flatMap(URL.init(string:))
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

    /// Opens the `itms-services` link so iOS fetches the manifest and asks
    /// the user to confirm the installation.
    func openInstallURL() {
        guard let installURL else { return }
        UIApplication.shared.open(installURL)
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
        profileURL = nil
    }

    // MARK: Root certificate

    /// Loads (or creates) the root certificate so Settings can show the
    /// fingerprint before the first install.
    func prepareCertificate() {
        let result = IPInstallServer.prepareCertificate()
        if let certificate = result.certificate {
            certificateFingerprint = certificate.sha256Fingerprint
            certificateError = nil
            return
        }
        if let error = result.error {
            certificateError = StoreError(error).message
        }
    }

    /// Starts the certificate-only server and opens the profile in Safari,
    /// which starts the iOS profile download flow. Used from Settings and
    /// from the install sheet when installation is blocked.
    @discardableResult
    func openCertificateProfile() -> Bool {
        // When an install server is already running its own profile
        // endpoint is the right one; a second server would be redundant.
        if let profileURL {
            UIApplication.shared.open(profileURL)
            return true
        }
        let server = certificateServer ?? IPInstallCertificateServer()
        certificateServer = server
        guard server.start() else {
            certificateError = server.startupError.map { StoreError($0).message }
                ?? "The certificate server did not start."
            return false
        }
        if let certificate = server.certificate {
            certificateFingerprint = certificate.sha256Fingerprint
        }
        certificateError = nil
        guard let url = server.profileURL.flatMap(URL.init(string:)) else {
            certificateError = "The certificate server did not publish a URL."
            return false
        }
        UIApplication.shared.open(url)
        return true
    }

    func stopCertificateServer() {
        certificateServer?.stop()
        certificateServer = nil
    }

    // MARK: Background assertion

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
}
