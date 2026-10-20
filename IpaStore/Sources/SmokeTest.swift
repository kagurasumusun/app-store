// SPDX-License-Identifier: MIT
//
// In-app smoke test.
//
// The CI runner has no UI harness, so the app verifies itself: when it is
// launched with `-IPSmokeTest 1` it exercises the parts that only exist at
// runtime (Keychain, Security.framework, the generated CA, the Network
// listeners and the OTA router) and reports the result on stdout before
// quitting. A normal launch never touches this file.

import Foundation

enum SmokeTest {
    /// Path prefix used in the console output the CI script greps for.
    private static let tag = "SMOKE:"

    static func runIfRequested() {
        #if DEBUG
        guard ProcessInfo.processInfo.arguments.contains("-IPSmokeTest") else {
            return
        }
        run()
        #endif
    }

    #if DEBUG

    private static func run() -> Never {
        var failures = 0
        var checks = 0

        func check(_ name: String, _ condition: Bool, _ detail: String = "") {
            checks += 1
            if condition {
                print("\(tag) ok \(name)")
            } else {
                failures += 1
                let suffix = detail.isEmpty ? "" : " (\(detail))"
                print("\(tag) FAIL \(name)\(suffix)")
            }
        }

        print("\(tag) starting bundle=\(Bundle.main.bundleIdentifier ?? "?")")

        // 1. The root certificate is generated through x509.cpp and stored in
        //    the Keychain, so this covers the crypto and the keychain bridge.
        let prepared = IPInstallServer.prepareCertificate()
        if let certificate = prepared.certificate {
            check("certificate.der", certificate.der.count > 100,
                  "\(certificate.der.count) bytes")
            check("certificate.fingerprint", isFingerprint(certificate.sha256Fingerprint),
                  certificate.sha256Fingerprint)
            check("certificate.commonName", !certificate.commonName.isEmpty,
                  certificate.commonName)
        } else {
            check("certificate.preparation", false,
                  prepared.error?.localizedDescription ?? "no certificate")
        }

        // 2. Serve a payload over the real listeners and read it back through
        //    127.0.0.1, exercising the plain HTTP page and the TLS listener.
        if let payload = makePayload() {
            let server = IPInstallServer(bundleID: "com.example.ipastore.smoke",
                                         version: "1.2.3",
                                         title: "IPAShelf Smoke",
                                         ipaAtPath: payload.path,
                                         artworkPath: nil)
            defer {
                server.stop()
                try? FileManager.default.removeItem(at: payload)
            }
            check("server.start", server.start(),
                  server.startupError?.localizedDescription ?? "")

            if let page = server.pageURL, let pageURL = URL(string: page),
               let plainPort = pageURL.port {
                let trust = SmokeTrustDelegate()
                let insecure = URLSession(configuration: .ephemeral)
                let trusted = URLSession(configuration: .ephemeral,
                                         delegate: trust, delegateQueue: nil)

                let index = httpGet(URL(string: "http://127.0.0.1:\(plainPort)/")!,
                                    session: insecure)
                check("http.index", index?.status == 200,
                      "status \(index?.status ?? -1)")
                check("http.index.body",
                      index.map { String(decoding: $0.body, as: UTF8.self)
                          .contains("com.example.ipastore.smoke") } ?? false)

                let ca = httpGet(URL(string: "http://127.0.0.1:\(plainPort)/ca.cer")!,
                                 session: insecure)
                check("http.ca", ca?.status == 200, "status \(ca?.status ?? -1)")
                if let ca, let certificate = prepared.certificate {
                    check("http.ca.der", ca.body == certificate.der,
                          "\(ca.body.count) bytes")
                }

                if let manifest = server.manifestURL,
                   let manifestURL = URL(string: manifest),
                   let tlsPort = manifestURL.port {
                    let plist = httpGet(
                        URL(string: "https://127.0.0.1:\(tlsPort)/install.plist")!,
                        session: trusted)
                    check("https.manifest", plist?.status == 200,
                          "status \(plist?.status ?? -1)")
                    check("https.manifest.body",
                          plist.map { String(decoding: $0.body, as: UTF8.self)
                              .contains("com.example.ipastore.smoke") } ?? false)

                    let payloadResponse = httpGet(
                        URL(string: "https://127.0.0.1:\(tlsPort)/payload.ipa")!,
                        session: trusted)
                    check("https.payload", payloadResponse?.status == 200,
                          "status \(payloadResponse?.status ?? -1)")
                    check("https.payload.size",
                          payloadResponse?.body.count == 8192,
                          "\(payloadResponse?.body.count ?? -1) bytes")
                } else {
                    check("server.manifestURL", false)
                }
            } else {
                check("server.pageURL", false)
            }

            check("server.installURL",
                  server.installURL?.hasPrefix("itms-services://?action=download-manifest")
                      ?? false,
                  server.installURL ?? "nil")
            check("server.certificate", server.certificate != nil)
        } else {
            check("payload.creation", false)
        }

        print("\(tag) \(checks - failures)/\(checks) checks passed")
        print(failures == 0 ? "\(tag) PASS" : "\(tag) FAIL")
        exit(failures == 0 ? 0 : 1)
    }

    private static func isFingerprint(_ value: String) -> Bool {
        let parts = value.split(separator: ":")
        guard parts.count == 32 else { return false }
        return parts.allSatisfy { part in
            part.count == 2 && part.allSatisfy { $0.isHexDigit }
                && part == part.uppercased()
        }
    }

    private static func makePayload() -> URL? {
        let url = FileManager.default.temporaryDirectory
            .appendingPathComponent("smoke-payload.ipa")
        var bytes = [UInt8](repeating: 0, count: 8192)
        for index in bytes.indices {
            bytes[index] = UInt8(truncatingIfNeeded: index &* 31 &+ 7)
        }
        do {
            try Data(bytes).write(to: url)
            return url
        } catch {
            print("\(tag) could not write the payload: \(error.localizedDescription)")
            return nil
        }
    }

    private struct Response {
        let status: Int
        let body: Data
    }

    private static func httpGet(_ url: URL, session: URLSession) -> Response? {
        let semaphore = DispatchSemaphore(value: 0)
        var response: Response?
        let task = session.dataTask(with: url) { data, urlResponse, error in
            if let http = urlResponse as? HTTPURLResponse {
                response = Response(status: http.statusCode, body: data ?? Data())
            } else if let error {
                print("\(tag) request to \(url.path) failed: \(error.localizedDescription)")
            }
            semaphore.signal()
        }
        task.resume()
        if semaphore.wait(timeout: .now() + 30) == .timedOut {
            task.cancel()
            print("\(tag) request to \(url.path) timed out")
            return nil
        }
        return response
    }

    #endif
}

#if DEBUG
/// The local root CA is not trusted by the simulator, so accept the identity
/// the server presents. This only runs inside the smoke test.
private final class SmokeTrustDelegate: NSObject, URLSessionDelegate {
    func urlSession(_ session: URLSession,
                    didReceive challenge: URLAuthenticationChallenge,
                    completionHandler: @escaping (URLSession.AuthChallengeDisposition,
                                                  URLCredential?) -> Void) {
        guard let trust = challenge.protectionSpace.serverTrust else {
            completionHandler(.performDefaultHandling, nil)
            return
        }
        completionHandler(.useCredential, URLCredential(trust: trust))
    }
}

#endif
