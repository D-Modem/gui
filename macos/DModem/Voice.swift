// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 D-Modem Authors
//
// Voice.swift - the Phone page: voice calls dialled and answered, their
// sound on the Mac's microphone and speakers (AVAudioEngine), the
// keypad's DTMF; the answering machine (off until the user turns it on),
// its greeting and the messages it took, kept as WAV files in
// ~/Library/Application Support/D-Modem/Voicemail

import AppKit
import AVFoundation
import SwiftUI
import UniformTypeIdentifiers
import UserNotifications

/// Samples between the sound card's thread and the main one, the newest
/// `capacity` of them kept.
final class SampleQueue {
    private var buf: [Float]
    private var head = 0
    private var count = 0
    private let lock = NSLock()

    init(capacity: Int) {
        buf = [Float](repeating: 0, count: capacity)
    }

    func push(_ x: UnsafeBufferPointer<Float>) {
        lock.lock()
        for v in x {
            if count == buf.count {
                head = (head + 1) % buf.count
                count -= 1
            }
            buf[(head + count) % buf.count] = v
            count += 1
        }
        lock.unlock()
    }

    /// up to n into p: how many
    func pop(into p: UnsafeMutablePointer<Float>, _ n: Int) -> Int {
        lock.lock()
        let k = min(n, count)
        for i in 0..<k {
            p[i] = buf[head]
            head = (head + 1) % buf.count
        }
        count -= k
        lock.unlock()
        return k
    }

    var available: Int {
        lock.lock()
        defer { lock.unlock() }
        return count
    }

    func clear() {
        lock.lock()
        head = 0
        count = 0
        lock.unlock()
    }
}

/// The sound card at the line's own rate, 8 kHz mono: the engine's mixer
/// and a converter make it the hardware's.
final class AudioIO {
    let speaker = SampleQueue(capacity: Int(DMC_VOICE_RATE))
    let mic = SampleQueue(capacity: Int(DMC_VOICE_RATE))
    private var engine: AVAudioEngine?
    private var tapped = false
    private let format = AVAudioFormat(commonFormat: .pcmFormatFloat32,
                                       sampleRate: Double(DMC_VOICE_RATE),
                                       channels: 1, interleaved: false)!

    var running: Bool { engine != nil }

    /// the speaker from `speaker`, the microphone into `mic`: false if
    /// there is no sound
    func start(play: Bool, record: Bool) -> Bool {
        stop()
        let e = AVAudioEngine()
        let fmt = format
        if play {
            let q = speaker
            let src = AVAudioSourceNode(format: fmt) { _, _, frames, abl -> OSStatus in
                let list = UnsafeMutableAudioBufferListPointer(abl)
                guard let p = list[0].mData?.assumingMemoryBound(to: Float.self) else { return noErr }
                let n = Int(frames)
                let got = q.pop(into: p, n)
                var i = got
                while i < n {
                    p[i] = 0
                    i += 1
                }
                return noErr
            }
            e.attach(src)
            e.connect(src, to: e.mainMixerNode, format: fmt)
        }
        if record {
            let input = e.inputNode
            let hw = input.outputFormat(forBus: 0)
            guard hw.sampleRate > 0, hw.channelCount > 0,
                  let conv = AVAudioConverter(from: hw, to: fmt) else { return false }
            let q = mic
            input.installTap(onBus: 0, bufferSize: 1024, format: hw) { buffer, _ in
                let cap = AVAudioFrameCount(Double(buffer.frameLength) * fmt.sampleRate / hw.sampleRate) + 32
                guard let out = AVAudioPCMBuffer(pcmFormat: fmt, frameCapacity: cap) else { return }
                var given = false
                var err: NSError?
                _ = conv.convert(to: out, error: &err) { _, status in
                    if given {
                        status.pointee = .noDataNow
                        return nil
                    }
                    given = true
                    status.pointee = .haveData
                    return buffer
                }
                if let d = out.floatChannelData?[0], out.frameLength > 0 {
                    q.push(UnsafeBufferPointer(start: d, count: Int(out.frameLength)))
                }
            }
            tapped = true
        }
        do {
            e.prepare()
            try e.start()
        } catch {
            if tapped { e.inputNode.removeTap(onBus: 0) }
            tapped = false
            return false
        }
        engine = e
        return true
    }

    func stop() {
        if let e = engine {
            if tapped { e.inputNode.removeTap(onBus: 0) }
            e.stop()
        }
        tapped = false
        engine = nil
        speaker.clear()
        mic.clear()
    }
}

