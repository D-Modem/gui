// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 D-Modem Authors
//
// RemoteView.swift - the Remote page: a login to a server by SSH, Telnet
// or rlogin, in a terminal of its own (TerminalView.swift's view)
//
// Telnet and rlogin are a socket made here and libdmodemctl's dmc_net
// for the protocol. SSH is libdmodemssh (the apps' own client, on
// libssh) through SSHConnection.swift: it reads OpenSSH's config,
// known_hosts, keys and agent, so that they are the user's.

import Darwin
import Foundation
import SwiftUI

final class RemoteSession: ObservableObject, TerminalHost {
    enum Proto: String, CaseIterable, Identifiable {
        case ssh = "SSH", telnet = "Telnet", rlogin = "rlogin"
        var id: String { rawValue }
        var net: Int32 {
            switch self {
            case .ssh: return Int32(DMC_NET_SSH)
            case .telnet: return Int32(DMC_NET_TELNET)
            case .rlogin: return Int32(DMC_NET_RLOGIN)
            }
        }
        var defaultPort: Int { Int(dmc_net_default_port(net)) }
    }

    @Published var proto: Proto
    @Published var host: String
    /// empty: the protocol's
    @Published var port: String
    @Published var user: String
    @Published private(set) var running = false
    @Published private(set) var status = "Not connected"
    /// bumped when the screen changed, to draw it again
    @Published private(set) var screenVersion = 0
    /// what the SSH login asks the user, if anything
    @Published var question: Question?

    /// an SSH login's question
    enum Question {
        /// a server not known yet: its key type and fingerprint
        case newHost(type: String, fingerprint: String)
        /// a password; why the last was refused
        case password(why: String)
        /// the server's questions (keyboard-interactive): each with
        /// whether its answer may be shown
        case prompts(title: String, instruction: String, questions: [(String, Bool)])
    }

    /// the kind of terminal (a dmc_vt profile's id): the far end is told
    /// it when connecting
    @Published var terminalType = UserDefaults.standard.string(forKey: "remoteTerminal") ?? "xterm" {
        didSet {
            dmc_vt_set_profile(vt, terminalType)
            defaults.set(terminalType, forKey: "remoteTerminal")
            screenVersion += 1
        }
    }

    let vt: OpaquePointer
    private var fd: Int32 = -1          // Telnet's and rlogin's socket
    private var source: DispatchSourceRead?
    private var writer: DispatchSourceWrite?
    private var net: OpaquePointer?
    /// SSH: the connection, the shell's channel, the logins tried
    private var ssh: SSHConnection?
    private var shell: Int32 = 0
    private var triedKeys = false
    private var asks = 0
    private var exitStatus = 0
    private var pending: [UInt8] = []
    private var cols = 80, rows = 24
    private var timer: Timer?
    /// a connection under way: older ones' results ignored
    private var gen = 0

    private let defaults = UserDefaults.standard

    init() {
        vt = dmc_vt_new(80, 24, 2000)
        // (Backspace is DEL to a host, not the modem's BS)
        dmc_vt_set_backspace(vt, 0x7f)
        dmc_vt_set_theme(vt, 0xe8f0ff, 0x0a0f1c, 0xe8f0ff, nil)
        proto = Proto(rawValue: defaults.string(forKey: "remoteProtocol") ?? "") ?? .ssh
        host = defaults.string(forKey: "remoteHost") ?? ""
        port = defaults.string(forKey: "remotePort") ?? ""
        user = defaults.string(forKey: "remoteUser") ?? ""
        dmc_vt_set_profile(vt, terminalType)
    }

    deinit {
        stop()
        dmc_vt_free(vt)
    }

    // MARK: - TerminalHost

    func send(_ bytes: [UInt8]) {
        guard running, !bytes.isEmpty else { return }
        if let ssh {
            if shell > 0 { ssh.write(shell, bytes) }
            return
        }
        guard fd >= 0 else { return }
        if let net {
            bytes.withUnsafeBufferPointer { b in
                b.baseAddress!.withMemoryRebound(to: CChar.self, capacity: b.count) {
                    dmc_net_type(net, $0, Int32(b.count))
                }
            }
        } else {
            pending.append(contentsOf: bytes)
        }
        pump()
    }

