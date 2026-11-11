// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 D-Modem Authors
//
// ContentView.swift - the window: its pages (Modem, Terminal, Settings,
// Call Stats, Log) in a sidebar

import SwiftUI

enum Page: String, CaseIterable, Identifiable {
    case modem = "Modem", phone = "Phone", terminal = "Terminal", settings = "Settings"
    case stats = "Call Stats", log = "Log"
    var id: String { rawValue }
    var icon: String {
        switch self {
        case .modem: return "phone.connection"
        case .phone: return "phone"
        case .terminal: return "terminal"
        case .settings: return "slider.horizontal.3"
        case .stats: return "chart.bar"
        case .log: return "doc.plaintext"
        }
    }
}

struct ContentView: View {
    @EnvironmentObject var modem: Modem
    @State private var page: Page? = Page(rawValue: ProcessInfo.processInfo.environment["DMODEM_GUI_PAGE"] ?? "") ?? .modem

    var body: some View {
        NavigationSplitView {
            List(Page.allCases, selection: $page) { p in
                Label(p.rawValue, systemImage: p.icon).tag(p)
            }
            .navigationSplitViewColumnWidth(170)
        } detail: {
            Group {
                switch page ?? .modem {
                case .modem: ModemPage()
                case .phone: PhonePage()
                case .terminal: TerminalPane()
                case .settings: SettingsPage()
                case .stats: StatsPage()
                case .log: LogPage()
                }
            }
            .navigationTitle(page?.rawValue ?? "D-Modem")
        }
        .toolbar {
            ToolbarItem(placement: .primaryAction) {
                Button(modem.running ? "Stop" : "Start") {
                    modem.running ? modem.stop() : modem.start()
                }
            }
        }
        .overlay(alignment: .bottom) {
            if let t = modem.toast {
                Text(t)
                    .padding(.horizontal, 14).padding(.vertical, 8)
                    .background(.regularMaterial, in: Capsule())
                    .padding(16)
                    .transition(.opacity)
                    .task(id: t) {
                        try? await Task.sleep(nanoseconds: 4_000_000_000)
                        if modem.toast == t { modem.toast = nil }
                    }
            }
        }
        .animation(.default, value: modem.toast)
    }
}

// MARK: - Modem

struct ModemPage: View {
    @EnvironmentObject var modem: Modem
    @State private var number = ""

    private var icon: String {
        guard modem.running else { return "phone.down" }
        switch modem.state {
        case Int(DMC_RINGING): return "phone.arrow.down.left"
        case Int(DMC_DIALING), Int(DMC_ANSWERING): return "phone.arrow.up.right"
        case Int(DMC_ONLINE), Int(DMC_ONLINE_COMMAND): return "arrow.up.arrow.down"
        case Int(DMC_CONFIGURATOR): return "gearshape"
        case Int(DMC_VOICE): return "phone.fill"
        case Int(DMC_VOICEMAIL): return "recordingtape"
        default: return "phone"
        }
    }

    var body: some View {
        Form {
            Section {
                HStack(spacing: 16) {
                    Image(systemName: icon)
                        .font(.system(size: 36))
                        .frame(width: 56)
                    VStack(alignment: .leading, spacing: 4) {
                        Text(modem.stateName).font(.title.bold())
                        Text(modem.detail).foregroundStyle(.secondary).textSelection(.enabled)
                    }
                }
                .padding(.vertical, 8)
            }
            Section("Call") {
                HStack {
                    TextField("Number", text: $number)
                        .onSubmit { if modem.isIdle { modem.dial(number) } }
                    Button("Dial") { modem.dial(number) }
                        .disabled(!modem.isIdle)
                }
                .disabled(!modem.isOpen)
                HStack {
                    Button("Answer") { modem.answer() }
                        .disabled(modem.state != Int(DMC_RINGING))
                    Button("Hang Up", role: .destructive) { modem.hangUp() }
                        .disabled(!modem.canHangUp)
                }
            }
            Section("Line") {
                LabeledContent("Line", value: modem.config.summary)
                LabeledContent("Modem device") {
                    HStack {
                        Text(modem.device.isEmpty ? "Not running" : modem.device)
                            .textSelection(.enabled)
                        if !modem.device.isEmpty {
                            Button {
                                NSPasteboard.general.clearContents()
                                NSPasteboard.general.setString(modem.device, forType: .string)
                            } label: { Image(systemName: "doc.on.doc") }
                            .buttonStyle(.borderless)
                            .help("Copy, for another program to use the modem")
                        }
                    }
                }
                HStack {
                    if #available(macOS 14, *) {
                        SettingsLink { Text("Line Settings…") }
                    } else {
                        Button("Line Settings…") {
                            NSApp.sendAction(Selector(("showPreferencesWindow:")), to: nil, from: nil)
                        }
                    }
                    Spacer()
                    Button(modem.running ? "Stop" : "Start") {
                        modem.running ? modem.stop() : modem.start()
                    }
                    .keyboardShortcut(.defaultAction)
                }
            }
        }
        .formStyle(.grouped)
    }
}

