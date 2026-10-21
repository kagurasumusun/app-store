// SPDX-License-Identifier: MIT

import AVFoundation
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
    var manifestURL: URL?
    var pingURL: URL?
    var pageURL: URL?
    /// Recent server activity (connections, served paths, TLS errors,
    /// self-test results), newest last, capped so the sheet can show it
    /// as a footnote. When installation fails this is what separates
    /// "the device never connected" from "TLS broke" from "iOS fetched
    /// the manifest fine and failed later".
    var diagnosticLines: [String] = []
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
    @ObservationIgnored private let keepAlive = InstallKeepAlive()

    var certificateFingerprintShort: String {
        guard certificateFingerprint.count > 23 else { return certificateFingerprint }
        return String(certificateFingerprint.prefix(23)) + "…"
    }

    var hasCertificate: Bool { !certificateFingerprint.isEmpty }

    // MARK: Installation

    func start(item: LibraryItem) {
        if server != nil, self.item?.id == item.id {
            // Re-presenting the sheet for the same package: the server
            // and any transfer in flight stay exactly as they are.
            return
        }
        stop()
        self.item = item
        phase = .preparing
        message = "Preparing the local install server…"

        diagnosticLines = []
        let server = makeServer(for: item)
        self.server = server
        server.stateHandler = { [weak self] state, progress, message in
            Task { @MainActor in
                self?.apply(state: state, progress: progress, message: message)
            }
        }
        server.activityHandler = { [weak self] line in
            Task { @MainActor in self?.noteDiagnostic(line) }
        }
        guard server.start() else {
            phase = .failed(server.startupError.map { StoreError($0).message }
                ?? "The install server did not start.")
            return
        }
        installURL = server.installURL.flatMap(URL.init(string:))
        manifestURL = server.manifestURL.flatMap(URL.init(string:))
        pingURL = server.pingURL.flatMap(URL.init(string:))
        pageURL = server.pageURL.flatMap(URL.init(string:))
        profileURL = server.profileURL.flatMap(URL.init(string:))
        certificateFingerprint = server.certificate?.sha256Fingerprint ?? ""
        if let pingURL {
            noteDiagnostic("Listening at \(pingURL.absoluteString)")
        }
        if let email = item.accountEmail {
            // The FairPlay authorization inside the package belongs to
            // this account; iOS rejects the install when the device's
            // App Store (Media & Purchases) sign-in is a different one.
            noteDiagnostic("Licensed to \(email)")
        }
        phase = .ready
        message = "Waiting for the device"
        // The transfer happens while the user is on the Home Screen
        // watching the icon fill in; hold the process open for it.
        keepAlive.start()
        noteDiagnostic("Keeping the app awake while the server runs")
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
        case .sent:
            phase = .sent
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
                noteDiagnostic("Self-test: /ping OK (TLS trusted)")
                // Then fetch the manifest itself — the exact resource the
                // system fetcher needs next. /ping proves the listener
                // and trust; this additionally proves routing and the
                // manifest body, so opening the link can only still fail
                // for reasons specific to that fetcher.
                if let manifestURL {
                    var manifestRequest = URLRequest(url: manifestURL)
                    manifestRequest.timeoutInterval = 4
                    manifestRequest.cachePolicy = .reloadIgnoringLocalCacheData
                    let (data, manifestResponse) =
                        try await URLSession.shared.data(for: manifestRequest)
                    guard let manifestHTTP = manifestResponse as? HTTPURLResponse,
                          manifestHTTP.statusCode == 200, !data.isEmpty else {
                        let code = (manifestResponse as? HTTPURLResponse)?.statusCode ?? -1
                        noteDiagnostic("Self-test: manifest check failed (HTTP \(code))")
                        phase = .failed(String(localized: "The install manifest could not be loaded from the local server (HTTP \(code))."))
                        return
                    }
                    noteDiagnostic("Self-test: manifest OK (\(data.count) bytes)")
                }
                noteDiagnostic("Opening the system install prompt")
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
                noteDiagnostic("Self-test failed: \(error.localizedDescription)")
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
        keepAlive.stop()
        server?.stop()
        server = nil
        if case .failed = phase { return }
        if phase != .idle {
            phase = .stopped
        }
        installURL = nil
        manifestURL = nil
        pingURL = nil
        pageURL = nil
        profileURL = nil
    }

    /// Appends one diagnostic line, timestamped, keeping the most recent
    /// handful. The sheet renders these verbatim — they are a log, not UI
    /// copy, so they stay in the server's own wording.
    private func noteDiagnostic(_ line: String) {
        let time = Date().formatted(date: .omitted, time: .standard)
        diagnosticLines.append("\(time)  \(line)")
        if diagnosticLines.count > 10 {
            diagnosticLines.removeFirst(diagnosticLines.count - 10)
        }
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
            Task { @MainActor in self?.renewBackgroundAssertion() }
        }
        backgroundTask = identifier == .invalid ? .invalid : identifier
    }

    /// The system commit is short and the user may still be reading the
    /// install prompt when it lapses. End the expiring task and ask for
    /// another window while a server is up; if the system has no more
    /// time to give, the new task expires immediately and nothing is
    /// worse than before.
    private func renewBackgroundAssertion() {
        endBackgroundAssertion()
        beginBackgroundAssertion()
    }

    func endBackgroundAssertion() {
        guard backgroundTask != .invalid else { return }
        UIApplication.shared.endBackgroundTask(backgroundTask)
        backgroundTask = .invalid
    }
}

