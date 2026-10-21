// SPDX-License-Identifier: MIT

import Foundation
import Observation
import UIKit

/// Drives on-device OTA installation and the root-certificate profile
/// server.
///
/// The servers themselves live in Objective-C++ (`IPInstallServer` and
/// `IPInstallCertificateServer`): a locally generated root CA signs a leaf
/// for loopback, the manifest and IPA are served over TLS on 127.0.0.1,
/// and the certificate profile is served over plain HTTP so it can be
/// fetched before the CA is trusted.
@MainActor
@Observable
final class InstallService {
    var phase: InstallPhase = .idle
    var message: String = ""
    var installURL: URL?
    var pingURL: URL?
    var pageURL: URL?
    var profileURL: URL?
    var certificateFingerprint: String = ""
    var certificateError: String?
    /// Whether the device currently trusts the root certificate. `nil`
    /// until first checked; refreshed whenever Settings appears or the
    /// certificate is (re)generated.
    var rootCertificateTrusted: Bool?
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
        pingURL = server.pingURL.flatMap(URL.init(string:))
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
    ///
    /// The link is only opened after the app has fetched `/ping` from its
    /// own HTTPS listener using the system trust store. That one request
    /// proves end-to-end what `installd` will do seconds later — listener
    /// reachable, TLS identity sane, root certificate trusted — and turns
    /// the otherwise silent no-op (an untrusted certificate makes iOS
    /// ignore the link without any message) into a state the sheet can
    /// explain and guide.
    func openInstallURL() {
        guard let installURL else { return }
        // Begun here, in the foreground: installd fetches from this process
        // while the user is looking at the system prompt.
        beginBackgroundAssertion()
        guard let pingURL else {
            UIApplication.shared.open(installURL)
            return
        }
        Task { [weak self] in
            guard let self else { return }
            do {
                var request = URLRequest(url: pingURL)
                request.timeoutInterval = 4
                request.cachePolicy = .reloadIgnoringLocalCacheData
                let (_, response) = try await URLSession.shared.data(for: request)
                guard let http = response as? HTTPURLResponse,
                      http.statusCode == 200 else {
                    phase = .failed(String(localized: "The install server answered with an unexpected response."))
                    return
                }
                // Explicit completion-handler form: in an async context
                // the iOS 27 SDK otherwise resolves `open` to its async
                // overload and rejects the un-awaited call.
                UIApplication.shared.open(installURL, options: [:],
                                          completionHandler: nil)
            } catch let error as URLError where Self.isCertificateError(error) {
                // The static trust check passed but TLS disagrees (the
                // trust toggle was just turned off, or the leaf no longer
                // chains to the installed root): show the certificate
                // setup steps again instead of opening a link iOS would
                // silently ignore.
                rootCertificateTrusted = false
            } catch {
                phase = .failed(String(localized: "The install server did not respond on this device: \(error.localizedDescription)"))
            }
        }
    }

    private static func isCertificateError(_ error: URLError) -> Bool {
        switch error.code {
        case .serverCertificateUntrusted, .serverCertificateHasBadDate,
             .serverCertificateHasUnknownRoot, .serverCertificateNotYetValid,
             .secureConnectionFailed:
            return true
        default:
            return false
        }
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
        pingURL = nil
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
            rootCertificateTrusted = IPInstallServer.rootCertificateIsTrusted(certificate)
            certificateError = nil
            return
        }
        if let error = result.error {
            certificateError = StoreError(error).message
        }
    }

    /// Re-evaluates whether the device trusts the root certificate. Cheap
    /// enough to call whenever Settings or the install sheet appears, so
    /// the user sees the state flip right after flipping the iOS toggle.
    func refreshTrustStatus() {
        guard !certificateFingerprint.isEmpty else { return }
        let result = IPInstallServer.prepareCertificate()
        if let certificate = result.certificate {
            certificateFingerprint = certificate.sha256Fingerprint
            rootCertificateTrusted = IPInstallServer.rootCertificateIsTrusted(certificate)
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
            // Safari fetches the profile from this process after we hand
            // over. Without the assertion the app is suspended seconds
            // later and the fetch never completes — Safari reports it
            // cannot connect. Begun before opening, in the foreground.
            beginBackgroundAssertion()
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
        beginBackgroundAssertion()
        UIApplication.shared.open(url)
        return true
    }

    func stopCertificateServer() {
        certificateServer?.stop()
        certificateServer = nil
        if server == nil {
            endBackgroundAssertion()
        }
    }

    // MARK: Background assertion

    /// Keeps the process (and therefore the listener) alive briefly after the
    /// user leaves the app to accept the install prompt or fetch the
    /// certificate profile in Safari.
    func beginBackgroundAssertion() {
        guard server != nil || certificateServer != nil,
              backgroundTask == .invalid else { return }
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
