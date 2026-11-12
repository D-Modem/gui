// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 D-Modem Authors
//
// SSHConnection.swift - a libdmodemssh connection polled on a thread of
// its own, each event handed to the main queue. It reads OpenSSH's
// config and known_hosts. Its dmc_ssh is freed by finish(), once the
// DMC_SSH_EV_CLOSED that ends it has been handed over.

import Foundation

final class SSHConnection {
    /// an event, copied off the connection's thread
    struct Event {
        let type: Int
        let channel: Int32
        let value: Int32
        let data: [UInt8]?

        /// the data as UTF-8 text
        var text: String? { data.map { String(decoding: $0, as: UTF8.self) } }
    }

    let s: OpaquePointer
    private let done = DispatchSemaphore(value: 0)
    private var started = false
    private var freed = false

    init?(host: String, port: Int, user: String) {
        guard let s = dmc_ssh_new(host, Int32(port), user) else { return nil }
        self.s = s
        // OpenSSH's own config and known hosts: the user's
        dmc_ssh_option(s, "config", "")
        dmc_ssh_option(s, "knownhosts", "")
    }

    /// an option of dmc_ssh_option's, before it starts ("agent": its socket)
    func option(_ name: String, _ value: String) {
        dmc_ssh_option(s, name, value)
    }

    deinit {
        if !freed {
            if started {
                dmc_ssh_disconnect(s)
                done.wait()
            }
            dmc_ssh_free(s)
        }
    }

    /// polled until it's over, each event to handle on the main queue
    func start(_ handle: @escaping (Event) -> Void) {
        started = true
        let s = self.s
        let done = self.done
        let t = Thread {
            var ev = dmc_ssh_event()
            while true {
                let n = dmc_ssh_poll(s, 1000, &ev)
                if n < 0 { break }
                if n == 0 { continue }
                let data: [UInt8]? = ev.data.map { p in
                    p.withMemoryRebound(to: UInt8.self, capacity: Int(ev.len)) {
                        Array(UnsafeBufferPointer(start: $0, count: Int(ev.len)))
                    }
                }
                let e = Event(type: Int(ev.type), channel: ev.channel, value: ev.value, data: data)
                DispatchQueue.main.async { handle(e) }
                if Int(ev.type) == DMC_SSH_EV_CLOSED { break }
            }
            done.signal()
        }
        t.name = "ssh"
        t.start()
    }

    /// what's typed, to a channel
    func write(_ channel: Int32, _ bytes: [UInt8]) {
        bytes.withUnsafeBufferPointer { b in
            b.baseAddress!.withMemoryRebound(to: CChar.self, capacity: b.count) {
                _ = dmc_ssh_write(s, channel, $0, Int32(b.count))
            }
        }
    }

    /// a shell's new size
    func resize(_ channel: Int32, cols: Int, rows: Int) {
        dmc_ssh_resize(s, channel, Int32(cols), Int32(rows))
    }

    /// closed by the user: a DMC_SSH_EV_CLOSED comes
    func disconnect() {
        if !freed { dmc_ssh_disconnect(s) }
    }

    /// the connection freed, once its thread has ended
    func finish() {
        guard !freed else { return }
        done.wait()
        freed = true
        dmc_ssh_free(s)
    }
}