/// A message the answering machine took.
struct VoiceMessage: Identifiable {
    var url: URL
    var who: String
    var date: Date
    var seconds: Int
    var id: URL { url }
}

final class Voice: ObservableObject {
    weak var modem: Modem?
    let audio = AudioIO()

    @Published var voicemail: Bool {
        didSet { save(); configure() }
    }
    @Published var rings: Int {
        didSet { save(); configure() }
    }
    @Published var muted = false
    @Published var listening = false {
        didSet { modem.map { sound(Int($0.state)) } }
    }
    @Published private(set) var messages: [VoiceMessage] = []
    @Published private(set) var greetingSeconds = 0
    @Published private(set) var recording = false
    /// what plays: a message's file, or the greeting's
    @Published private(set) var playing: URL?

    private let maxSeconds: Int
    private var greeting: [Int16] = []
    private var playBuf: [Int16] = []
    private var playPos = 0
    private var recBuf: [Int16] = []
    private var timer: Timer?
    private var lastState = Int(DMC_CLOSED)
    private var listenOn = false

    /// the longest greeting recorded (s)
    private static let greetingMax = 30
    private static let defaults = UserDefaults.standard

    init() {
        let d = Voice.defaults
        voicemail = d.bool(forKey: "voicemail")
        let r = d.integer(forKey: "voicemailRings")
        rings = (1...15).contains(r) ? r : 4
        let m = d.integer(forKey: "voicemailMaxSeconds")
        maxSeconds = m >= 10 ? m : 120
        loadGreeting()
        refreshMessages()
    }

    private func save() {
        let d = Voice.defaults
        d.set(voicemail, forKey: "voicemail")
        d.set(rings, forKey: "voicemailRings")
    }

    // MARK: - files

    private static var dir: URL {
        FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("D-Modem")
    }
    private static var messagesDir: URL { dir.appendingPathComponent("Voicemail") }
    static var greetingURL: URL { dir.appendingPathComponent("greeting.wav") }

    static func writeWav(_ pcm: [Int16], to url: URL) throws {
        try FileManager.default.createDirectory(at: url.deletingLastPathComponent(),
                                                withIntermediateDirectories: true)
        var head = [CChar](repeating: 0, count: 44)
        dmc_wav_header(&head, Int32(pcm.count))
        var d = Data(head.map { UInt8(bitPattern: $0) })
        d.reserveCapacity(44 + pcm.count * 2)
        for v in pcm {
            let u = UInt16(bitPattern: v)
            d.append(UInt8(u & 0xff))
            d.append(UInt8(u >> 8))
        }
        try d.write(to: url, options: .atomic)
    }

    /// a WAV file's samples at 8 kHz, nil if it is not one D-Modem reads
    static func readWav(_ url: URL) -> [Int16]? {
        guard let d = try? Data(contentsOf: url), d.count < Int(Int32.max) else { return nil }
        var pcm: UnsafeMutablePointer<Int16>?
        let n = d.withUnsafeBytes { b in
            dmc_wav_decode(b.baseAddress?.assumingMemoryBound(to: CChar.self),
                           Int32(b.count), &pcm)
        }
        defer { dmc_free(pcm) }
        guard n >= 0, let p = pcm else { return nil }
        return Array(UnsafeBufferPointer(start: p, count: Int(n)))
    }

    private func loadGreeting() {
        greeting = Voice.readWav(Voice.greetingURL) ?? []
        greetingSeconds = (greeting.count + Int(DMC_VOICE_RATE) / 2) / Int(DMC_VOICE_RATE)
    }

    /// the answering machine's settings to the session
    func configure() {
        guard let s = modem?.session else { return }
        greeting.withUnsafeBufferPointer { g in
            dmc_voicemail_set(s, g.baseAddress, Int32(g.count), Int32(maxSeconds), 0)
        }
        dmc_voicemail_auto(s, voicemail ? Int32(rings) : 0)
    }

    func refreshMessages() {
        let fm = FileManager.default
        let names = (try? fm.contentsOfDirectory(atPath: Voice.messagesDir.path)) ?? []
        let parse = DateFormatter()
        parse.dateFormat = "yyyyMMdd-HHmmss"
        parse.locale = Locale(identifier: "en_US_POSIX")
        messages = names.filter { $0.hasSuffix(".wav") }.sorted(by: >).compactMap { name in
            // 20261111-120501_5551234.wav
            let base = String(name.dropLast(4))
            let parts = base.split(separator: "_", maxSplits: 1, omittingEmptySubsequences: false)
            guard let stamp = parts.first, let date = parse.date(from: String(stamp)) else { return nil }
            let who = parts.count > 1 ? String(parts[1]) : ""
            let url = Voice.messagesDir.appendingPathComponent(name)
            let size = (try? fm.attributesOfItem(atPath: url.path)[.size] as? Int) ?? 0
            return VoiceMessage(url: url, who: who.isEmpty ? "Unknown caller" : who, date: date,
                                seconds: max(0, (size - 44) / (2 * Int(DMC_VOICE_RATE))))
        }
    }

