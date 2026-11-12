// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 D-Modem Authors
//
// Modem.swift - dmodemd started and stopped, its pty opened, and the
// bytes between the pty, libdmodemctl's session and the terminal screen
//
// dmodemd runs as the user: it makes its pty and says where it is ("Use
// `/dev/ttys003' as modem device"), and the app opens that.  With the
// "device" line nothing is started and the given device is opened.

import AppKit
import Foundation
import UserNotifications

/// A C string from the library, "" for none.
func cstr(_ p: UnsafePointer<CChar>?) -> String {
    p.map { String(cString: $0) } ?? ""
}

/// What a command of the app's got back.
struct Response {
    var ok: Bool
    var text: String      // its lines, apart by "\n"
    var result: String    // OK, ERROR, CONNECT..., timeout
}

/// A choice of a setting's
struct Choice: Hashable {
    var value: String
    var label: String
}

/// One of the modem's settings (AT!M), as the Settings page shows it.
struct Setting: Identifiable {
    var index: Int
    var key: String
    var kind: Int
    var value: String
    var defaultValue: String
    var lo: Int
    var hi: Int
    var group: String
    var label: String
    var help: String
    var choices: [Choice]
    var id: String { key }
    var changed: Bool { value != defaultValue }
}

/// A row of the last call's statistics (AT#UD).
struct DiagRow: Identifiable {
    var id: Int
    var label: String
    var value: String
}

final class Modem: ObservableObject {
    @Published var config = LineConfig.load()
    @Published private(set) var state = Int(DMC_CLOSED)
    @Published private(set) var running = false
    @Published private(set) var device = ""
    @Published private(set) var log: [String] = []
    @Published var toast: String?

    @Published private(set) var settings: [Setting] = []
    @Published private(set) var groups: [String] = []
    @Published private(set) var settingsNote = "Start the modem to see its settings."
    @Published private(set) var initString = ""

    @Published private(set) var diag: [DiagRow] = []
    @Published private(set) var diagNote = "Start the modem to see the last call's statistics."
    @Published private(set) var report = ""

    /// bumped when the terminal screen changed, to draw it again
    @Published private(set) var screenVersion = 0
    /// the Terminal page's kind of terminal (a dmc_vt profile's id)
    @Published var terminalType = UserDefaults.standard.string(forKey: "terminalType") ?? "xterm" {
        didSet {
            dmc_vt_set_profile(vt, terminalType)
            UserDefaults.standard.set(terminalType, forKey: "terminalType")
            screenVersion += 1
        }
    }

    let session: OpaquePointer
    let vt: OpaquePointer
    /// voice calls and the answering machine (Voice.swift)
    let voice = Voice()

    private var process: Process?
    private var fd: Int32 = -1
    private var source: DispatchSourceRead?
    private var pending: [UInt8] = []
    private var waiting: [Int32: (Response) -> Void] = [:]
    private var timer: Timer?
    private var model: OpaquePointer?
    private var modelGen = 0
    private var lastState = Int(DMC_CLOSED)

    init() {
        session = dmc_session_new()
        vt = dmc_vt_new(80, 24, 2000)
        // (the modem takes S5, a backspace, not DEL)
        dmc_vt_set_backspace(vt, 0x08)
        dmc_vt_set_theme(vt, 0xe8f0ff, 0x0a0f1c, 0xe8f0ff, nil)
        dmc_vt_set_profile(vt, terminalType)
        voice.modem = self
        voice.configure()
        let t = Timer(timeInterval: 0.05, repeats: true) { [weak self] _ in
            self?.pump()
        }
        RunLoop.main.add(t, forMode: .common)
        timer = t
    }

    deinit {
        timer?.invalidate()
        dmc_model_free(model)
        dmc_vt_free(vt)
        dmc_session_free(session)
    }

    // MARK: - what the pages use

    var stateName: String {
        running && state == Int(DMC_CLOSED) ? "Starting" : cstr(dmc_state_name(Int32(state)))
    }

    var detail: String {
        switch state {
        case Int(DMC_ONLINE), Int(DMC_ONLINE_COMMAND), Int(DMC_HANGING_UP):
            return cstr(dmc_connect_text(session))
        case Int(DMC_RINGING):
            let n = cstr(dmc_caller_number(session))
            if !n.isEmpty { return n + " " + cstr(dmc_caller_name(session)) }
            return config.summary
        default:
            return config.summary
        }
    }

    var isOpen: Bool { state != Int(DMC_CLOSED) }
    var isIdle: Bool { state == Int(DMC_IDLE) }
    var canHangUp: Bool {
        [DMC_DIALING, DMC_ANSWERING, DMC_ONLINE, DMC_ONLINE_COMMAND, DMC_RINGING,
         DMC_VOICE, DMC_VOICEMAIL]
            .map { Int($0) }.contains(state)
    }

