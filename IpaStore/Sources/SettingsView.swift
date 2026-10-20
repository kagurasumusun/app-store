// SPDX-License-Identifier: MIT

import SwiftUI
import UIKit

struct SettingsView: View {
    @Environment(AppModel.self) private var model
    @State private var showAcknowledgements = false
    @State private var showDeleteAll = false

    private var installService: InstallService { model.installService }

    var body: some View {
        NavigationStack {
            Form {
                installSection
                deviceSection
                storageSection
                aboutSection
            }
            .scrollContentBackground(.hidden)
            .background(AppBackground())
            .navigationTitle("Settings")
            .sheet(isPresented: $showAcknowledgements) {
                AcknowledgementsView()
            }
            .confirmationDialog("Delete all downloaded packages?",
                                isPresented: $showDeleteAll,
                                titleVisibility: .visible) {
                Button("Delete All", role: .destructive) {
                    model.deleteAllLibraryItems()
                }
            } message: {
                Text("Every IPA in Downloads will be removed from this device. Installed apps are not affected.")
            }
            .onAppear {
                installService.prepareCertificate()
                model.refreshLibrary()
            }
        }
    }

    // MARK: On-device installation

    private var installSection: some View {
        Section {
            HStack {
                Text("Root certificate")
                Spacer()
                if installService.hasCertificate {
                    CopyableIdentifier(value: installService.certificateFingerprintShort)
                } else {
                    Text("Not created")
                        .foregroundStyle(.tertiary)
                }
            }
            Button {
                if !installService.openCertificateProfile(),
                   let error = installService.certificateError {
                    model.presentMessage("Certificate", error)
                }
            } label: {
                Label("Install Root Certificate", systemImage: "checkmark.shield")
            }
            Button {
                if let url = URL(string: UIApplication.openSettingsURLString) {
                    UIApplication.shared.open(url)
                }
            } label: {
                Label("Open Settings", systemImage: "gearshape")
            }
            certificateSteps
            if let error = installService.certificateError {
                Text(error)
                    .font(.caption)
                    .foregroundStyle(.red)
            }
        } header: {
            Text("On-Device Installation")
        } footer: {
            Text("Installing a downloaded package on this device needs the iOS OTA flow: a manifest served over HTTPS from a certificate the device trusts. IPAShelf generates that certificate locally and serves everything on this device only — the private keys never leave the Keychain.")
        }
    }

    private var certificateSteps: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("One-time setup")
                .font(.subheadline.weight(.semibold))
            step(1, "Tap “Install Root Certificate” and allow the profile download in Safari.")
            step(2, "Install it under Settings › General › VPN & Device Management.")
            step(3, "Enable “IPAShelf Root CA” under Settings › General › About › Certificate Trust Settings.")
            step(4, "Open Downloads and tap Install on any package.")
        }
        .padding(.vertical, 4)
    }

    private func step(_ number: Int, _ text: String) -> some View {
        HStack(alignment: .top, spacing: 8) {
            Text("\(number)")
                .font(.caption.weight(.bold))
                .foregroundStyle(.white)
                .frame(width: 18, height: 18)
                .background(Circle().fill(Color.indigo))
            Text(text)
                .font(.caption)
                .foregroundStyle(.secondary)
        }
    }

    // MARK: Device

    private var deviceSection: some View {
        Section {
            HStack {
                Text("Device ID (GUID)")
                Spacer()
                CopyableIdentifier(value: model.deviceIdentifier)
            }
        } header: {
            Text("Device")
        } footer: {
            Text("A random identifier generated on first launch and kept in the Keychain. Apple sees it as the device that signs in and downloads, like a Mac running Configurator. It never changes on its own — changing it would orphan your signed-in accounts.")
        }
    }

    // MARK: Storage

    private var storageSection: some View {
        Section {
            InfoRow(label: "Downloaded packages", value: "\(model.library.count)")
            InfoRow(label: "Space used",
                    value: ByteCountFormatter.string(fromByteCount: model.libraryTotalSize, countStyle: .file))
            if !model.library.isEmpty {
                Button(role: .destructive) {
                    showDeleteAll = true
                } label: {
                    Label("Delete All Packages", systemImage: "trash")
                }
            }
        } header: {
            Text("Storage")
        } footer: {
            Text("Packages are stored in the app's Documents folder and are visible in the Files app under IPAShelf.")
        }
    }

    // MARK: About

    private var aboutSection: some View {
        Section {
            InfoRow(label: "Version", value: model.appVersion)
            Button {
                showAcknowledgements = true
            } label: {
                Label("Acknowledgements", systemImage: "doc.text")
            }
        } header: {
            Text("About")
        } footer: {
            Text("IPAShelf downloads App Store packages with your own Apple ID, including older versions, and installs them on this device. Sign-in is protected by a complete in-app implementation of Apple's SAP signing protocol — no servers, no private frameworks.")
        }
    }
}

struct AcknowledgementsView: View {
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: 18) {
                    Text("miniz 3.0.2")
                        .font(.headline)
                    Text("""
                    Copyright 2013-2025 Nikolai Torkelsson and contributors.

                    Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the “Software”), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

                    The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

                    THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
                    """)
                    .font(.footnote.monospaced())

                    Divider()

                    Text("SAP protocol constant tables")
                        .font(.headline)
                    Text("""
                    MIT License — Copyright (c) 2025 Lakr233 (ApplePackage project).

                    The SAP v200 signer is an independent implementation written for this app; its interoperability constant tables (fixed substitution and routing data, no executable code) were reverse-engineered and published by the ApplePackage project under the MIT License. The full license text ships in ThirdParty/sap-tables/LICENSE in the source repository.
                    """)
                    .font(.footnote)
                    .foregroundStyle(.secondary)

                    Divider()

                    Text("Protocol notes")
                        .font(.headline)
                    Text("""
                    The App Store protocol layer is an independent C++ implementation. It was written from observed request and response formats; no source code from existing tools is included.

                    Sign-in requests are signed by a complete in-app implementation of Apple’s SAP v200 protocol — pure software, no private frameworks, so it runs the same on device and in the Simulator. Download metadata and FairPlay Sinf blobs stay inside the package; the app does not decrypt app code.

                    The OTA manifest, the HTTP parser, the range/file serving logic and the X.509 encoder are part of the same C++ core and are covered by the portable test suite.
                    """)
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                }
                .padding(20)
            }
            .navigationTitle("Acknowledgements")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                }
            }
        }
    }
}