    /// the message the answering machine took, kept
    private func saveMessage(from number: String) {
        guard let s = modem?.session else { return }
        var p: UnsafePointer<Int16>?
        let n = dmc_voice_message(s, &p)
        guard n > 0, let p else { return }
        let pcm = Array(UnsafeBufferPointer(start: p, count: Int(n)))
        let who = String(number.filter { $0.isLetter || $0.isNumber || $0 == "+" || $0 == "#" }.prefix(32))
        let f = DateFormatter()
        f.dateFormat = "yyyyMMdd-HHmmss"
        f.locale = Locale(identifier: "en_US_POSIX")
        let url = Voice.messagesDir.appendingPathComponent("\(f.string(from: Date()))_\(who).wav")
        do {
            try Voice.writeWav(pcm, to: url)
        } catch {
            modem?.say("Can't keep the message: \(error.localizedDescription)")
            return
        }
        modem?.addLog("The answering machine took a message.")
        refreshMessages()
        let c = UNMutableNotificationContent()
        c.title = "New voice message"
        c.body = "From " + (who.isEmpty ? "an unknown caller" : who)
        let center = UNUserNotificationCenter.current()
        center.requestAuthorization(options: [.alert, .sound]) { ok, _ in
            guard ok else { return }
            center.add(UNNotificationRequest(identifier: "voicemail", content: c, trigger: nil))
        }
    }

    // MARK: - what the session does

    /// the session's events of voice calls
    func event(_ type: Int, text: String, ok: Bool) {
        if type == Int(DMC_EV_MESSAGE) {
            if ok { saveMessage(from: text) }
        } else if type == Int(DMC_EV_VOICE) {
            if text == "b" { modem?.say("Busy") }
            if text == "l" && lastState != Int(DMC_VOICEMAIL) { modem?.say("The other end hung up") }
        }
    }

    /// the sound as the call needs it: both ways in a call; the caller (if
    /// listening) while the answering machine has the call
    func sound(_ st: Int) {
        let was = lastState
        let inCall = st == Int(DMC_VOICE) || st == Int(DMC_VOICEMAIL)
        let listen = st == Int(DMC_VOICEMAIL) && listening
        if st == Int(DMC_VOICE) && was != Int(DMC_VOICE) {
            stopSound()
            listenOn = false
            if !audio.start(play: true, record: true) {
                modem?.say("No sound: allow D-Modem the microphone in System Settings")
            }
        } else if listen && !listenOn {
            _ = audio.start(play: true, record: false)
            listenOn = true
        } else if !listen && listenOn {
            audio.stop()
            listenOn = false
        } else if was == Int(DMC_VOICE) && st != Int(DMC_VOICE) {
            audio.stop()
        }
        lastState = st
        tick(inCall || playing != nil || recording)
    }

    /// the 20 ms tick that a call, a playback or a recording needs
    private func tick(_ on: Bool) {
        if on && timer == nil {
            let t = Timer(timeInterval: 0.02, repeats: true) { [weak self] _ in
                self?.modem?.pump()
            }
            RunLoop.main.add(t, forMode: .common)
            timer = t
        } else if !on, let t = timer {
            t.invalidate()
            timer = nil
        }
    }

    /// before the session runs: the microphone's samples to it (or to the
    /// greeting being recorded)
    func beforePoll() {
        guard audio.running else { return }
        var f = [Float](repeating: 0, count: 512)
        while true {
            let n = f.withUnsafeMutableBufferPointer { audio.mic.pop(into: $0.baseAddress!, 512) }
            if n <= 0 { break }
            let pcm = f[0..<n].map { Int16(max(-32768, min(32767, $0 * 32767))) }
            if recording {
                let room = Voice.greetingMax * Int(DMC_VOICE_RATE) - recBuf.count
                recBuf += pcm.prefix(max(0, room))
            } else if lastState == Int(DMC_VOICE), !muted, let s = modem?.session {
                pcm.withUnsafeBufferPointer { dmc_voice_write(s, $0.baseAddress, Int32($0.count)) }
            }
        }
    }

