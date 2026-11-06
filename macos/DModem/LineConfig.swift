// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, D-Modem
//
// LineConfig.swift - the line settings: where dmodemd's calls go, kept in
// the user defaults (the SIP password in the keychain)

import Foundation
import Security

enum LineKind: String, CaseIterable, Identifiable {
    case sip, usb, device
    var id: String { rawValue }
    var label: String {
        switch self {
        case .sip: return "SIP account"
        case .usb: return "USB modem"
        case .device: return "A modem that already runs"
        }
    }
}

struct LineConfig {
    var line: LineKind = .sip
    var sipUser = ""
    var sipPassword = ""
    var sipServer = ""
    var listen = false
    var t38 = true
    var v152 = true
    var v150 = false
    var srtp = ""           // "", "no" or "require"
    var country = ""
    var usb = "auto"
    var device = "/dev/ttyDM0"
    var dmodemd = ""        // its path, "" to look for it
    var initString = ""     // commands sent when the modem opens
    var debug = 0

    private static let defaults = UserDefaults.standard

    static func load() -> LineConfig {
        var c = LineConfig()
        let d = defaults
        c.line = LineKind(rawValue: d.string(forKey: "line") ?? "") ?? .sip
        c.sipUser = d.string(forKey: "sipUser") ?? ""
        c.sipServer = d.string(forKey: "sipServer") ?? ""
        c.listen = d.bool(forKey: "listen")
        c.t38 = d.object(forKey: "t38") as? Bool ?? true
        c.v152 = d.object(forKey: "v152") as? Bool ?? true
        c.v150 = d.bool(forKey: "v150")
        c.srtp = d.string(forKey: "srtp") ?? ""
        c.country = d.string(forKey: "country") ?? ""
        c.usb = d.string(forKey: "usb") ?? "auto"
        c.device = d.string(forKey: "device") ?? "/dev/ttyDM0"
        c.dmodemd = d.string(forKey: "dmodemd") ?? ""
        c.initString = d.string(forKey: "init") ?? ""
        c.debug = d.integer(forKey: "debug")
        c.sipPassword = Keychain.password(for: c.sipUser, server: c.sipServer) ?? ""
        return c
    }

    func save() {
        let d = LineConfig.defaults
        d.set(line.rawValue, forKey: "line")
        d.set(sipUser, forKey: "sipUser")
        d.set(sipServer, forKey: "sipServer")
        d.set(listen, forKey: "listen")
        d.set(t38, forKey: "t38")
        d.set(v152, forKey: "v152")
        d.set(v150, forKey: "v150")
        d.set(srtp, forKey: "srtp")
        d.set(country, forKey: "country")
        d.set(usb, forKey: "usb")
        d.set(device, forKey: "device")
        d.set(dmodemd, forKey: "dmodemd")
        d.set(initString, forKey: "init")
        d.set(debug, forKey: "debug")
        Keychain.setPassword(sipPassword, for: sipUser, server: sipServer)
    }

    var summary: String {
        switch line {
        case .usb: return "USB modem (\(usb))"
        case .device: return "The modem at \(device)"
        case .sip:
            if sipUser.isEmpty || sipServer.isEmpty { return "SIP account not set up" }
            return "SIP: \(sipUser)@\(sipServer)" + (listen ? ", taking calls" : "")
        }
    }

    var isSetUp: Bool {
        line != .sip || (!sipUser.isEmpty && !sipServer.isEmpty)
    }
}

/// The SIP password, as an internet password in the login keychain.
enum Keychain {
    private static func query(_ user: String, _ server: String) -> [String: Any] {
        [kSecClass as String: kSecClassInternetPassword,
         kSecAttrServer as String: server,
         kSecAttrAccount as String: user]
    }

    static func password(for user: String, server: String) -> String? {
        guard !user.isEmpty, !server.isEmpty else { return nil }
        var q = query(user, server)
        q[kSecReturnData as String] = true
        q[kSecMatchLimit as String] = kSecMatchLimitOne
        var out: CFTypeRef?
        guard SecItemCopyMatching(q as CFDictionary, &out) == errSecSuccess,
              let data = out as? Data else { return nil }
        return String(data: data, encoding: .utf8)
    }

    static func setPassword(_ password: String, for user: String, server: String) {
        guard !user.isEmpty, !server.isEmpty else { return }
        let q = query(user, server)
        SecItemDelete(q as CFDictionary)
        guard !password.isEmpty else { return }
        var add = q
        add[kSecValueData as String] = Data(password.utf8)
        add[kSecAttrLabel as String] = "D-Modem SIP account"
        SecItemAdd(add as CFDictionary, nil)
    }
}
