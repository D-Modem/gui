// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 D-Modem Authors
//
// LineSettingsView.swift - the line settings (D-Modem > Settings):
// where the modem's calls go

import SwiftUI
import UniformTypeIdentifiers

struct LineSettingsView: View {
    @EnvironmentObject var modem: Modem
    @State private var c = LineConfig()
    @State private var loaded = false
    @State private var choosing = false

    var body: some View {
        Form {
            Picker("Line", selection: $c.line) {
                ForEach(LineKind.allCases) { Text($0.label).tag($0) }
            }
            .pickerStyle(.radioGroup)

            switch c.line {
            case .sip:
                Section("SIP account") {
                    Toggle("No account (anonymous)", isOn: $c.sipAnonymous)
                    if !c.sipAnonymous {
                        TextField("User", text: $c.sipUser)
                        SecureField("Password", text: $c.sipPassword)
                        TextField("Server", text: $c.sipServer, prompt: Text("sip.example.net"))
                    }
                    Toggle("Take incoming calls", isOn: $c.listen)
                    Text(c.sipAnonymous
                         ? "Calls go straight to a SIP address (sip:user@host, or user@host), from “Anonymous”: nothing is registered, and numbers can’t be dialled."
                         : "A SIP address (sip:user@host) is dialled as it is too; the password only goes to the account’s own server.")
                        .font(.caption).foregroundStyle(.secondary)
                }
                Section("Calls") {
                    Toggle("Fax over T.38", isOn: $c.t38)
                    Toggle("Modem over V.152", isOn: $c.v152)
                    Toggle("Modem relay (V.150.1)", isOn: $c.v150)
                    Picker("Encryption (SRTP)", selection: $c.srtp) {
                        Text("When offered").tag("")
                        Text("Never").tag("no")
                        Text("Required").tag("require")
                    }
                }
            case .usb:
                Section("USB modem") {
                    TextField("Modem", text: $c.usb, prompt: Text("auto"))
                    Text("“auto” takes the first USB voice modem found.")
                        .font(.caption).foregroundStyle(.secondary)
                }
            case .device:
                Section("The modem's device") {
                    TextField("Device", text: $c.device, prompt: Text("/dev/ttyDM0"))
                    Text("A D-Modem that already runs, as a service or by hand.")
                        .font(.caption).foregroundStyle(.secondary)
                }
            }

            Section("The modem") {
                TextField("Country", text: $c.country, prompt: Text("as the modem's default"))
                TextField("Init string", text: $c.initString, prompt: Text("sent each time it opens"))
                    .font(.body.monospaced())
                if c.line != .device {
                    HStack {
                        TextField("dmodemd", text: $c.dmodemd, prompt: Text("looked for"))
                        Button("Choose…") { choosing = true }
                    }
                    Stepper("Debug level: \(c.debug)", value: $c.debug, in: 0...5)
                }
            }

            HStack {
                Spacer()
                Button("Save") {
                    modem.saveConfig(c)
                    if modem.running { modem.say("The new settings apply when the modem starts again") }
                }
                .keyboardShortcut(.defaultAction)
            }
        }
        .formStyle(.grouped)
        .frame(width: 480)
        .fixedSize(horizontal: false, vertical: true)
        .onAppear {
            if !loaded {
                c = modem.config
                loaded = true
            }
        }
        .fileImporter(isPresented: $choosing, allowedContentTypes: [.unixExecutable, .item]) { r in
            if case .success(let url) = r { c.dmodemd = url.path }
        }
    }
}
