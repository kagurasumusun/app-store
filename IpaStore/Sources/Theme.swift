// SPDX-License-Identifier: MIT

import SwiftUI

/// User-selectable accent colors. The default matches the App Store blue.
enum AccentChoice: String, CaseIterable, Identifiable {
    case blue
    case indigo
    case purple
    case pink
    case red
    case orange
    case green
    case teal

    var id: String { rawValue }

    var color: Color {
        switch self {
        case .blue: return Color(red: 0.0, green: 0.478, blue: 1.0)
        case .indigo: return .indigo
        case .purple: return .purple
        case .pink: return .pink
        case .red: return .red
        case .orange: return .orange
        case .green: return Color(red: 0.15, green: 0.68, blue: 0.38)
        case .teal: return .teal
        }
    }

    var title: String {
        switch self {
        case .blue: return String(localized: "Blue")
        case .indigo: return String(localized: "Indigo")
        case .purple: return String(localized: "Purple")
        case .pink: return String(localized: "Pink")
        case .red: return String(localized: "Red")
        case .orange: return String(localized: "Orange")
        case .green: return String(localized: "Green")
        case .teal: return String(localized: "Teal")
        }
    }
}

enum AppearanceChoice: String, CaseIterable, Identifiable {
    case system
    case light
    case dark

    var id: String { rawValue }

    var colorScheme: ColorScheme? {
        switch self {
        case .system: return nil
        case .light: return .light
        case .dark: return .dark
        }
    }

    var title: String {
        switch self {
        case .system: return String(localized: "System")
        case .light: return String(localized: "Light")
        case .dark: return String(localized: "Dark")
        }
    }
}

/// Appearance preferences, persisted in UserDefaults and applied at the root
/// of the view tree (tint + color scheme). Views that need the raw accent
/// color read it from here; everything else follows the SwiftUI tint.
@MainActor
@Observable
final class ThemeStore {
    var accent: AccentChoice {
        didSet { UserDefaults.standard.set(accent.rawValue, forKey: Self.accentKey) }
    }
    var appearance: AppearanceChoice {
        didSet { UserDefaults.standard.set(appearance.rawValue, forKey: Self.appearanceKey) }
    }

    var accentColor: Color { accent.color }
    var colorScheme: ColorScheme? { appearance.colorScheme }

    private static let accentKey = "IPAShelf.accent"
    private static let appearanceKey = "IPAShelf.appearance"

    init() {
        let defaults = UserDefaults.standard
        accent = AccentChoice(rawValue: defaults.string(forKey: Self.accentKey) ?? "") ?? .blue
        appearance = AppearanceChoice(rawValue: defaults.string(forKey: Self.appearanceKey) ?? "") ?? .system
    }
}