    // MARK: - connecting

    func connect() {
        if running { return }
        let h = host.trimmingCharacters(in: .whitespaces)
        guard !h.isEmpty else { status = "Enter the server to connect to"; return }
        let p = port.isEmpty ? proto.defaultPort : (Int(port) ?? 0)
        guard p > 0 && p <= 65535 else { status = "The port is a number from 1 to 65535"; return }
        if proto == .rlogin && user.isEmpty { status = "rlogin needs a user name"; return }
        defaults.set(proto.rawValue, forKey: "remoteProtocol")
        defaults.set(h, forKey: "remoteHost")
        defaults.set(port, forKey: "remotePort")
        defaults.set(user, forKey: "remoteUser")

        dmc_vt_reset(vt)
        cols = Int(dmc_vt_cols(vt))
        rows = Int(dmc_vt_rows(vt))
        let what = "Connecting to \(h) port \(p) by \(proto.rawValue)"
        status = what
        show("[\(what)]\r\n")
        gen += 1
        running = true
        if proto == .ssh {
            guard startSSH(host: h, port: p) else {
                ended("Could not start SSH")
                return
            }
        } else {
            // (Telnet's TERMINAL-TYPE, rlogin's TERM)
            let type = proto == .telnet ? dmc_vt_profile_telnet(terminalType) : dmc_vt_profile_term(terminalType)
            net = dmc_net_new(proto.net, user, type, Int32(cols), Int32(rows))
            connectSocket(host: h, port: p, gen: gen)
        }
        let t = Timer(timeInterval: 0.25, repeats: true) { [weak self] _ in self?.tick() }
        RunLoop.main.add(t, forMode: .common)
        timer = t
    }

    func stop() { ended(nil) }

    // MARK: - SSH

    private func startSSH(host: String, port: Int) -> Bool {
        let u = user.isEmpty ? NSUserName() : user
        guard let c = SSHConnection(host: host, port: port, user: u) else { return false }
        ssh = c
        shell = 0
        triedKeys = false
        asks = 0
        exitStatus = 0
        c.start { [weak self, weak c] ev in
            guard let self, let c else { return }
            self.sshEvent(c, ev)
        }
        return true
    }

    /// an event of the connection's, on the main queue
    private func sshEvent(_ c: SSHConnection, _ ev: SSHConnection.Event) {
        if ev.type == DMC_SSH_EV_CLOSED {
            c.finish()
            guard c === ssh else { return }
            ssh = nil
            question = nil
            if ev.data == nil && exitStatus > 0 {
                ended("the shell exited with status \(exitStatus)")
            } else {
                ended(ev.text)
            }
            return
        }
        guard c === ssh else { return }
        switch ev.type {
        case DMC_SSH_EV_HOSTKEY:
            hostKey(c, ev.text ?? "", Int(ev.value))
        case DMC_SSH_EV_AUTH:
            login(c, methods: Int(ev.value), why: ev.text ?? "")
        case DMC_SSH_EV_PROMPTS:
            if ev.value == 0 {
                dmc_ssh_answered(c.s)
            } else {
                var qs: [(String, Bool)] = []
                for i in 0..<Int(ev.value) {
                    var echo: Int32 = 0
                    let q = String(cString: dmc_ssh_prompt(c.s, Int32(i), &echo))
                    qs.append((q, echo != 0))
                }
                let t = ev.text ?? ""
                question = .prompts(title: t.isEmpty ? "The server asks" : t,
                                    instruction: String(cString: dmc_ssh_instruction(c.s)),
                                    questions: qs)
            }
        case DMC_SSH_EV_READY:
            if let d = ev.data, !d.isEmpty { feed(d) }
            shell = dmc_ssh_shell(c.s, dmc_vt_profile_term(terminalType), Int32(cols), Int32(rows), 0)
            status = "Connected by SSH"
        case DMC_SSH_EV_OPEN:
            if ev.value == 0 {
                show("\r\n[The server refused the shell]\r\n")
                c.disconnect()
            }
        case DMC_SSH_EV_DATA:
            if ev.channel == shell, let d = ev.data {
                feed(d)
                pump()
            }
        case DMC_SSH_EV_EXIT:
            if ev.channel == shell { exitStatus = Int(ev.value) }
        case DMC_SSH_EV_CLOSE:
            // (the shell's over: so is the connection)
            if ev.channel == shell { c.disconnect() }
        case DMC_SSH_EV_INCOMING:
            dmc_ssh_close(c.s, ev.channel)
        default:
            break
        }
    }

