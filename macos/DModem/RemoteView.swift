// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 D-Modem Authors
//
// RemoteView.swift - the Remote page: a login to a server by SSH, Telnet
// or rlogin, in a terminal of its own (TerminalView.swift's view)
//
// Telnet and rlogin are a socket made here and libdmodemctl's dmc_net
// for the protocol. SSH is the system's /usr/bin/ssh on a pty
// (PtySpawn.c), so that its keys, agent and config are the user's.

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
    private var fd: Int32 = -1          // the socket, or the pty's master
    private var source: DispatchSourceRead?
    private var writer: DispatchSourceWrite?
    private var exitSource: DispatchSourceProcess?
    private var net: OpaquePointer?
    private var pid: pid_t = 0
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
        guard running, fd >= 0, !bytes.isEmpty else { return }
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
                ended("Could not start ssh")
                return
            }
            status = "Connected by SSH"
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

    private func startSSH(host: String, port: Int) -> Bool {
        var args = ["ssh", "-p", String(port)]
        if !user.isEmpty { args += ["-l", user] }
        args += ["--", host]
        let cargs: [UnsafeMutablePointer<CChar>?] = args.map { strdup($0) } + [nil]
        defer { for a in cargs { free(a) } }
        var p: pid_t = 0
        let f = cargs.map { UnsafePointer($0) }.withUnsafeBufferPointer {
            dm_pty_spawn("/usr/bin/ssh", $0.baseAddress, dmc_vt_profile_term(terminalType),
                         Int32(cols), Int32(rows), &p)
        }
        guard f >= 0 else { return false }
        fd = f
        pid = p
        watch()
        let e = DispatchSource.makeProcessSource(identifier: p, eventMask: .exit, queue: .main)
        e.setEventHandler { [weak self] in self?.childExited(p) }
        e.resume()
        exitSource = e
        return true
    }

    private func childExited(_ p: pid_t) {
        var st: Int32 = 0
        waitpid(p, &st, 0)
        guard p == pid else { return }
        pid = 0
        readable()      // (what is left)
        let code = (st & 0x7f) == 0 ? (st >> 8) & 0xff : 0
        ended(code != 0 ? "ssh exited with status \(code)" : "Connection closed")
    }

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
            // the end: the server closed, or ssh's pty (its exit says why)
            pump()
            if net != nil {
                ended("Connection closed by the server")
            } else {
                source?.cancel()
                source = nil
            }
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
            if let net { dmc_net_type(net, buf, k) } else {
                pending.append(contentsOf: buf[0..<Int(k)].map { UInt8(bitPattern: $0) })
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
            } else if fd >= 0 {
                dm_pty_resize(fd, Int32(c), Int32(r))
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
        exitSource?.cancel()
        exitSource = nil
        if pid > 0 {
            kill(pid, SIGHUP)
            let p = pid
            DispatchQueue.global().async { var st: Int32 = 0; waitpid(p, &st, 0) }
            pid = 0
        }
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
    }
}
