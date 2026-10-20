// SPDX-License-Identifier: MIT

import SwiftUI

struct AccountsView: View {
    @Environment(AppModel.self) private var model
    @Environment(\.dismiss) private var dismiss
    @State private var showLogin = false
    @State private var pendingRemoval: StoreAccount?

    var body: some View {
        NavigationStack {
            List {
                Section {
                    if model.accounts.isEmpty {
                        Text("No accounts yet. Add the Apple Account you use for App Store purchases.")
                            .font(.subheadline)
                            .foregroundStyle(.secondary)
                    } else {
                        ForEach(model.accounts) { account in
                            Button {
                                model.setActive(account)
                            } label: {
                                AccountRow(account: account,
                                           isActive: account == model.activeAccount)
                            }
                            .buttonStyle(.plain)
                            .swipeActions(edge: .trailing, allowsFullSwipe: false) {
                                Button(role: .destructive) {
                                    pendingRemoval = account
                                } label: {
                                    Label("Remove", systemImage: "trash")
                                }
                            }
                        }
                    }
                } header: {
                    Text("Accounts")
                } footer: {
                    Text("Account credentials and tokens are stored in the Keychain. Switching accounts changes the storefront used for new requests.")
                }

                Section {
                    Button {
                        showLogin = true
                    } label: {
                        Label("Add Apple Account", systemImage: "plus.circle.fill")
                    }
                }

                Section("Protocol identity") {
                    InfoRow(label: "Device ID", value: model.deviceIdentifier)
                }
            }
            .navigationTitle("Accounts")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                }
            }
            .sheet(isPresented: $showLogin) {
                LoginView()
            }
            .confirmationDialog("Remove this account?",
                                isPresented: Binding(
                                    get: { pendingRemoval != nil },
                                    set: { if !$0 { pendingRemoval = nil } }),
                                titleVisibility: .visible) {
                Button("Remove", role: .destructive) {
                    if let pendingRemoval { model.remove(pendingRemoval) }
                    pendingRemoval = nil
                }
            } message: {
                Text("The token and password for this account are deleted from the Keychain. Downloaded packages stay on disk.")
            }
        }
    }
}

private struct AccountRow: View {
    let account: StoreAccount
    let isActive: Bool

    var body: some View {
        HStack(spacing: 12) {
            ZStack {
                Circle().fill(Color.indigo.opacity(0.15))
                Text(initials)
                    .font(.subheadline.weight(.bold))
                    .foregroundStyle(.indigo)
            }
            .frame(width: 38, height: 38)

            VStack(alignment: .leading, spacing: 2) {
                Text(account.name.isEmpty ? account.email : account.name)
                    .font(.body.weight(.medium))
                    .foregroundStyle(.primary)
                Text(account.email)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            Spacer()
            VStack(alignment: .trailing, spacing: 2) {
                Text(account.region)
                    .font(.caption.weight(.semibold))
                    .foregroundStyle(.secondary)
                if !account.storefront.isEmpty {
                    Text("Front \(account.storefront)")
                        .font(.system(size: 9).monospaced())
                        .foregroundStyle(.tertiary)
                }
                if isActive {
                    Text("ACTIVE")
                        .font(.system(size: 9, weight: .bold))
                        .foregroundStyle(.indigo)
                }
            }
        }
        .contentShape(Rectangle())
        .padding(.vertical, 2)
    }

    private var initials: String {
        let source = account.name.isEmpty ? account.email : account.name
        let parts = source.split(separator: " ")
        let letters = parts.prefix(2).compactMap { $0.first }
        return letters.isEmpty ? "?" : String(letters).uppercased()
    }
}

struct LoginView: View {
    @Environment(AppModel.self) private var model
    @Environment(\.dismiss) private var dismiss

    private enum Stage {
        case credentials
        case twoFactor
    }

    @State private var stage: Stage = .credentials
    @State private var email = ""
    @State private var password = ""
    @State private var code = ""
    @State private var region = "US"
    @State private var isWorking = false
    @State private var errorMessage: String?

    private var regions: [StoreStorefront] { model.storefronts }

    var body: some View {
        NavigationStack {
            Form {
                if stage == .credentials {
                    credentials
                } else {
                    twoFactor
                }

                if let errorMessage {
                    Section {
                        Label(errorMessage, systemImage: "exclamationmark.triangle.fill")
                            .font(.footnote)
                            .foregroundStyle(.orange)
                    }
                }

                Section {
                    Label("Apple signs every request with a device-bound SAP signature. Credentials are never sent to any server other than Apple’s.",
                          systemImage: "lock.shield")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }
            .navigationTitle(stage == .credentials ? "Sign In" : "Two-Factor Code")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { dismiss() }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button(stage == .credentials ? "Sign In" : "Verify") {
                        Task { await submit() }
                    }
                    .disabled(!canSubmit || isWorking)
                }
            }
            .interactiveDismissDisabled(isWorking)
        }
        .onAppear {
            region = model.activeAccount?.region ?? model.region
        }
    }

    private var credentials: some View {
        Group {
            Section("Apple Account") {
                TextField("Email", text: $email)
                    .keyboardType(.emailAddress)
                    .textContentType(.username)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                SecureField("Password", text: $password)
                    .textContentType(.password)
            }

            Section {
                Picker("Country or Region", selection: $region) {
                    ForEach(regions) { storefront in
                        Text("\(storefront.flag) \(storefront.name) (\(storefront.currency))")
                            .tag(storefront.code)
                    }
                }
                .pickerStyle(.navigationLink)
            } header: {
                Text("Storefront")
            } footer: {
                Text("Apple associates purchases with the storefront of the account. Choose the region where the account was created.")
            }
        }
    }

    private var twoFactor: some View {
        Group {
            Section {
                Text("Enter the six-digit code shown on your other Apple devices. It refreshes every few minutes.")
                    .font(.subheadline)
            }
            Section("Verification Code") {
                TextField("123456", text: $code)
                    .keyboardType(.numberPad)
                    .textContentType(.oneTimeCode)
                    .font(.title3.monospacedDigit())
                    .onChange(of: code) { _, newValue in
                        code = String(newValue.filter(\.isNumber).prefix(6))
                    }
            }
            Section {
                Button("Use a different account") {
                    stage = .credentials
                    code = ""
                    errorMessage = nil
                }
            }
        }
    }

    private var canSubmit: Bool {
        if stage == .credentials {
            return email.contains("@") && password.count >= 1
        }
        return code.count == 6
    }

    private func submit() async {
        guard canSubmit else { return }
        isWorking = true
        errorMessage = nil
        defer { isWorking = false }

        do {
            let outcome = try await model.service.login(
                email: email.trimmingCharacters(in: .whitespacesAndNewlines),
                password: password,
                authCode: stage == .twoFactor ? code : nil,
                region: region)
            switch outcome {
            case .signedIn:
                model.reloadAccounts()
                password = ""
                code = ""
                dismiss()
            case .twoFactorRequired:
                stage = .twoFactor
            }
        } catch {
            let storeError = StoreError(error)
            errorMessage = message(for: storeError)
        }
    }

    private func message(for error: StoreError) -> String {
        switch error.code {
        case .signerUnavailable:
            return "SAP signing is unavailable. CoreFP only ships on physical iPhones and iPads — the Simulator cannot sign in."
        case .unauthorized:
            return "Apple rejected the credentials. Check the password, or enter the current two-factor code."
        case .network:
            return "The network request failed. Check the connection and try again."
        default:
            return error.message
        }
    }
}