    /// after: the far end's samples to the speaker, or the next of what
    /// plays
    func afterPoll() {
        if playing != nil {
            let want = Int(DMC_VOICE_RATE) / 4
            while audio.speaker.available < want && playPos < playBuf.count {
                let k = min(512, playBuf.count - playPos)
                let f = playBuf[playPos..<(playPos + k)].map { Float($0) / 32768 }
                f.withUnsafeBufferPointer { audio.speaker.push($0) }
                playPos += k
            }
            if playPos >= playBuf.count && audio.speaker.available == 0 {
                stopSound()
            }
            return
        }
        guard audio.running, let s = modem?.session else { return }
        var pcm = [Int16](repeating: 0, count: 512)
        while true {
            let n = Int(dmc_voice_read(s, &pcm, 512))
            if n <= 0 { break }
            let f = pcm[0..<n].map { Float($0) / 32768 }
            f.withUnsafeBufferPointer { audio.speaker.push($0) }
        }
    }

    // MARK: - what the page does

    func stopSound() {
        audio.stop()
        playing = nil
        playBuf = []
        playPos = 0
        recording = false
        recBuf = []
        listenOn = false
        tick(lastState == Int(DMC_VOICE) || lastState == Int(DMC_VOICEMAIL))
    }

    private func play(_ pcm: [Int16], _ url: URL) {
        stopSound()
        playBuf = pcm
        playPos = 0
        guard audio.start(play: true, record: false) else {
            modem?.say("No sound to play it on")
            return
        }
        playing = url
        tick(true)
    }

    func play(_ m: VoiceMessage) {
        if playing == m.url {
            stopSound()
            return
        }
        guard let pcm = Voice.readWav(m.url) else {
            modem?.say("Can't read the message")
            return
        }
        play(pcm, m.url)
    }

    func delete(_ m: VoiceMessage) {
        if playing == m.url { stopSound() }
        try? FileManager.default.removeItem(at: m.url)
        refreshMessages()
    }

    func playGreeting() {
        if playing == Voice.greetingURL {
            stopSound()
            return
        }
        if !greeting.isEmpty { play(greeting, Voice.greetingURL) }
    }

    func recordGreeting() {
        if recording {
            let pcm = recBuf
            stopSound()
            if pcm.count > Int(DMC_VOICE_RATE) / 2 {
                do {
                    try Voice.writeWav(pcm, to: Voice.greetingURL)
                } catch {
                    modem?.say("Can't keep the greeting: \(error.localizedDescription)")
                }
                loadGreeting()
                configure()
            }
            return
        }
        stopSound()
        recBuf = []
        guard audio.start(play: false, record: true) else {
            modem?.say("No microphone: allow D-Modem the microphone in System Settings")
            return
        }
        recording = true
        tick(true)
    }

    func chooseGreeting() {
        let p = NSOpenPanel()
        p.title = "Greeting"
        p.allowedContentTypes = [UTType.wav]
        p.allowsMultipleSelection = false
        guard p.runModal() == .OK, let url = p.url else { return }
        guard let pcm = Voice.readWav(url), !pcm.isEmpty else {
            modem?.say("That isn't a WAV file D-Modem can play (PCM, A-law or µ-law)")
            return
        }
        do {
            try Voice.writeWav(pcm, to: Voice.greetingURL)
        } catch {
            modem?.say("Can't keep the greeting: \(error.localizedDescription)")
        }
        loadGreeting()
        configure()
    }

    func clearGreeting() {
        if playing == Voice.greetingURL { stopSound() }
        try? FileManager.default.removeItem(at: Voice.greetingURL)
        loadGreeting()
        configure()
    }
}

// MARK: - the page

struct PhonePage: View {
    @EnvironmentObject var modem: Modem
    @EnvironmentObject var voice: Voice
    @State private var number = ""

    private var st: Int { modem.state }

    private var status: String {
        let who = modem.callerNumber
        switch st {
        case Int(DMC_CLOSED): return "The modem isn't running"
        case Int(DMC_RINGING): return who.isEmpty ? "Ringing" : "Ringing: \(who)"
        case Int(DMC_VOICE): return "In a call"
        case Int(DMC_VOICEMAIL): return who.isEmpty ? "Taking a message" : "Taking a message from \(who)"
        default: return modem.stateName
        }
    }

    private let keys = [["1", "2", "3"], ["4", "5", "6"], ["7", "8", "9"], ["*", "0", "#"]]

    private func key(_ k: String) {
        if st == Int(DMC_VOICE) {
            modem.dtmf(k)
        } else {
            number += k
        }
    }