    var callerNumber: String { cstr(dmc_caller_number(session)) }

    func say(_ text: String) {
        toast = text
    }

    func addLog(_ line: String) {
        log.append(line)
        if log.count > 5000 { log.removeFirst(log.count - 5000) }
    }

    // MARK: - commands

    @discardableResult
    func command(_ cmd: String, quiet: Bool = false,
                 _ done: ((Response) -> Void)? = nil) -> Int32 {
        let tag = dmc_command(session, cmd, quiet ? Int32(DMC_QUIET) : 0, 0)
        if tag > 0 {
            waiting[tag] = done ?? { _ in }
            pump()
        }
        return tag
    }

    func dial(_ number: String) {
        let n = number.trimmingCharacters(in: .whitespaces)
        guard !n.isEmpty else { return say("Enter a number to dial") }
        command("ATD" + n) { [weak self] r in
            if !r.ok { self?.say(r.result) }
        }
    }

    func answer() {
        if dmc_answer(session) > 0 { pump() }
    }

    func hangUp() {
        dmc_hangup(session)
        pump()
    }

    /// done gets the response of the session's command tag
    private func expect(_ tag: Int32, _ done: @escaping (Response) -> Void) {
        guard tag > 0 else { return }
        waiting[tag] = done
        pump()
    }

    private func voiceResult(_ r: Response) {
        if !r.ok {
            say(r.result == "ERROR" ? "The modem has no voice mode (D-Modem 1.1 or later has)"
                                    : r.result.capitalized)
        }
    }

    func voiceDial(_ number: String) {
        let n = number.trimmingCharacters(in: .whitespaces)
        guard !n.isEmpty else { return say("Enter a number to call") }
        voice.stopSound()
        expect(dmc_voice_dial(session, n)) { [weak self] r in self?.voiceResult(r) }
    }

    func voiceAnswer() {
        expect(dmc_voice_answer(session)) { [weak self] r in self?.voiceResult(r) }
    }

    func voicemailAnswer() {
        expect(dmc_voicemail_answer(session)) { [weak self] r in self?.voiceResult(r) }
    }

    func pickUp() {
        dmc_voice_pickup(session)
        pump()
    }

    func dtmf(_ key: String) {
        guard let c = key.utf8.first else { return }
        dmc_voice_dtmf(session, CChar(bitPattern: c))
    }

    /// bytes typed in the terminal
    func type(_ bytes: [UInt8]) {
        guard fd >= 0, !bytes.isEmpty else { return }
        bytes.withUnsafeBufferPointer { b in
            b.baseAddress!.withMemoryRebound(to: CChar.self, capacity: b.count) {
                dmc_type(session, $0, Int32(b.count))
            }
        }
        pump()
    }

    func type(_ text: String) { type(Array(text.utf8)) }

    func clearScreen() {
        dmc_vt_reset(vt)
        screenVersion += 1
    }

    // MARK: - the bytes

    private static func now() -> Int64 {
        Int64(DispatchTime.now().uptimeNanoseconds / 1_000_000)
    }

    func pump() {
        voice.beforePoll()
        dmc_poll(session, Modem.now())
        var buf = [CChar](repeating: 0, count: 4096)
        var n: Int32
        while true {
            n = dmc_output(session, &buf, Int32(buf.count))
            if n <= 0 { break }
            pending.append(contentsOf: buf[0..<Int(n)].map { UInt8(bitPattern: $0) })
        }
        flush()
        var fed = false
        while true {
            n = dmc_terminal(session, &buf, Int32(buf.count))
            if n <= 0 { break }
            dmc_vt_feed(vt, buf, n)
            fed = true
        }
        if fed {
            // the screen's answers (where the cursor is), as if typed
            var reply = [CChar](repeating: 0, count: 256)
            while true {
                let k = dmc_vt_reply(vt, &reply, Int32(reply.count))
                if k <= 0 { break }
                dmc_type(session, reply, k)
            }
            TerminalTypes.afterFeed(vt)
        }
        if dmc_vt_changed(vt) != 0 { screenVersion += 1 }
        var ev = dmc_event()
        while dmc_next_event(session, &ev) != 0 {
            let type = Int(ev.type)
            if type == Int(DMC_EV_RESPONSE) {
                let r = Response(ok: ev.ok != 0, text: cstr(ev.text),
                                 result: cstr(ev.result))
                if let fn = waiting.removeValue(forKey: ev.tag) { fn(r) }
            } else {
                event(type, state: Int(ev.state), result: cstr(ev.result),
                      text: cstr(ev.text), ok: ev.ok != 0)
            }
        }
        voice.afterPoll()
    }

