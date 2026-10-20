// SPDX-License-Identifier: MIT

import SwiftUI
import UIKit

struct SettingsView: View {
    @Environment(AppModel.self) private var model
    @State private var showAcknowledgements = false
    @State private var showRegionPicker = false
    @State private var showRemoveActive = false

    var body: some View {
        NavigationStack {
            Form {
                accountSection
                storeSection
                installationSection
                librarySection
                wishlistSection
                deviceSection
                aboutSection
                legalSection
            }
            .navigationTitle("Settings")
            .sheet(isPresented: $showAcknowledgements) {
                AcknowledgementsView()
            }
            .sheet(isPresented: $showRegionPicker) {
                RegionPickerView()
            }
            .confirmationDialog("Remove the active account?",
                                isPresented: $showRemoveActive,
                                titleVisibility: .visible) {
                Button("Remove", role: .destructive) {
                    if let account = model.activeAccount {
                        model.remove(account)
                    }
                }
            } message: {
                Text("Its Keychain entry will be deleted. Downloaded packages remain.")
            }
        }
    }

    private var accountSection: some View {
        Section("Account") {
            if let account = model.activeAccount {
                VStack(alignment: .leading, spacing: 3) {
                    Text(account.name.isEmpty ? account.email : account.name)
                        .font(.body.weight(.medium))
                    Text("\(account.email) · \(account.region)")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                .padding(.vertical, 2)
                Button("Manage Accounts (\(model.accounts.count))") { model.showAccounts = true }
                Button("Remove This Account", role: .destructive) {
                    showRemoveActive = true
                }
            } else {
                Button("Sign In…") { model.showLogin = true }
            }
        }
    }

    private var storeSection: some View {
        Section {
            Picker("Preferred Platform", selection: Binding(
                get: { model.platform },
                set: { model.setPlatform($0) })) {
                ForEach(StorePlatform.allCases) { platform in
                    Label(platform.title, systemImage: platform.symbol)
                        .tag(platform)
                }
            }
            Button {
                showRegionPicker = true
            } label: {
                HStack {
                    Text("Storefront")
                    Spacer()
                    Text("\(model.currentStorefront?.flag ?? "") \(model.regionName(model.region))")
                        .foregroundStyle(.secondary)
                }
            }
        } header: {
            Text("Store")
        } footer: {
            Text("The storefront controls search results, ratings, reviews and which package Apple returns. The platform picker chooses the build family. Signing in again with a different region changes the storefront attached to that account.")
        }
    }

    private var installationSection: some View {
        Section {
            HStack {
                Text("Root certificate")
                Spacer()
                if model.installService.certificateFingerprint.isEmpty {
                    Text("Not created")
                        .foregroundStyle(.tertiary)
                } else {
                    CopyableIdentifier(value: model.installService.certificateFingerprintShort)
                }
            }
            Button {
                model.presentInstallSheet()
            } label: {
                Label("Open install server", systemImage: "square.and.arrow.down.on.square")
            }
            NavigationLink {
                CertificateGuideView()
            } label: {
                Label("Certificate trust guide", systemImage: "checkmark.shield")
            }
        } header: {
            Text("Installation")
        } footer: {
            Text("IPAShelf installs packages on this device with the same on-demand flow used by enterprise apps. A locally generated root certificate must be trusted once.")
        }
        .onAppear {
            if model.installService.certificateFingerprint.isEmpty {
                model.installService.prepareCertificate()
            }
        }
    }

    private var librarySection: some View {
        Section("Library") {
            InfoRow(label: "Packages", value: "\(model.library.count)")
            InfoRow(label: "Total size",
                    value: ByteCountFormatter.string(fromByteCount: model.libraryTotalSize,
                                                     countStyle: .file))
            InfoRow(label: "Location", value: "On My iPhone › IpaStore › IPAs")
            Button("Refresh Library") { model.refreshLibrary() }
        }
    }

    private var wishlistSection: some View {
        Section {
            if model.wishlist.isEmpty {
                Text("Apps you star are collected here and on the search screen.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } else {
                ForEach(model.wishlist) { entry in
                    HStack {
                        AppArtwork(url: entry.url, size: 34, cornerRadius: 8)
                        VStack(alignment: .leading, spacing: 1) {
                            Text(entry.name)
                                .font(.subheadline)
                            Text(entry.bundleID)
                                .font(.caption2)
                                .foregroundStyle(.secondary)
                        }
                        Spacer()
                        Button {
                            model.removeWishlist(appID: entry.appID)
                        } label: {
                            Image(systemName: "star.slash")
                                .foregroundStyle(.secondary)
                        }
                        .buttonStyle(.plain)
                    }
                }
            }
        } header: {
            Text("Wishlist")
        }
    }

    private var deviceSection: some View {
        Section("Device") {
            InfoRow(label: "Protocol ID", value: model.deviceIdentifier)
            Button {
                UIPasteboard.general.string = model.deviceIdentifier
                model.presentMessage("Copied", "The protocol identity is on the clipboard.")
            } label: {
                Label("Copy Protocol ID", systemImage: "doc.on.doc")
            }
        }
    }

    private var aboutSection: some View {
        Section("About") {
            InfoRow(label: "Version", value: model.appVersion)
            InfoRow(label: "Engine", value: "IPAShelf Core (C++20)")
            Button("Acknowledgements") { showAcknowledgements = true }
            Link("Apple Media Services Terms",
                 destination: URL(string: "https://www.apple.com/legal/internet-services/itunes/")!)
        }
    }

    private var legalSection: some View {
        Section {
            Text("IPAShelf talks to the same private endpoints as Apple Configurator. It is not affiliated with Apple. Use it only with accounts and purchases you own.")
                .font(.footnote)
                .foregroundStyle(.secondary)
        }
    }
}

struct CertificateGuideView: View {
    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 16) {
                Label("Why a certificate is needed", systemImage: "lock.shield")
                    .font(.headline)
                Text("iOS only installs a package from an HTTPS manifest signed by a certificate the device trusts. IPAShelf creates a local root certificate the first time you open the install server. The private keys never leave this device.")
                    .font(.subheadline)
                    .foregroundStyle(.secondary)

                Divider()

                Label("One-time setup", systemImage: "1.circle")
                    .font(.headline)
                Text("Open the install server, tap “Install the root certificate profile”, then install it from Settings › General › VPN & Device Management.")
                    .font(.subheadline)
                    .foregroundStyle(.secondary)

                Label("Enable trust", systemImage: "2.circle")
                    .font(.headline)
                Text("Open Settings › General › About › Certificate Trust Settings and switch on “IPAShelf Root CA”. iOS asks for your passcode.")
                    .font(.subheadline)
                    .foregroundStyle(.secondary)

                Label("Install", systemImage: "3.circle")
                    .font(.headline)
                Text("Return to IPAShelf, tap Install, and confirm the system prompt. The package is served from this device over the local network; the transfer status appears in the app.")
                    .font(.subheadline)
                    .foregroundStyle(.secondary)

                Divider()

                Label("Removing it", systemImage: "trash")
                    .font(.headline)
                Text("Delete the profile in Settings › General › VPN & Device Management. Packages that are already installed keep working.")
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
            }
            .padding(20)
        }
        .navigationTitle("Certificate")
        .navigationBarTitleDisplayMode(.inline)
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

                    Text("Protocol notes")
                        .font(.headline)
                    Text("""
                    The App Store protocol layer is an independent C++ implementation. It was written from observed request and response formats; no source code from existing tools is included.

                    Sign-in uses Apple’s own SAP primitives from the system frameworks. Download metadata and FairPlay Sinf blobs stay inside the package; the app does not decrypt app code.

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
