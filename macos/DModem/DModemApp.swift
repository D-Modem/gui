// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 D-Modem Authors
//
// DModemApp.swift - the app: its window, menus and the line settings

import SwiftUI

@main
struct DModemApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var delegate
    @StateObject private var modem = Modem()

    var body: some Scene {
        Window("D-Modem", id: "main") {
            ContentView()
                .environmentObject(modem)
                .environmentObject(modem.voice)
                .frame(minWidth: 760, minHeight: 520)
                .onAppear { delegate.modem = modem }
        }
        .commands {
            CommandMenu("Modem") {
                Button(modem.running ? "Stop" : "Start") {
                    modem.running ? modem.stop() : modem.start()
                }
                .keyboardShortcut("r")
                Divider()
                Button("Answer") { modem.answer() }
                    .disabled(modem.state != Int(DMC_RINGING))
                Button("Hang Up") { modem.hangUp() }
                    .keyboardShortcut(".")
                    .disabled(!modem.canHangUp)
            }
        }
        Settings {
            LineSettingsView()
                .environmentObject(modem)
        }
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    weak var modem: Modem?

    func applicationShouldTerminateAfterLastWindowClosed(_ app: NSApplication) -> Bool {
        true
    }

    func applicationWillTerminate(_ notification: Notification) {
        modem?.stop()
    }
}