    private func flush() {
        while fd >= 0 && !pending.isEmpty {
            let n = pending.withUnsafeBytes { Darwin.write(fd, $0.baseAddress, $0.count) }
            if n > 0 {
                pending.removeFirst(n)
                continue
            }
            if n < 0 && errno == EINTR { continue }
            break           // EAGAIN: the next tick
        }
    }

    private func event(_ type: Int, state st: Int, result: String, text: String, ok: Bool) {
        switch type {
        case Int(DMC_EV_STATE):
            let was = lastState
            lastState = st
            state = st
            voice.sound(st)
            let calls = [DMC_ONLINE, DMC_ONLINE_COMMAND, DMC_HANGING_UP,
                         DMC_DIALING, DMC_ANSWERING].map { Int($0) }
            // a call over: its statistics
            if st == Int(DMC_IDLE) && calls.contains(was) { refreshStats() }
            // a configurator over: the settings it changed
            if st == Int(DMC_IDLE) && was == Int(DMC_CONFIGURATOR) { loadSettings() }
        case Int(DMC_EV_RESULT):
            if result == "RING" {
                ring()
            } else if !result.isEmpty {
                say(result)
            }
        case Int(DMC_EV_CALLER):
            objectWillChange.send()
        case Int(DMC_EV_VOICE), Int(DMC_EV_MESSAGE):
            voice.event(type, text: text, ok: ok)
        default:
            break
        }
    }

    private func ring() {
        guard !NSApp.isActive else { return }
        NSApp.requestUserAttention(.informationalRequest)
        let c = UNMutableNotificationContent()
        c.title = "Incoming call"
        let who = cstr(dmc_caller_number(session))
        c.body = who.isEmpty ? "The modem is ringing" : who
        let center = UNUserNotificationCenter.current()
        center.requestAuthorization(options: [.alert, .sound]) { ok, _ in
            guard ok else { return }
            center.add(UNNotificationRequest(identifier: "ring", content: c, trigger: nil))
        }
    }

    // MARK: - the device

    private func openDevice(_ path: String) -> Bool {
        let f = Darwin.open(path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC)
        if f < 0 {
            let m = "Can't open \(path): \(String(cString: strerror(errno)))"
            addLog(m)
            say(m)
            return false
        }
        var t = termios()
        if tcgetattr(f, &t) == 0 {
            cfmakeraw(&t)
            cfsetspeed(&t, speed_t(B115200))
            t.c_cflag |= tcflag_t(CLOCAL | CREAD)
            tcsetattr(f, TCSANOW, &t)
        }
        fd = f
        device = path
        let s = DispatchSource.makeReadSource(fileDescriptor: f, queue: .main)
        s.setEventHandler { [weak self] in self?.readable() }
        s.resume()
        source = s
        dmc_open(session, Modem.now())
        state = Int(dmc_state(session))
        lastState = state
        // the user's own commands first
        if !config.initString.isEmpty { command(config.initString) }
        loadSettings()
        refreshStats()
        pump()
        return true
    }

    private func readable() {
        var buf = [CChar](repeating: 0, count: 4096)
        while fd >= 0 {
            let n = Darwin.read(fd, &buf, buf.count)
            if n > 0 {
                dmc_feed(session, buf, Int32(n))
                continue
            }
            if n < 0 && errno == EINTR { continue }
            if n < 0 && errno == EAGAIN { break }
            // EOF, or EIO when dmodemd closed its end
            addLog("The modem's device closed.")
            closeDevice()
            return
        }
        pump()
    }

    private func closeDevice() {
        source?.cancel()
        source = nil
        if fd >= 0 {
            Darwin.close(fd)
            fd = -1
        }
        device = ""
        pending.removeAll()
        waiting.removeAll()
        if dmc_state(session) != Int32(DMC_CLOSED) {
            dmc_close(session)
            pump()
        }
        state = Int(DMC_CLOSED)
        lastState = state
        voice.sound(state)
        clearSettings()
        running = process != nil
    }

    // MARK: - dmodemd

    private func findDmodemd() -> String? {
        if !config.dmodemd.isEmpty { return config.dmodemd }
        var places = ["/opt/homebrew/sbin/dmodemd", "/opt/homebrew/bin/dmodemd",
                      "/usr/local/sbin/dmodemd", "/usr/local/bin/dmodemd"]
        if let inApp = Bundle.main.url(forAuxiliaryExecutable: "dmodemd") {
            places.insert(inApp.path, at: 0)
        }
        for dir in (ProcessInfo.processInfo.environment["PATH"] ?? "").split(separator: ":") {
            places.append(dir + "/dmodemd")
        }
        return places.first { FileManager.default.isExecutableFile(atPath: $0) }
    }