// MARK: - Settings

struct SettingsPage: View {
    @EnvironmentObject var modem: Modem

    var body: some View {
        Form {
            Section {
                LabeledContent("Init string") {
                    HStack {
                        Text(modem.initString.isEmpty ? "AT (all at their defaults)" : modem.initString)
                            .font(.body.monospaced())
                            .textSelection(.enabled)
                        Button("Keep") { modem.keepInit() }
                            .help("Send it each time the modem opens")
                    }
                }
            } header: {
                HStack {
                    Text("The modem's settings")
                    Spacer()
                    Button { modem.loadSettings() } label: { Image(systemName: "arrow.clockwise") }
                        .buttonStyle(.borderless)
                        .help("Read the settings again")
                    Button("Defaults") { modem.resetSettings() }
                        .buttonStyle(.borderless)
                        .help("Every setting back to its default (ATZ)")
                }
            } footer: {
                Text("Changes take effect at once. The modem keeps no stored profile, so keep the init string to have them each time.")
            }
            .disabled(!modem.isIdle)

            if !modem.settingsNote.isEmpty {
                Text(modem.settingsNote).foregroundStyle(.secondary)
            }
            ForEach(modem.groups, id: \.self) { g in
                Section(g) {
                    ForEach(modem.settings.filter { $0.group == g }) { s in
                        SettingRow(s: s)
                    }
                }
                .disabled(!modem.isIdle)
            }
        }
        .formStyle(.grouped)
    }
}

struct SettingRow: View {
    @EnvironmentObject var modem: Modem
    let s: Setting
    @State private var text = ""

    private var title: some View {
        Text(s.label).fontWeight(s.changed ? .semibold : .regular)
    }

    var body: some View {
        Group {
            switch s.kind {
            case Int(DMC_PICK):
                Picker(selection: Binding(get: { s.value }, set: { modem.apply(s.index, $0) })) {
                    ForEach(s.choices, id: \.value) { Text($0.label).tag($0.value) }
                    if !s.choices.contains(where: { $0.value == s.value }) {
                        Text(s.value).tag(s.value)
                    }
                } label: { title }
            case Int(DMC_NUMBER):
                LabeledContent {
                    HStack {
                        TextField("", text: $text)
                            .frame(width: 80)
                            .multilineTextAlignment(.trailing)
                            .onSubmit { modem.apply(s.index, text) }
                        Stepper("", value: Binding(
                            get: { Int(s.value) ?? s.lo },
                            set: { modem.apply(s.index, String($0)) }),
                                in: s.lo...max(s.lo, s.hi))
                        .labelsHidden()
                    }
                } label: { title }
            default:
                LabeledContent {
                    TextField("", text: $text)
                        .onSubmit { modem.apply(s.index, text) }
                } label: { title }
            }
        }
        .help(s.help)
        .onAppear { text = s.value }
        .onChange(of: s.value) { text = $0 }
    }
}

// MARK: - Call Stats

struct StatsPage: View {
    @EnvironmentObject var modem: Modem

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack {
                Text("The last call, as the modem reports it (AT#UD), after each call.")
                    .foregroundStyle(.secondary)
                Spacer()
                Button { modem.refreshStats() } label: { Image(systemName: "arrow.clockwise") }
                    .disabled(!modem.isIdle)
                    .help("Ask the modem again")
            }
            .padding(12)
            if !modem.diagNote.isEmpty {
                Text(modem.diagNote).foregroundStyle(.secondary).padding(.horizontal, 12)
            }
            Table(modem.diag) {
                TableColumn("Statistic", value: \.label)
                TableColumn("Value") { r in Text(r.value).textSelection(.enabled) }
            }
            .frame(minHeight: 180)
            Text("The modem's report (ATI11)").font(.headline).padding(12)
            ScrollView {
                Text(modem.report.isEmpty ? " " : modem.report)
                    .font(.body.monospaced())
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(12)
            }
            .frame(minHeight: 120)
        }
    }
}

// MARK: - Log

struct LogPage: View {
    @EnvironmentObject var modem: Modem

    var body: some View {
        ScrollViewReader { proxy in
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 1) {
                    ForEach(Array(modem.log.enumerated()), id: \.offset) { i, line in
                        Text(line).font(.callout.monospaced()).id(i)
                    }
                }
                .textSelection(.enabled)
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(12)
            }
            .onChange(of: modem.log.count) { n in
                if n > 0 { proxy.scrollTo(n - 1, anchor: .bottom) }
            }
        }
    }
}