/// Keeps the process runnable while iOS downloads the package from the
/// local server.
///
/// Once the user confirms the system prompt they typically watch the app
/// icon fill in on the Home Screen, so this app is in the background for
/// most of the transfer. A background-task assertion alone is a short,
/// system-sized window that a large package can outlast; inaudible
/// looping audio in a mixing playback session holds execution for as
/// long as the server is up, without disturbing other apps' audio.
private final class InstallKeepAlive {
    private var player: AVAudioPlayer?

    func start() {
        guard player == nil else { return }
        do {
            let session = AVAudioSession.sharedInstance()
            try session.setCategory(.playback, mode: .default,
                                    options: [.mixWithOthers])
            try session.setActive(true)
            let player = try AVAudioPlayer(data: Self.silence)
            player.numberOfLoops = -1
            player.volume = 0
            player.play()
            self.player = player
        } catch {
            // Best effort — the background-task assertion still applies.
        }
    }

    func stop() {
        player?.stop()
        player = nil
        try? AVAudioSession.sharedInstance().setActive(
            false, options: [.notifyOthersOnDeactivation])
    }

    /// One second of silent PCM in a minimal WAV container, built in
    /// memory so the app ships no audio asset for this.
    private static let silence: Data = {
        let sampleRate = 8000
        let byteCount = sampleRate * 2  // mono, 16-bit samples
        var data = Data()
        func appendLE<T: FixedWidthInteger>(_ value: T) {
            var little = value.littleEndian
            withUnsafeBytes(of: &little) { data.append(contentsOf: $0) }
        }
        data.append(contentsOf: [UInt8]("RIFF".utf8))
        appendLE(UInt32(36 + byteCount))
        data.append(contentsOf: [UInt8]("WAVE".utf8))
        data.append(contentsOf: [UInt8]("fmt ".utf8))
        appendLE(UInt32(16))              // fmt chunk size
        appendLE(UInt16(1))               // PCM
        appendLE(UInt16(1))               // channels
        appendLE(UInt32(sampleRate))
        appendLE(UInt32(sampleRate * 2))  // byte rate
        appendLE(UInt16(2))               // block align
        appendLE(UInt16(16))              // bits per sample
        data.append(contentsOf: [UInt8]("data".utf8))
        appendLE(UInt32(byteCount))
        data.append(Data(count: byteCount))
        return data
    }()
}