    /// the server's key: known goes on; new is asked; changed is refused
    private func hostKey(_ c: SSHConnection, _ key: String, _ known: Int) {
        if known == DMC_SSH_HOST_KNOWN {
            dmc_ssh_trust(c.s, 1, 0)
            return
        }
        var fp = [CChar](repeating: 0, count: 128)
        dmc_ssh_fingerprint(key, &fp, Int32(fp.count))
        let type = String(key.prefix { $0 != " " })
        let print = String(cString: fp)
        if known == DMC_SSH_HOST_CHANGED || known == DMC_SSH_HOST_OTHER_TYPE {
            show("\r\n\u{1b}[1;31mThe server's host key has changed (it is now \(type) \(print)).\r\n" +
                 "Someone may be in the middle; or the server has a new key. If that is\r\n" +
                 "expected, remove its old line from ~/.ssh/known_hosts.\u{1b}[0m\r\n")
            dmc_ssh_trust(c.s, 0, 0)
            return
        }
        question = .newHost(type: type, fingerprint: print)
    }

    /// what's tried, in OpenSSH's order: its keys and agent, then the
    /// server's questions or a password (three times at most)
    private func login(_ c: SSHConnection, methods: Int, why: String) {
        if methods & DMC_SSH_AUTH_PUBLICKEY != 0 && !triedKeys {
            triedKeys = true
            dmc_ssh_auth_auto(c.s)
            return
        }
        asks += 1
        if asks > 3 {
            dmc_ssh_auth_cancel(c.s)
        } else if methods & DMC_SSH_AUTH_INTERACTIVE != 0 {
            dmc_ssh_auth_interactive(c.s)
        } else if methods & DMC_SSH_AUTH_PASSWORD != 0 {
            question = .password(why: asks > 1 ? why : "")
        } else {
            dmc_ssh_auth_cancel(c.s)
        }
    }

    /// the user's answer to [question]: the answers (none for a new
    /// host's yes), or nil for no
    func answer(_ answers: [String]?) {
        guard let q = question, let c = ssh else { return }
        question = nil
        guard let answers else {
            if case .newHost = q { dmc_ssh_trust(c.s, 0, 0) } else { dmc_ssh_auth_cancel(c.s) }
            return
        }
        switch q {
        case .newHost:
            dmc_ssh_trust(c.s, 1, 1)
        case .password:
            dmc_ssh_auth_password(c.s, answers.first ?? "")
        case .prompts:
            for (i, a) in answers.enumerated() { dmc_ssh_answer(c.s, Int32(i), a) }
            dmc_ssh_answered(c.s)
        }
    }

    private func feed(_ d: [UInt8]) {
        d.withUnsafeBufferPointer { b in
            b.baseAddress!.withMemoryRebound(to: CChar.self, capacity: b.count) {
                dmc_vt_feed(vt, $0, Int32(b.count))
            }
        }
    }

    // MARK: - Telnet and rlogin

