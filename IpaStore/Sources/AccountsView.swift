// SPDX-License-Identifier: MIT

import SwiftUI

struct AccountsView: View {
    @Environment(AppModel.self) private var model
    @State private var accountToRemove: StoreAccount?

    var body: some View {
        NavigationStack {
            ZStack {
                AppBackground()
                if model.accounts.isEmpty {
                    EmptyStateView(symbol: "person.crop.circle.badge.plus",
                                   title: "No accounts",
                                   message: "Sign in with an Apple ID to purchase free apps and download packages, including older versions.",
                                   actionTitle: "Sign In",
                                   action: { model.showLogin = true })
                } else {
                    List {
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
                                        .tint(.indigo)
                                    }
                                }
                            }
                        } footer: {
                            Text("The active account is used for purchases, downloads and the purchase history. Passwords and tokens stay in the Keychain.")
                        }
                    }
                    .listStyle(.insetGrouped)
                    .scrollContentBackground(.hidden)
                }
            }
            .navigationTitle("Accounts")
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    Button {
                        model.showLogin = true
                    } label: {
                        Label("Add Account", systemImage: "plus")
                    }
                }
            }
            .navigationDestination(for: StoreAccount.self) { account in
                AccountDetailView(account: account)
            }
            .confirmationDialog("Remove this account?",
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
        }
    }
}

private struct AccountRow: View {
    let account: StoreAccount
    let isActive: Bool

    var body: some View {
        HStack(spacing: 12) {
            ZStack {
                Circle()
                    .fill(Color.indigo.opacity(isActive ? 0.85 : 0.25))
                    .frame(width: 42, height: 42)
                Text(account.initial)
                    .font(.headline)
                    .foregroundStyle(isActive ? Color.white : Color.indigo)
            }
            VStack(alignment: .leading, spacing: 2) {
                Text(account.name.isEmpty ? account.email : account.name)
                    .font(.headline)
                    .lineLimit(1)
                Text("\(account.email) · \(account.region)")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
            }
            Spacer()
            if isActive {
                Label("Active", systemImage: "checkmark.circle.fill")
                    .font(.caption.weight(.semibold))
                    .foregroundStyle(.green)
                    .labelStyle(.iconOnly)
            }
        }
        .padding(.vertical, 4)
    }
}

struct AccountDetailView: View {
    @Environment(AppModel.self) private var model
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
                    ZStack {
                        Circle()
                            .fill(Color.indigo.opacity(0.85))
                            .frame(width: 52, height: 52)
                        Text(current.initial)
                            .font(.title3.weight(.bold))
                            .foregroundStyle(.white)
                    }
                    VStack(alignment: .leading, spacing: 2) {
                        Text(current.name.isEmpty ? current.email : current.name)
                            .font(.headline)
                        Text(current.email)
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                }
                .padding(.vertical, 4)
                InfoRow(label: "Region", value: current.region)
                InfoRow(label: "Storefront", value: current.storefront)
                if !current.directoryServicesID.isEmpty {
                    InfoRow(label: "DSID", value: String(current.directoryServicesID.prefix(4)) + "…")
                }
                InfoRow(label: "Password saved", value: current.hasSavedPassword ? "Yes" : "No")
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
                        Label(needsCode ? "Refresh with Code" : "Refresh Token",
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
                Text(current.hasSavedPassword
                     ? "Apple periodically expires the download token. Refresh signs in again with the saved password and stores the new token. IPAShelf also does this automatically when a download reports an expired token."
                     : "This account was added without saving its password, so it cannot be refreshed automatically. Remove it and sign in again.")
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
        .scrollContentBackground(.hidden)
        .background(AppBackground())
        .navigationTitle("Account")
        .navigationBarTitleDisplayMode(.inline)
        .confirmationDialog("Remove this account?",
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
    @Environment(\.dismiss) private var dismiss

    @State private var email = ""
    @State private var password = ""
    @State private var authCode = ""
    @State private var needsCode = false
    @State private var isBusy = false

    private var canSubmit: Bool {
        email.contains("@") && email.contains(".")
            && !password.isEmpty
            && (!needsCode || authCode.count == 6)
            && !isBusy
    }

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    TextField("Apple ID email", text: $email)
                        .keyboardType(.emailAddress)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                        .textContentType(.emailAddress)
                    SecureField("Password", text: $password)
                        .textContentType(.password)
                    if needsCode {
                        TextField("Verification code", text: $authCode)
                            .keyboardType(.numberPad)
                            .textContentType(.oneTimeCode)
                    }
                } header: {
                    Text("Apple ID")
                } footer: {
                    Text(needsCode
                         ? "Enter the six-digit code Apple sent to your trusted devices."
                         : "Credentials are sent only to Apple over SAP-signed requests and stored in the Keychain so the download token can be refreshed later.")
                }

                Section {
                    Button {
                        Task { await submit() }
                    } label: {
                        HStack {
                            Spacer()
                            if isBusy {
                                ProgressView()
                            } else {
                                Text(needsCode ? "Verify and Sign In" : "Sign In")
                                    .font(.headline)
                            }
                            Spacer()
                        }
                    }
                    .disabled(!canSubmit)
                }
            }
            .scrollContentBackground(.hidden)
            .background(AppBackground())
            .navigationTitle("Sign In")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { dismiss() }
                        .disabled(isBusy)
                }
            }
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
        case nil:
            break
        }
    }
}