    private static let when: DateFormatter = {
        let f = DateFormatter()
        f.dateStyle = .medium
        f.timeStyle = .short
        return f
    }()

    var body: some View {
        Form {
            Section {
                LabeledContent("Phone", value: status)
                HStack {
                    TextField("Number to call", text: $number)
                        .onSubmit { if modem.isIdle { modem.voiceDial(number) } }
                    Button("Call") { modem.voiceDial(number) }
                        .disabled(!modem.isIdle)
                }
                HStack {
                    Button("Answer") { voice.stopSound(); modem.voiceAnswer() }
                        .disabled(st != Int(DMC_RINGING))
                    Button("Take a Message") { voice.stopSound(); modem.voicemailAnswer() }
                        .disabled(st != Int(DMC_RINGING))
                    if st == Int(DMC_VOICEMAIL) {
                        Button("Pick Up") { modem.pickUp() }
                            .help("Talk to the caller yourself")
                        Toggle("Listen", isOn: $voice.listening)
                            .help("Hear the caller leave the message")
                    }
                    Spacer()
                    Toggle("Mute", isOn: $voice.muted)
                        .disabled(st != Int(DMC_VOICE))
                    Button("Hang Up", role: .destructive) { modem.hangUp() }
                        .disabled(![DMC_VOICE, DMC_VOICEMAIL, DMC_DIALING, DMC_ANSWERING]
                            .map { Int($0) }.contains(st))
                }
                Grid(horizontalSpacing: 8, verticalSpacing: 8) {
                    ForEach(keys, id: \.self) { row in
                        GridRow {
                            ForEach(row, id: \.self) { k in
                                Button { key(k) } label: {
                                    Text(k).font(.title2.monospacedDigit()).frame(width: 48, height: 28)
                                }
                            }
                        }
                    }
                }
                .frame(maxWidth: .infinity)
                .disabled(!(st == Int(DMC_VOICE) || st == Int(DMC_IDLE) || st == Int(DMC_CLOSED)))
            } header: {
                Text("Voice Call")
            } footer: {
                Text("Calls through the modem's voice mode, with the Mac's microphone and speakers.")
            }

            Section {
                Toggle("Take messages", isOn: $voice.voicemail)
                Stepper("Rings before it answers: \(voice.rings)", value: $voice.rings, in: 1...15)
                    .disabled(!voice.voicemail)
                LabeledContent("Greeting") {
                    HStack {
                        Text(voice.recording ? "Recording: speak, then Stop"
                             : voice.greetingSeconds > 0 ? "Your recording (\(voice.greetingSeconds) s)"
                             : "None: callers hear a beep")
                            .foregroundStyle(.secondary)
                        Button { voice.playGreeting() } label: {
                            Image(systemName: voice.playing == Voice.greetingURL ? "stop.fill" : "play.fill")
                        }
                        .help("Play")
                        .disabled(voice.greetingSeconds == 0 || voice.recording)
                        Button(voice.recording ? "Stop" : "Record") { voice.recordGreeting() }
                            .disabled(voice.playing != nil || st == Int(DMC_VOICE) || st == Int(DMC_VOICEMAIL))
                        Button { voice.chooseGreeting() } label: { Image(systemName: "folder") }
                            .help("Use a WAV file")
                        Button { voice.clearGreeting() } label: { Image(systemName: "trash") }
                            .help("No greeting: the beep alone")
                            .disabled(voice.greetingSeconds == 0 || voice.recording)
                    }
                    .buttonStyle(.borderless)
                }
            } header: {
                Text("Answering Machine")
            } footer: {
                Text("It answers calls nobody picks up, plays the greeting and records. Messages are kept on this Mac only.")
            }

            Section("Messages") {
                if voice.messages.isEmpty {
                    Text("No messages").foregroundStyle(.secondary)
                }
                ForEach(voice.messages) { m in
                    HStack {
                        VStack(alignment: .leading) {
                            Text(m.who)
                            Text("\(PhonePage.when.string(from: m.date)) · \(m.seconds / 60):\(String(format: "%02d", m.seconds % 60))")
                                .font(.caption).foregroundStyle(.secondary)
                        }
                        Spacer()
                        Button { voice.play(m) } label: {
                            Image(systemName: voice.playing == m.url ? "stop.fill" : "play.fill")
                        }
                        .help("Play")
                        .disabled(voice.recording || st == Int(DMC_VOICE))
                        Button { voice.delete(m) } label: { Image(systemName: "trash") }
                            .help("Delete")
                    }
                    .buttonStyle(.borderless)
                }
            }
        }
        .formStyle(.grouped)
    }
}