    private func connectSocket(host: String, port: Int, gen: Int) {
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            var hints = addrinfo()
            hints.ai_family = AF_UNSPEC
            hints.ai_socktype = SOCK_STREAM
            var res: UnsafeMutablePointer<addrinfo>?
            var s: Int32 = -1
            var err = ""
            let rc = getaddrinfo(host, String(port), &hints, &res)
            if rc != 0 {
                err = String(cString: gai_strerror(rc))
            } else {
                var a = res
                while let ai = a {
                    let k = socket(ai.pointee.ai_family, ai.pointee.ai_socktype, ai.pointee.ai_protocol)
                    if k >= 0 {
                        if Darwin.connect(k, ai.pointee.ai_addr, ai.pointee.ai_addrlen) == 0 {
                            s = k
                            break
                        }
                        err = String(cString: strerror(errno))
                        close(k)
                    }
                    a = ai.pointee.ai_next
                }
                freeaddrinfo(res)
            }
            DispatchQueue.main.async {
                guard let self, self.gen == gen, self.running else {
                    if s >= 0 { close(s) }
                    return
                }
                self.socketConnected(s, err)
            }
        }
    }

    private func socketConnected(_ s: Int32, _ err: String) {
        guard s >= 0, let net else {
            ended(err.isEmpty ? "Could not connect" : err)
            return
        }
        var one: Int32 = 1
        setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, socklen_t(MemoryLayout<Int32>.size))
        _ = fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK)
        fd = s
        watch()
        status = "Connected"
        dmc_net_connected(net)
        pump()
    }

    private func watch() {
        let s = DispatchSource.makeReadSource(fileDescriptor: fd, queue: .main)
        s.setEventHandler { [weak self] in self?.readable() }
        s.resume()
        source = s
    }

    // MARK: - the bytes

    /// rlogin's control byte, sent urgent
    private func urgent() {
        guard let net, fd >= 0 else { return }
        var b: UInt8 = 0
        if recv(fd, &b, 1, Int32(MSG_OOB)) == 1 { dmc_net_urgent(net, Int32(b)) }
    }

    private func readable() {
        guard fd >= 0 else { return }
        urgent()
        var buf = [CChar](repeating: 0, count: 8192)
        while fd >= 0 {
            let n = Darwin.read(fd, &buf, buf.count)
            if n > 0 {
                if let net {
                    dmc_net_feed(net, buf, Int32(n))
                } else {
                    dmc_vt_feed(vt, buf, Int32(n))
                }
                continue
            }
            if n < 0 && errno == EINTR { continue }
            if n < 0 && errno == EAGAIN { break }
            // the end: the server closed
            pump()
            ended("Connection closed by the server")
            return
        }
        pump()
    }

    private func pump() {
        var buf = [CChar](repeating: 0, count: 8192)
        if let net {
            while true {
                let n = dmc_net_screen(net, &buf, Int32(buf.count))
                if n <= 0 { break }
                dmc_vt_feed(vt, buf, n)
            }
        }
        TerminalTypes.afterFeed(vt)
        // the screen's answers (where the cursor is), as if typed
        while true {
            let k = dmc_vt_reply(vt, &buf, Int32(buf.count))
            if k <= 0 { break }
            if let net {
                dmc_net_type(net, buf, k)
            } else if let ssh, shell > 0 {
                ssh.write(shell, buf[0..<Int(k)].map { UInt8(bitPattern: $0) })
            }
        }
        if let net {
            while true {
                let n = dmc_net_output(net, &buf, Int32(buf.count))
                if n <= 0 { break }
                pending.append(contentsOf: buf[0..<Int(n)].map { UInt8(bitPattern: $0) })
            }
        }
        flush()
        if dmc_vt_changed(vt) != 0 { screenVersion += 1 }
    }

    private func flush() {
        while fd >= 0 && !pending.isEmpty {
            let w = pending.withUnsafeBytes { Darwin.write(fd, $0.baseAddress, $0.count) }
            if w > 0 {
                pending.removeFirst(w)
                continue
            }
            if w < 0 && errno == EINTR { continue }
            if w < 0 && errno == EAGAIN {
                if writer == nil {
                    let s = DispatchSource.makeWriteSource(fileDescriptor: fd, queue: .main)
                    s.setEventHandler { [weak self] in
                        self?.writer?.cancel()
                        self?.writer = nil
                        self?.flush()
                    }
                    s.resume()
                    writer = s
                }
                return
            }
            return
        }
    }

    /// the screen's size, told to the far end as it changes
    private func tick() {
        urgent()
        let c = Int(dmc_vt_cols(vt)), r = Int(dmc_vt_rows(vt))
        if c != cols || r != rows {
            cols = c
            rows = r
            if let net {
                dmc_net_resize(net, Int32(c), Int32(r))
            } else if let ssh, shell > 0 {
                ssh.resize(shell, cols: c, rows: r)
            }
        }
        pump()
    }

    private func show(_ s: String) {
        let b = Array(s.utf8).map { CChar(bitPattern: $0) }
        dmc_vt_feed(vt, b, Int32(b.count))
        screenVersion += 1
    }

    private func ended(_ why: String?) {
        guard running else { return }
        running = false
        gen += 1
        timer?.invalidate()
        timer = nil
        source?.cancel()
        source = nil
        writer?.cancel()
        writer = nil
        // (let go: its thread ends with it)
        ssh?.disconnect()
        ssh = nil
        shell = 0
        question = nil
        if fd >= 0 {
            close(fd)
            fd = -1
        }
        if let net { dmc_net_free(net) }
        net = nil
        pending.removeAll()
        show("\r\n\u{1b}[0m[\(why ?? "Disconnected")]\r\n")
        status = why ?? "Disconnected"
    }
}

