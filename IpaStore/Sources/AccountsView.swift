// SPDX-License-Identifier: MIT

import SwiftUI
import UIKit

/// The account hub behind the avatar button: accounts, on-device
/// installation, appearance, storage and about — like the App Store
/// account page.
struct AccountHubView: View {
    @Environment(AppModel.self) private var model
    @Environment(ThemeStore.self) private var theme
    @Environment(\.dismiss) private var dismiss

    @State private var accountToRemove: StoreAccount?
    @State private var showDeleteAll = false

    private var installService: InstallService { model.installService }

    var body: some View {
        @Bindable var theme = theme

        NavigationStack {
            List {
                identitySection
                accountsSection
                installSection
                appearanceSection(bindableTheme: $theme)
                storageSection
                deviceSection
                aboutSection
            }
            .listStyle(.insetGrouped)
            .navigationTitle("Account")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                        .fontWeight(.semibold)
                }
            }
            .navigationDestination(for: StoreAccount.self) { account in
                AccountDetailView(account: account)
            }
            .navigationDestination(for: HubDestination.self) { destination in
                switch destination {
                case .acknowledgements:
                    AcknowledgementsView()
                }
            }
            .confirmationDialog("Remove This Account?",
                                isPresented: Binding(
                                    get: { accountToRemove != nil },
                                    set: { if !$0 { accountToRemove = nil } }),
                                titleVisibility: .visible) {
                Button("Remove", role: .destructive) {
                    if let accountToRemove { model.remove(accountToRemove) }
                }
            } message: {
                Text("Its Keychain entry will be deleted. Downloaded packages remain.")
            }
            .confirmationDialog("Delete All Downloaded Packages?",
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
                installService.refreshTrustStatus()
                model.refreshLibrary()
            }
        }
    }

    // MARK: Identity

    @ViewBuilder
    private var identitySection: some View {
        Section {
            if let account = model.activeAccount {
                HStack(spacing: 14) {
                    avatarCircle(for: account, size: 60)
                    VStack(alignment: .leading, spacing: 3) {
                        Text(account.name.isEmpty ? account.email : account.name)
                            .font(.system(size: 19, weight: .bold))
                            .lineLimit(1)
                        Text(account.email)
                            .font(.system(size: 13))
                            .foregroundStyle(.secondary)
                            .lineLimit(1)
                        Text(String(localized: "Active account · \(account.region)"))
                            .font(.system(size: 12))
                            .foregroundStyle(.tertiary)
                    }
                }
                .padding(.vertical, 6)
            } else {
                Button {
                    model.showLogin = true
                } label: {
                    HStack(spacing: 14) {
                        Image(systemName: "person.crop.circle.badge.plus")
                            .font(.system(size: 40, weight: .thin))
                            .foregroundStyle(theme.accentColor)
                        VStack(alignment: .leading, spacing: 3) {
                            Text("Sign in with your Apple ID")
                                .font(.system(size: 16, weight: .semibold))
                                .foregroundStyle(.primary)
                            Text("Get free apps and download packages, including older versions.")
                                .font(.system(size: 12))
                                .foregroundStyle(.secondary)
                        }
                        Spacer()
                    }
                    .padding(.vertical, 6)
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
            }
        }
    }

    private func avatarCircle(for account: StoreAccount, size: CGFloat) -> some View {
        Text(account.initial)
            .font(.system(size: size * 0.42, weight: .bold))
            .foregroundStyle(.white)
            .frame(width: size, height: size)
            .background {
                Circle().fill(LinearGradient(
                    colors: [theme.accentColor, theme.accentColor.opacity(0.6)],
                    startPoint: .top, endPoint: .bottom))
            }
    }

    // MARK: Accounts

    private var accountsSection: some View {
        Section {
            ForEach(model.accounts) { account in
                NavigationLink(value: account) {
                    AccountRow(account: account,
                               isActive: account == model.activeAccount)
                }
                .swipeActions(edge: .trailing, allowsFullSwipe: false) {
                    Button(role: .destructive) {
                        accountToRemove = account
                    } label: {
                        Label("Remove", systemImage: "trash")
                    }
                    if account != model.activeAccount {
                        Button {
                            model.setActive(account)
                        } label: {
                            Label("Use", systemImage: "checkmark.circle")
                        }
                        .tint(theme.accentColor)
                    }
                }
            }
            Button {
                model.showLogin = true
            } label: {
                Label("Add Account", systemImage: "plus.circle.fill")
            }
        } header: {
            Text("Accounts")
        } footer: {
            Text("The active account is used for purchases, downloads and the purchase history. Passwords and tokens stay in the Keychain.")
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
            if installService.hasCertificate {
                HStack {
                    Text("Trust on this device")
                    Spacer()
                    if installService.rootCertificateTrusted == true {
                        Label("Trusted", systemImage: "checkmark.circle.fill")
                            .foregroundStyle(.green)
                    } else {
                        Label("Not trusted yet", systemImage: "exclamationmark.triangle.fill")
                            .foregroundStyle(.orange)
                    }
                }
            }
            Button {
                if !installService.openCertificateProfile(),
                   let error = installService.certificateError {
                    model.presentMessage(String(localized: "Certificate"), error)
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
            DisclosureGroup {
                VStack(alignment: .leading, spacing: 8) {
                    step(1, String(localized: "Tap “Install Root Certificate” and allow the profile download in Safari."))
                    step(2, String(localized: "Install it under Settings › General › VPN & Device Management."))
                    step(3, String(localized: "Enable “IPAShelf Root CA” under Settings › General › About › Certificate Trust Settings."))
                    step(4, String(localized: "Open Downloads and tap Install on any package."))
                }
                .padding(.vertical, 4)
            } label: {
                Text("One-Time Setup")
            }
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

    private func step(_ number: Int, _ text: String) -> some View {
        HStack(alignment: .top, spacing: 8) {
            Text("\(number)")
                .font(.system(size: 11, weight: .bold))
                .foregroundStyle(.white)
                .frame(width: 18, height: 18)
                .background(Circle().fill(theme.accentColor))
            Text(text)
                .font(.system(size: 12))
                .foregroundStyle(.secondary)
        }
    }

    // MARK: Appearance

    private func appearanceSection(bindableTheme: Bindable<ThemeStore>) -> some View {
        Section {
            VStack(alignment: .leading, spacing: 12) {
                HStack {
                    Text("Accent Color")
                    Spacer()
                    Text(theme.accent.title)
                        .foregroundStyle(.secondary)
                }
                HStack(spacing: 14) {
                    ForEach(AccentChoice.allCases) { choice in
                        Button {
                            theme.accent = choice
                        } label: {
                            ZStack {
                                Circle()
                                    .fill(choice.color)
                                    .frame(width: 32, height: 32)
                                if theme.accent == choice {
                                    Image(systemName: "checkmark")
                                        .font(.system(size: 13, weight: .bold))
                                        .foregroundStyle(.white)
                                }
                            }
                            .overlay {
                                if theme.accent == choice {
                                    Circle()
                                        .strokeBorder(choice.color, lineWidth: 2)
                                        .padding(-5)
                                }
                            }
                        }
                        .buttonStyle(.plain)
                        .accessibilityLabel(choice.title)
                    }
                }
                .padding(.vertical, 5)
            }
            .padding(.vertical, 2)
            Picker("Appearance", selection: bindableTheme.appearance) {
                ForEach(AppearanceChoice.allCases) { choice in
                    Text(choice.title).tag(choice)
                }
            }
            .pickerStyle(.segmented)
        } header: {
            Text("Appearance")
        }
        .sensoryFeedback(.selection, trigger: theme.accent)
    }

    // MARK: Storage / Device / About

    private var storageSection: some View {
        Section {
            InfoRow(label: String(localized: "Downloaded packages"),
                    value: "\(model.library.count)")
            InfoRow(label: String(localized: "Space used"),
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

    private var aboutSection: some View {
        Section {
            InfoRow(label: String(localized: "Version"), value: model.appVersion)
            NavigationLink(value: HubDestination.acknowledgements) {
                Label("Acknowledgements", systemImage: "doc.text")
            }
        } header: {
            Text("About")
        } footer: {
            Text("IPAShelf downloads App Store packages with your own Apple ID, including older versions, and installs them on this device. Sign-in is protected by a complete in-app implementation of Apple's SAP signing protocol — no servers, no private frameworks.")
        }
    }
}

private enum HubDestination: Hashable {
    case acknowledgements
}

private struct AccountRow: View {
    @Environment(ThemeStore.self) private var theme
    let account: StoreAccount
    let isActive: Bool

    var body: some View {
        HStack(spacing: 12) {
            Text(account.initial)
                .font(.system(size: 16, weight: .bold))
                .foregroundStyle(isActive ? Color.white : theme.accentColor)
                .frame(width: 40, height: 40)
                .background {
                    Circle().fill(isActive
                                  ? theme.accentColor
                                  : theme.accentColor.opacity(0.14))
                }
            VStack(alignment: .leading, spacing: 2) {
                Text(account.name.isEmpty ? account.email : account.name)
                    .font(.system(size: 15, weight: .semibold))
                    .lineLimit(1)
                Text("\(account.email) · \(account.region)")
                    .font(.system(size: 12))
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
            }
            Spacer()
            if isActive {
                Image(systemName: "checkmark.circle.fill")
                    .foregroundStyle(.green)
                    .accessibilityLabel(String(localized: "Active"))
            }
        }
        .padding(.vertical, 3)
    }
}

struct AccountDetailView: View {
    @Environment(AppModel.self) private var model
    @Environment(ThemeStore.self) private var theme
    @Environment(\.dismiss) private var dismiss
    let account: StoreAccount

    @State private var needsCode = false
    @State private var authCode = ""
    @State private var didRefresh = false
    @State private var showRemove = false

    /// The stored copy, refreshed after rotation or activation changes.
    private var current: StoreAccount {
        model.accounts.first { $0.email == account.email } ?? account
    }

    var body: some View {
        List {
            Section {
                HStack(spacing: 12) {
                    Text(current.initial)
                        .font(.system(size: 22, weight: .bold))
                        .foregroundStyle(.white)
                        .frame(width: 52, height: 52)
                        .background {
                            Circle().fill(LinearGradient(
                                colors: [theme.accentColor, theme.accentColor.opacity(0.6)],
                                startPoint: .top, endPoint: .bottom))
                        }
                    VStack(alignment: .leading, spacing: 2) {
                        Text(current.name.isEmpty ? current.email : current.name)
                            .font(.system(size: 16, weight: .semibold))
                        Text(current.email)
                            .font(.system(size: 12))
                            .foregroundStyle(.secondary)
                    }
                }
                .padding(.vertical, 4)
                InfoRow(label: String(localized: "Region"), value: current.region)
                InfoRow(label: String(localized: "Storefront"), value: current.storefront)
                if !current.directoryServicesID.isEmpty {
                    InfoRow(label: "DSID", value: String(current.directoryServicesID.prefix(4)) + "…")
                }
                InfoRow(label: String(localized: "Password saved"),
                        value: current.hasSavedPassword ? String(localized: "Yes") : String(localized: "No"))
            }

            Section {
                if current != model.activeAccount {
                    Button {
                        model.setActive(current)
                    } label: {
                        Label("Use This Account", systemImage: "checkmark.circle")
                    }
                }
                if needsCode {
                    TextField("Verification code", text: $authCode)
                        .keyboardType(.numberPad)
                        .textContentType(.oneTimeCode)
                }
                Button {
                    Task { await rotate() }
                } label: {
                    HStack {
                        Label(needsCode
                              ? String(localized: "Refresh with Code")
                              : String(localized: "Refresh Token"),
                              systemImage: "arrow.triangle.2.circlepath")
                        Spacer()
                        if model.isRefreshingToken {
                            ProgressView().controlSize(.small)
                        } else if didRefresh {
                            Image(systemName: "checkmark.circle.fill")
                                .foregroundStyle(.green)
                        }
                    }
                }
                .disabled(!current.hasSavedPassword || model.isRefreshingToken
                          || (needsCode && authCode.count != 6))
            } header: {
                Text("Token")
            } footer: {
                if current.hasSavedPassword {
                    Text("Apple periodically expires the download token. Refresh signs in again with the saved password and stores the new token. IPAShelf also does this automatically when a download reports an expired token.")
                } else {
                    Text("This account was added without saving its password, so it cannot be refreshed automatically. Remove it and sign in again.")
                }
            }

            Section {
                Button(role: .destructive) {
                    showRemove = true
                } label: {
                    Label("Remove Account", systemImage: "trash")
                }
            }
        }
        .listStyle(.insetGrouped)
        .navigationTitle("Account")
        .navigationBarTitleDisplayMode(.inline)
        .confirmationDialog("Remove This Account?",
                            isPresented: $showRemove,
                            titleVisibility: .visible) {
            Button("Remove", role: .destructive) {
                model.remove(current)
                dismiss()
            }
        } message: {
            Text("Its Keychain entry will be deleted. Downloaded packages remain.")
        }
    }

    private func rotate() async {
        let outcome = await model.refreshToken(
            for: current,
            authCode: needsCode ? authCode : nil)
        switch outcome {
        case .signedIn:
            didRefresh = true
            needsCode = false
            authCode = ""
        case .twoFactorRequired:
            needsCode = true
        case nil:
            break
        }
    }
}

// MARK: - Sign in

struct LoginView: View {
    @Environment(AppModel.self) private var model
    @Environment(ThemeStore.self) private var theme
    @Environment(\.dismiss) private var dismiss

    @State private var email = ""
    @State private var password = ""
    @State private var authCode = ""
    @State private var needsCode = false
    @State private var isBusy = false
    @FocusState private var focusedField: Field?

    private enum Field {
        case email, password, code
    }

    private var canSubmit: Bool {
        email.contains("@") && email.contains(".")
            && !password.isEmpty
            && (!needsCode || authCode.count == 6)
            && !isBusy
    }

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(spacing: 22) {
                    VStack(spacing: 12) {
                        Image(systemName: "person.crop.circle.fill")
                            .font(.system(size: 64, weight: .thin))
                            .foregroundStyle(theme.accentColor)
                        Text("Sign in with Apple ID")
                            .font(.system(size: 24, weight: .bold))
                        Text(needsCode
                             ? String(localized: "Enter the six-digit code Apple sent to your trusted devices.")
                             : String(localized: "Use the account you use in the App Store."))
                            .font(.system(size: 14))
                            .foregroundStyle(.secondary)
                            .multilineTextAlignment(.center)
                    }
                    .padding(.top, 28)

                    VStack(spacing: 12) {
                        field(symbol: "envelope") {
                            TextField("Apple ID email", text: $email)
                                .keyboardType(.emailAddress)
                                .textInputAutocapitalization(.never)
                                .autocorrectionDisabled()
                                .textContentType(.emailAddress)
                                .focused($focusedField, equals: .email)
                                .submitLabel(.next)
                        }
                        field(symbol: "lock") {
                            SecureField("Password", text: $password)
                                .textContentType(.password)
                                .focused($focusedField, equals: .password)
                                .submitLabel(needsCode ? .next : .go)
                        }
                        if needsCode {
                            field(symbol: "key") {
                                TextField("Verification code", text: $authCode)
                                    .keyboardType(.numberPad)
                                    .textContentType(.oneTimeCode)
                                    .focused($focusedField, equals: .code)
                            }
                            .transition(.opacity.combined(with: .move(edge: .top)))
                        }
                    }
                    .animation(.default, value: needsCode)
                    .onSubmit {
                        switch focusedField {
                        case .email:
                            focusedField = .password
                        case .password where needsCode:
                            focusedField = .code
                        default:
                            if canSubmit { Task { await submit() } }
                        }
                    }

                    Button {
                        Task { await submit() }
                    } label: {
                        Group {
                            if isBusy {
                                ProgressView()
                            } else {
                                Text(needsCode
                                     ? String(localized: "Verify and Sign In")
                                     : String(localized: "Sign In"))
                                    .font(.system(size: 16, weight: .semibold))
                            }
                        }
                        .frame(maxWidth: .infinity)
                    }
                    .buttonStyle(.borderedProminent)
                    .buttonBorderShape(.capsule)
                    .controlSize(.large)
                    .disabled(!canSubmit)

                    Text("Credentials are sent only to Apple over SAP-signed requests and stored in the Keychain so the download token can be refreshed later.")
                        .font(.system(size: 12))
                        .foregroundStyle(.secondary)
                        .multilineTextAlignment(.center)
                        .padding(.horizontal, 8)
                }
                .padding(.horizontal, 24)
                .padding(.bottom, 24)
            }
            .scrollDismissesKeyboard(.interactively)
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { dismiss() }
                        .disabled(isBusy)
                }
            }
            .onAppear {
                focusedField = .email
            }
        }
    }

    private func field<Content: View>(symbol: String,
                                      @ViewBuilder content: () -> Content) -> some View {
        HStack(spacing: 10) {
            Image(systemName: symbol)
                .font(.system(size: 15, weight: .medium))
                .foregroundStyle(.secondary)
                .frame(width: 22)
            content()
        }
        .padding(.horizontal, 14)
        .frame(minHeight: 50)
        .background {
            RoundedRectangle(cornerRadius: 12, style: .continuous)
                .fill(Color(.secondarySystemBackground))
        }
        .overlay {
            RoundedRectangle(cornerRadius: 12, style: .continuous)
                .strokeBorder(Color.primary.opacity(0.06), lineWidth: 0.5)
        }
    }

    private func submit() async {
        isBusy = true
        defer { isBusy = false }
        let outcome = await model.login(
            email: email.trimmingCharacters(in: .whitespacesAndNewlines),
            password: password,
            authCode: needsCode ? authCode : nil,
            region: model.region)
        switch outcome {
        case .signedIn:
            dismiss()
        case .twoFactorRequired:
            needsCode = true
            focusedField = .code
        case nil:
            break
        }
    }
}

// MARK: - Acknowledgements

struct AcknowledgementsView: View {
    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 18) {
                Text(verbatim: "miniz 3.0.2")
                    .font(.headline)
                Text(verbatim: """
                Copyright 2013-2025 Nikolai Torkelsson and contributors.

                Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the “Software”), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

                The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

                THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
                """)
                .font(.footnote.monospaced())

                Divider()

                Text("SAP protocol constant tables")
                    .font(.headline)
                Text(verbatim: """
                MIT License — Copyright (c) 2025 Lakr233 (ApplePackage project).

                The SAP v200 signer is an independent implementation written for this app; its interoperability constant tables (fixed substitution and routing data, no executable code) were reverse-engineered and published by the ApplePackage project under the MIT License. The full license text ships in ThirdParty/sap-tables/LICENSE in the source repository.
                """)
                .font(.footnote)
                .foregroundStyle(.secondary)

                Divider()

                Text("Protocol notes")
                    .font(.headline)
                Text(verbatim: """
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
    }
}