    func start() {
        guard !running else { return }
        let c = config
        if c.line == .device {
            running = openDevice(c.device)
            return
        }
        guard let exe = findDmodemd() else {
            return say("dmodemd was not found: install D-Modem, or set its path in the line settings")
        }
        if !c.isSetUp {
            say("Set up the SIP account first")
            if !NSApp.sendAction(Selector(("showSettingsWindow:")), to: nil, from: nil) {
                NSApp.sendAction(Selector(("showPreferencesWindow:")), to: nil, from: nil)
            }
            return
        }
        var args: [String] = []
        if !c.country.isEmpty { args += ["-c", c.country] }
        // (the pty in the user's own group: the default would need root)
        if let g = getgrgid(getgid()) { args += ["-g", String(cString: g.pointee.gr_name)] }
        if c.debug > 0 { args.append("-d\(c.debug)") }
        if c.line == .usb {
            args.append("--usb=\(c.usb)")
        } else if c.listen {
            args.append("-L")
        }
        var env = ProcessInfo.processInfo.environment
        if c.line == .sip {
            // (no account: dmodemd's anonymous user agent)
            env["SIP_LOGIN"] = c.sipAnonymous ? "anonymous" : "\(c.sipUser):\(c.sipPassword)@\(c.sipServer)"
        }
        if !c.t38 { env["SIP_T38"] = "no" }
        if !c.v152 { env["SIP_V152"] = "no" }
        if c.v150 { env["SIP_V150"] = "yes" }
        if !c.srtp.isEmpty { env["SIP_SRTP"] = c.srtp }

        let p = Process()
        p.executableURL = URL(fileURLWithPath: exe)
        p.arguments = args
        p.environment = env
        // its log files where the user may write them
        let caches = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("D-Modem")
        try? FileManager.default.createDirectory(at: caches, withIntermediateDirectories: true)
        p.currentDirectoryURL = caches
        let out = Pipe()
        p.standardOutput = out
        p.standardError = out
        p.standardInput = FileHandle.nullDevice
        var partial = Data()
        out.fileHandleForReading.readabilityHandler = { [weak self] h in
            let d = h.availableData
            if d.isEmpty {
                h.readabilityHandler = nil
                return
            }
            partial.append(d)
            var lines: [String] = []
            while let nl = partial.firstIndex(of: 0x0A) {
                lines.append(String(decoding: partial[partial.startIndex..<nl], as: UTF8.self))
                partial.removeSubrange(partial.startIndex...nl)
            }
            if lines.isEmpty { return }
            DispatchQueue.main.async { lines.forEach { self?.daemonLine($0) } }
        }
        p.terminationHandler = { [weak self] q in
            DispatchQueue.main.async { self?.exited(q) }
        }
        do {
            try p.run()
        } catch {
            let m = "Can't start dmodemd: \(error.localizedDescription)"
            addLog(m)
            return say(m)
        }
        process = p
        running = true
        addLog("$ " + ([exe] + args).joined(separator: " "))
    }

    private func daemonLine(_ raw: String) {
        let line = raw.trimmingCharacters(in: CharacterSet(charactersIn: "\r"))
        addLog(line)
        var dev = [CChar](repeating: 0, count: 512)
        if dmc_daemon_ready(line, &dev, Int32(dev.count)) != 0 && fd < 0 {
            _ = openDevice(String(cString: dev))
        } else if dmc_daemon_error(line) != 0 && fd < 0 &&
                    !line.contains("group") && !line.contains("symbolic link") {
            say(String(line.dropFirst(7)))
        }
    }

    private func exited(_ p: Process) {
        let m = p.terminationReason == .uncaughtSignal
            ? "dmodemd stopped (signal \(p.terminationStatus))."
            : "dmodemd stopped (exit status \(p.terminationStatus))."
        addLog(m)
        guard p === process else { return }
        process = nil
        closeDevice()
        running = false
        if !(p.terminationReason == .uncaughtSignal && p.terminationStatus == SIGTERM) {
            say(m)
        }
    }

    func stop() {
        if fd >= 0 && process != nil {
            // hang up first, so the call ends cleanly
            dmc_hangup(session)
            pump()
        }
        closeDevice()
        process?.terminate()
        running = process != nil
    }

    func saveConfig(_ c: LineConfig) {
        config = c
        c.save()
    }

    // MARK: - the settings (AT!M)