struct RemotePage: View {
    @ObservedObject var session: RemoteSession

    var body: some View {
        VStack(spacing: 0) {
            HStack {
                Picker("", selection: $session.proto) {
                    ForEach(RemoteSession.Proto.allCases) { Text($0.rawValue).tag($0) }
                }
                .labelsHidden()
                .frame(width: 100)
                .disabled(session.running)
                TextField("Server", text: $session.host)
                    .onSubmit { session.connect() }
                    .disabled(session.running)
                TextField(String(session.proto.defaultPort), text: $session.port)
                    .frame(width: 60)
                    .help("Port (empty: the protocol's)")
                    .disabled(session.running)
                TextField("User", text: $session.user)
                    .frame(width: 120)
                    .disabled(session.running)
                if session.running {
                    Button("Disconnect", role: .destructive) { session.stop() }
                } else {
                    Button("Connect") { session.connect() }
                        .keyboardShortcut(.defaultAction)
                }
            }
            .padding(8)
            TerminalView(host: session, version: session.screenVersion)
            HStack {
                Text(session.status).foregroundStyle(.secondary)
                Spacer()
                TerminalTypePicker(selection: $session.terminalType)
            }
            .padding(.horizontal, 8).padding(.vertical, 4)
        }
        .onChange(of: session.question != nil) { _ in
            answers = Array(repeating: "", count: answers.count)
        }
        .alert(title, isPresented: asking, presenting: session.question) { q in
            switch q {
            case .newHost:
                Button("Connect") { session.answer([]) }
                Button("Cancel", role: .cancel) { session.answer(nil) }
            case .password:
                SecureField("Password", text: answerBinding(0))
                Button("Log In") { session.answer([answers[0]]) }
                Button("Cancel", role: .cancel) { session.answer(nil) }
            case .prompts(_, _, let qs):
                ForEach(qs.indices, id: \.self) { i in
                    if qs[i].1 {
                        TextField(qs[i].0, text: answerBinding(i))
                    } else {
                        SecureField(qs[i].0, text: answerBinding(i))
                    }
                }
                Button("Log In") { session.answer(Array(answers.prefix(qs.count))) }
                Button("Cancel", role: .cancel) { session.answer(nil) }
            }
        } message: { q in
            switch q {
            case .newHost(let type, let fp):
                Text("This server is not known yet. Its key's fingerprint is\n\n\(type) \(fp)\n\n" +
                     "If it's the one you expect, it will be added to ~/.ssh/known_hosts.")
            case .password(let why):
                Text(why)
            case .prompts(_, let instruction, let qs):
                Text(instruction.isEmpty ? qs.map(\.0).joined(separator: "\n") : instruction)
            }
        }
    }

    /// the answers being typed, cleared for each question
    @State private var answers = ["", "", "", "", "", "", "", ""]

    private func answerBinding(_ i: Int) -> Binding<String> {
        Binding(get: { i < answers.count ? answers[i] : "" },
                set: { if i < answers.count { answers[i] = $0 } })
    }

    private var asking: Binding<Bool> {
        // (dismissed by its buttons, each of which answers)
        Binding(get: { session.question != nil }, set: { _ in })
    }

    private var title: String {
        switch session.question {
        case .newHost: return "Connect to a new server?"
        case .password: return "Log in to \(session.host)"
        case .prompts(let t, _, _): return t
        case nil: return ""
        }
    }
}