    private func clearSettings() {
        modelGen += 1
        dmc_model_free(model)
        model = nil
        settings = []
        groups = []
        initString = ""
        settingsNote = "Start the modem to see its settings."
    }

    private func snapshot(_ i: Int) -> Setting {
        let m = model, n = Int32(i)
        var choices: [Choice] = []
        for c in 0..<dmc_setting_choices(m, n) {
            choices.append(Choice(value: cstr(dmc_choice_value(m, n, c)),
                                  label: cstr(dmc_choice_label(m, n, c))))
        }
        return Setting(index: i, key: cstr(dmc_setting_key(m, n)),
                       kind: Int(dmc_setting_kind(m, n)),
                       value: cstr(dmc_setting_value(m, n)),
                       defaultValue: cstr(dmc_setting_default(m, n)),
                       lo: Int(dmc_setting_lo(m, n)), hi: Int(dmc_setting_hi(m, n)),
                       group: cstr(dmc_setting_group(m, n)),
                       label: cstr(dmc_setting_label(m, n)),
                       help: cstr(dmc_setting_help(m, n)), choices: choices)
    }

    func loadSettings() {
        guard isOpen else { return }
        command("AT!M", quiet: true) { [weak self] r in
            guard let self else { return }
            guard r.ok else {
                self.settingsNote = "This modem does not list its settings (AT!M): update D-Modem, or use the Terminal's setup screen."
                return
            }
            guard let m = dmc_model_parse(r.text) else {
                self.settingsNote = "The modem's settings list was not readable."
                return
            }
            self.clearSettings()
            self.model = m
            self.groups = (0..<dmc_model_groups(m)).map { cstr(dmc_model_group(m, $0)) }
            self.settings = (0..<Int(dmc_model_count(m))).map { self.snapshot($0) }
            self.settingsNote = ""
            self.command("AT!R", quiet: true) { [weak self] r in
                if r.ok { self?.initString = Modem.initLine(r.text) }
            }
        }
    }

    /// AT!R's lines, as one line of commands
    static func initLine(_ text: String) -> String {
        var s = ""
        for raw in text.split(separator: "\n") {
            let l = raw.trimmingCharacters(in: .whitespaces)
            if l.isEmpty || l.uppercased() == "AT" { continue }
            s += s.isEmpty ? l : String(l.dropFirst(2))
        }
        return s
    }

    /// setting i set to value: checked, sent, and kept after the OK
    func apply(_ i: Int, _ value: String) {
        guard let m = model, i < settings.count, settings[i].value != value else { return }
        let why = cstr(dmc_setting_check(m, Int32(i), value))
        if !why.isEmpty {
            objectWillChange.send()     // the control back as it was
            return say(why)
        }
        let gen = modelGen
        let cmd = cstr(dmc_setting_command(m, Int32(i), value))
        let tag = command(cmd) { [weak self] r in
            guard let self, gen == self.modelGen, let m = self.model else { return }
            if r.ok {
                dmc_setting_set(m, Int32(i), value)
                self.settings[i] = self.snapshot(i)
                self.command("AT!R", quiet: true) { [weak self] r in
                    if r.ok { self?.initString = Modem.initLine(r.text) }
                }
            } else {
                self.objectWillChange.send()
                self.say("The modem refused \(cmd)")
            }
        }
        if tag < 0 {
            objectWillChange.send()
            say("The modem is busy")
        }
    }

    func resetSettings() {
        command("ATZ") { [weak self] r in
            if !r.ok { self?.say("The modem refused ATZ") }
            self?.loadSettings()
        }
    }

    func keepInit() {
        var c = config
        c.initString = initString
        saveConfig(c)
        say(initString.isEmpty ? "Nothing to send: every setting is at its default"
                               : "Sent each time the modem opens")
    }

    // MARK: - the call's statistics

    func refreshStats() {
        guard isOpen else { return }
        command("AT#UD", quiet: true) { [weak self] r in
            guard let self else { return }
            let d = dmc_diag_parse(r.ok ? r.text : "")
            defer { dmc_diag_free(d) }
            self.diag = (0..<dmc_diag_count(d)).map {
                DiagRow(id: Int(dmc_diag_key(d, $0)), label: cstr(dmc_diag_label(d, $0)),
                        value: cstr(dmc_diag_value(d, $0)))
            }
            self.diagNote = !r.ok ? "This modem does not report statistics (AT#UD)."
                : dmc_diag_has_call(d) == 0 ? "No call yet." : ""
        }
        command("ATI11", quiet: true) { [weak self] r in
            self?.report = r.ok ? r.text : ""
        }
    }
}
