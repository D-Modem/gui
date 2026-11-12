// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 D-Modem Authors
//
// TerminalView.swift - the modem's console and its full-screen setup, and
// the terminal view the Remote page shares: libdmodemctl's terminal screen
// (dmc_vt), drawn cell by cell
//
// Every kind of terminal the engine is (xterm, the VTs, ANSI-BBS, the
// Commodores and Ataris, Viewdata, the Minitel, the Tektronix) is drawn
// here: a cell's background, its character in Menlo or, where the profile
// has its machine's font, the glyph's bitmap (a row's glyphs put in an
// image at the font's own size and stretched over the row, so a C64's 8x8
// stays sharp), its lines, the pictures (sixel, ReGIS, RIPscrip's, the
// Tektronix's) over the text, and the cursor. A machine with a screen of
// its own size (a C64's 40x25) has its cells stretched to fill the view.

import AppKit
import SwiftUI

/// what a terminal view shows and where its keys go: the modem, or a
/// remote login (RemoteView.swift)
protocol TerminalHost: AnyObject {
    var vt: OpaquePointer { get }
    /// the bytes a key or a paste sends, as the terminal sends them
    func send(_ bytes: [UInt8])
}

extension Modem: TerminalHost {
    func send(_ bytes: [UInt8]) { type(bytes) }
}

/// the kinds of terminal, in the pickers' order
enum TerminalTypes {
    struct Kind: Identifiable, Hashable {
        let id: String
        let label: String
    }

    static let all: [Kind] = (0..<Int(dmc_vt_profile_count())).map { i in
        Kind(id: String(cString: dmc_vt_profile_id(Int32(i))),
             label: String(cString: dmc_vt_profile_label(Int32(i))))
    }

    /// what the terminal told the remote after it was fed: its bell,
    /// its music and its printer (the last two not kept)
    static func afterFeed(_ vt: OpaquePointer) {
        var buf = [CChar](repeating: 0, count: 256)
        while dmc_vt_printed(vt, &buf, Int32(buf.count)) > 0 {}
        var hz: UInt32 = 0
        var ms: Int32 = 0, rest: Int32 = 0
        while dmc_vt_music(vt, &hz, &ms, &rest) != 0 {}
        if dmc_vt_bells(vt) > 0 { NSSound.beep() }
    }
}

/// a picker of the kinds of terminal
struct TerminalTypePicker: View {
    @Binding var selection: String

    var body: some View {
        Picker("Terminal", selection: $selection) {
            ForEach(TerminalTypes.all) { Text($0.label).tag($0.id) }
        }
        .frame(maxWidth: 260)
        .help("The kind of terminal")
    }
}

struct TerminalPane: View {
    @EnvironmentObject var modem: Modem

    var body: some View {
        VStack(spacing: 0) {
            HStack {
                Group {
                    Button("Full-screen setup") { modem.type("AT!CF\r") }
                        .help("The configurator in screens and menus (AT!CF)")
                    Button("Command-line setup") { modem.type("AT!CT\r") }
                        .help("The configurator as commands, Cisco style (AT!CT)")
                }
                .disabled(!modem.isIdle)
                Spacer()
                TerminalTypePicker(selection: $modem.terminalType)
                Button("Clear") { modem.clearScreen() }
            }
            .padding(8)
            TerminalView(host: modem, version: modem.screenVersion)
        }
    }
}

struct TerminalView: NSViewRepresentable {
    let host: TerminalHost
    let version: Int

    func makeNSView(context: Context) -> TerminalNSView {
        let v = TerminalNSView()
        v.host = host
        return v
    }

    func updateNSView(_ v: TerminalNSView, context: Context) {
        v.host = host
        v.relayout()
        v.needsDisplay = true
    }
}

final class TerminalNSView: NSView {
    weak var host: TerminalHost?
    private let font = NSFont(name: "Menlo", size: 13) ?? .monospacedSystemFont(ofSize: 13, weight: .regular)
    private lazy var fonts: [NSFont] = {
        let m = NSFontManager.shared
        let b = m.convert(font, toHaveTrait: .boldFontMask)
        return [font, b, m.convert(font, toHaveTrait: .italicFontMask),
                m.convert(b, toHaveTrait: .italicFontMask)]
    }()
    /// the font's cell, and a cell as drawn
    private var fontCell = NSSize(width: 8, height: 16)
    private var cell = NSSize(width: 8, height: 16)
    private var origin = NSPoint(x: 0, y: 0)
    private var statusRows = 0
    private var scroll = 0          // rows up into the scrollback
    private var blinkOff = false, hasBlink = false
    private var blinkTimer: Timer?
    private var buttons = 0

    override init(frame: NSRect) {
        super.init(frame: frame)
        let w = ("M" as NSString).size(withAttributes: [.font: font]).width
        fontCell = NSSize(width: ceil(w), height: ceil(font.ascender - font.descender + font.leading))
        cell = fontCell
        let t = Timer(timeInterval: 0.5, repeats: true) { [weak self] _ in
            guard let self else { return }
            self.blinkOff.toggle()
            if self.hasBlink { self.needsDisplay = true }
        }
        RunLoop.main.add(t, forMode: .common)
        blinkTimer = t
    }

    required init?(coder: NSCoder) { fatalError("not used") }

    deinit { blinkTimer?.invalidate() }

    override var isFlipped: Bool { true }
    override var acceptsFirstResponder: Bool { true }

    override func viewDidMoveToWindow() {
        window?.makeFirstResponder(self)
    }

    override func becomeFirstResponder() -> Bool {
        focusChanged(true)
        return true
    }

    override func resignFirstResponder() -> Bool {
        focusChanged(false)
        return true
    }

    private func focusChanged(_ on: Bool) {
        guard let host else { return }
        var buf = [CChar](repeating: 0, count: 16)
        let n = dmc_vt_focus(host.vt, on ? 1 : 0, &buf, Int32(buf.count))
        send(buf, n)
        needsDisplay = true
    }

    override func setFrameSize(_ size: NSSize) {
        super.setFrameSize(size)
        relayout()
    }

    /// the columns and rows that fit, or the machine's own with its cells
    /// stretched to fill the view
    func relayout() {
        guard let vt = host?.vt else { return }
        let W = bounds.width, H = bounds.height
        guard W > 0, H > 0 else { return }
        let rc = Int(dmc_vt_requested_cols(vt)), rr = Int(dmc_vt_requested_rows(vt))
        let st = dmc_vt_status_line(vt).map { String(cString: $0) } ?? ""
        statusRows = st.isEmpty ? 0 : 1
        var gw: Int32 = 0, gh: Int32 = 0
        let glyphs = dmc_vt_glyph_cell(vt, &gw, &gh) != 0
        var cw = fontCell.width, ch = fontCell.height
        if glyphs && rc > 0 && rr > 0 {
            let s = min(floor(W / CGFloat(rc * Int(gw))), floor(H / CGFloat((rr + statusRows) * Int(gh))))
            if s >= 1 {
                cw = CGFloat(gw) * s
                ch = CGFloat(gh) * s
            } else {
                cw = floor(W / CGFloat(rc))
                ch = floor(H / CGFloat(rr + statusRows))
            }
        } else {
            if rc > 0 { cw = floor(W / CGFloat(rc)) }
            if rr > 0 { ch = floor(H / CGFloat(rr + statusRows)) }
        }
        cw = max(2, cw)
        ch = max(2, ch)
        let cols = max(10, min(400, rc > 0 ? rc : Int(W / cw)))
        let rows = max(3, min(200, rr > 0 ? rr : Int(H / ch) - statusRows))
        cell = NSSize(width: cw, height: ch)
        origin = NSPoint(x: rc > 0 ? max(0, floor((W - CGFloat(cols) * cw) / 2)) : 0,
                         y: rr > 0 ? max(0, floor((H - CGFloat(rows + statusRows) * ch) / 2)) : 0)
        dmc_vt_set_cell_pixels(vt, Int32(cw), Int32(ch))
        if cols != Int(dmc_vt_cols(vt)) || rows != Int(dmc_vt_rows(vt)) {
            dmc_vt_resize(vt, Int32(cols), Int32(rows))
        }
    }

    // MARK: - drawing

    private static func color(_ c: UInt32) -> CGColor {
        CGColor(srgbRed: CGFloat((c >> 16) & 0xff) / 255, green: CGFloat((c >> 8) & 0xff) / 255,
                blue: CGFloat(c & 0xff) / 255, alpha: 1)
    }

    /// pixels (0xAARRGGBB, not premultiplied) as an image
    private static func image(_ px: UnsafePointer<UInt32>, _ w: Int, _ h: Int) -> CGImage? {
        var data = [UInt32](repeating: 0, count: w * h)
        for i in 0..<(w * h) {
            let v = px[i], a = v >> 24
            data[i] = (a << 24) | ((((v >> 16) & 255) * a / 255) << 16) |
                ((((v >> 8) & 255) * a / 255) << 8) | ((v & 255) * a / 255)
        }
        return data.withUnsafeMutableBytes { b -> CGImage? in
            guard let ctx = CGContext(data: b.baseAddress, width: w, height: h, bitsPerComponent: 8,
                                      bytesPerRow: w * 4, space: CGColorSpace(name: CGColorSpace.sRGB)!,
                                      bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue |
                                        CGBitmapInfo.byteOrder32Little.rawValue) else { return nil }
            return ctx.makeImage()
        }
    }

    /// an image drawn upright in rect (the view is flipped), unsmoothed
    private static func draw(_ img: CGImage, in r: CGRect, _ ctx: CGContext) {
        ctx.saveGState()
        ctx.interpolationQuality = .none
        ctx.translateBy(x: r.minX, y: r.maxY)
        ctx.scaleBy(x: 1, y: -1)
        ctx.draw(img, in: CGRect(origin: .zero, size: r.size))
        ctx.restoreGState()
    }

    override func draw(_ dirty: NSRect) {
        guard let ctx = NSGraphicsContext.current?.cgContext else { return }
        guard let vt = host?.vt else {
            NSColor.black.setFill()
            bounds.fill()
            return
        }
        ctx.setFillColor(Self.color(dmc_vt_background(vt)))
        ctx.fill(bounds)
        let rows = Int(dmc_vt_rows(vt)), cols = Int(dmc_vt_cols(vt))
        scroll = min(scroll, Int(dmc_vt_history(vt)))
        hasBlink = false
        var pictures: [UInt32: CGImage] = [:]
        for y in 0..<rows {
            let vrow = Int32(y - scroll)
            let size = Int(dmc_vt_line_size(vt, vrow))
            let cursor = scroll == 0 && dmc_vt_cursor_visible(vt) != 0 && Int(dmc_vt_cursor_row(vt)) == y
            ctx.saveGState()
            ctx.translateBy(x: origin.x, y: origin.y + CGFloat(y) * cell.height)
            ctx.clip(to: CGRect(x: 0, y: 0, width: CGFloat(cols) * cell.width, height: cell.height))
            var n = cols
            if size != Int(DMC_LINE_SINGLE) {
                n = (cols + 1) / 2
                if size == Int(DMC_LINE_DOUBLE_WIDTH) {
                    ctx.scaleBy(x: 2, y: 1)
                } else {
                    if size == Int(DMC_LINE_DOUBLE_BOTTOM) { ctx.translateBy(x: 0, y: -cell.height) }
                    ctx.scaleBy(x: 2, y: 2)
                }
            }
            drawRow(vt, vrow, n, cursor, ctx, &pictures)
            ctx.restoreGState()
        }
        if statusRows > 0, let s = dmc_vt_status_line(vt) {
            let r = CGRect(x: origin.x, y: origin.y + CGFloat(rows) * cell.height,
                           width: CGFloat(cols) * cell.width, height: cell.height)
            ctx.setFillColor(Self.color(dmc_vt_foreground(vt)))
            ctx.fill(r)
            (String(cString: s) as NSString).draw(at: r.origin, withAttributes: [
                .font: font, .foregroundColor: NSColor(cgColor: Self.color(dmc_vt_background(vt))) ?? .black])
        }
    }

    private func drawRow(_ vt: OpaquePointer, _ vrow: Int32, _ ncols: Int, _ cursor: Bool,
                         _ ctx: CGContext, _ pictures: inout [UInt32: CGImage]) {
        let cw = cell.width, ch = cell.height
        var cells = [dmc_cell](repeating: dmc_cell(), count: ncols)
        var anyGlyph = false, anyImage = false
        for x in 0..<ncols {
            dmc_vt_cell(vt, vrow, Int32(x), &cells[x])
            let f = Int(cells[x].flags)
            if f & Int(DMC_CELL_BLINK) != 0 { hasBlink = true }
            if f & Int(DMC_CELL_GLYPH) != 0 { anyGlyph = true }
            if f & Int(DMC_CELL_IMAGE) != 0 { anyImage = true }
        }
        // the backgrounds, in runs
        var x = 0
        while x < ncols {
            var n = 1
            while x + n < ncols && cells[x + n].bg == cells[x].bg { n += 1 }
            ctx.setFillColor(Self.color(cells[x].bg))
            ctx.fill(CGRect(x: CGFloat(x) * cw, y: 0, width: CGFloat(n) * cw, height: ch))
            x += n
        }
        // the text, in the font
        let ty = max(0, floor((ch - fontCell.height) / 2))
        for x in 0..<ncols {
            let c = cells[x], f = Int(c.flags)
            if f & Int(DMC_CELL_GLYPH | DMC_CELL_INVISIBLE | DMC_CELL_WIDE_TAIL) != 0 { continue }
            if f & Int(DMC_CELL_BLINK) != 0 && blinkOff { continue }
            if c.ch == 0 || (c.ch == 32 && f & Int(DMC_CELL_COMBINING) == 0) { continue }
            let s: String
            if f & Int(DMC_CELL_COMBINING) != 0, let t = dmc_vt_cell_text(vt, vrow, Int32(x)) {
                s = String(cString: t)
            } else {
                s = Unicode.Scalar(c.ch).map { String(Character($0)) } ?? "?"
            }
            let fi = (f & Int(DMC_CELL_BOLD) != 0 ? 1 : 0) | (f & Int(DMC_CELL_ITALIC) != 0 ? 2 : 0)
            (s as NSString).draw(at: NSPoint(x: CGFloat(x) * cw, y: ty), withAttributes: [
                .font: fonts[fi], .foregroundColor: NSColor(cgColor: Self.color(c.fg)) ?? .white])
        }
        if anyGlyph { drawGlyphs(vt, cells, ctx) }
        // the lines
        let th = max(1, floor(ch / 16))
        for x in 0..<ncols {
            let c = cells[x], f = Int(c.flags), l = CGFloat(x) * cw
            if f & Int(DMC_CELL_INVISIBLE) != 0 { continue }
            ctx.setFillColor(Self.color(c.fg))
            if f & Int(DMC_CELL_UNDERLINE | DMC_CELL_LINK) != 0 {
                ctx.fill(CGRect(x: l, y: ch - 2 * th, width: cw, height: th))
            }
            if f & Int(DMC_CELL_DOUBLE_UNDERLINE) != 0 {
                ctx.fill(CGRect(x: l, y: ch - th, width: cw, height: th))
                ctx.fill(CGRect(x: l, y: ch - 3 * th, width: cw, height: th))
            }
            if f & Int(DMC_CELL_CURLY_UNDERLINE) != 0 {
                ctx.setStrokeColor(Self.color(c.fg))
                ctx.setLineWidth(th)
                ctx.move(to: CGPoint(x: l, y: ch - 2 * th))
                for i in 0..<4 {
                    ctx.addLine(to: CGPoint(x: l + CGFloat(i + 1) * cw / 4,
                                            y: ch - (i % 2 == 1 ? 2 : 1) * th - th / 2))
                }
                ctx.strokePath()
            }
            if f & Int(DMC_CELL_STRIKE) != 0 {
                ctx.fill(CGRect(x: l, y: floor(ch / 2), width: cw, height: th))
            }
            if f & Int(DMC_CELL_OVERLINE) != 0 {
                ctx.fill(CGRect(x: l, y: 0, width: cw, height: th))
            }
        }
        // the pictures over the text
        if anyImage {
            for x in 0..<ncols where Int(cells[x].flags) & Int(DMC_CELL_IMAGE) != 0 {
                var im = dmc_image()
                guard let px = dmc_vt_cell_image(vt, vrow, Int32(x), &im),
                      im.cell_width > 0, im.cell_height > 0 else { continue }
                var img = pictures[im.id]
                if img == nil {
                    img = Self.image(px, Int(im.width), Int(im.height))
                    pictures[im.id] = img
                }
                guard let img else { continue }
                let sx = cw / CGFloat(im.cell_width), sy = ch / CGFloat(im.cell_height)
                ctx.saveGState()
                ctx.clip(to: CGRect(x: CGFloat(x) * cw, y: 0, width: cw, height: ch))
                Self.draw(img, in: CGRect(x: CGFloat(x) * cw - CGFloat(im.tx) * cw,
                                          y: -CGFloat(im.ty) * ch,
                                          width: CGFloat(im.width) * sx,
                                          height: CGFloat(im.height) * sy), ctx)
                ctx.restoreGState()
            }
        }
        // the cursor
        if cursor {
            let cx = Int(dmc_vt_cursor_col(vt))
            if cx >= 0 && cx < ncols {
                let w = Int(cells[cx].flags) & Int(DMC_CELL_WIDE) != 0 ? 2 * cw : cw
                let r = CGRect(x: CGFloat(cx) * cw, y: 0, width: w, height: ch)
                let style = Int(dmc_vt_cursor_style(vt))
                let cc = Self.color(dmc_vt_cursor_color(vt))
                if window?.firstResponder !== self {
                    ctx.setStrokeColor(cc)
                    ctx.setLineWidth(1)
                    ctx.stroke(r.insetBy(dx: 0.5, dy: 0.5))
                } else if style == 3 || style == 4 {
                    ctx.setFillColor(cc)
                    ctx.fill(CGRect(x: r.minX, y: ch - max(2, floor(ch / 8)), width: w, height: max(2, floor(ch / 8))))
                } else if style == 5 || style == 6 {
                    ctx.setFillColor(cc)
                    ctx.fill(CGRect(x: r.minX, y: 0, width: max(2, floor(cw / 8)), height: ch))
                } else {
                    ctx.saveGState()
                    ctx.setBlendMode(.difference)
                    ctx.setFillColor(cc)
                    ctx.fill(r)
                    ctx.restoreGState()
                }
            }
        }
    }

    /// a row's machine-font glyphs at the font's size, stretched over it
    private func drawGlyphs(_ vt: OpaquePointer, _ cells: [dmc_cell], _ ctx: CGContext) {
        var gw: Int32 = 8, gh: Int32 = 16
        if dmc_vt_glyph_cell(vt, &gw, &gh) == 0 {
            // (soft characters only: their own size)
            for c in cells where Int(c.flags) & Int(DMC_CELL_GLYPH) != 0 {
                if dmc_vt_glyph(vt, c.ch, &gw, &gh) != 0 { break }
            }
        }
        let n = cells.count, W = n * Int(gw), H = Int(gh)
        guard W > 0, H > 0 else { return }
        var px = [UInt32](repeating: 0, count: W * H)
        for (x, c) in cells.enumerated() {
            let f = Int(c.flags)
            var w: Int32 = 0, h: Int32 = 0
            if f & Int(DMC_CELL_GLYPH) == 0 || f & Int(DMC_CELL_INVISIBLE) != 0 ||
                (f & Int(DMC_CELL_BLINK) != 0 && blinkOff) ||
                dmc_vt_glyph(vt, c.ch, &w, &h) == 0 || w <= 0 || h <= 0 { continue }
            let span = f & Int(DMC_CELL_WIDE) != 0 && x + 1 < n ? 2 : 1
            let fg = 0xff00_0000 | (c.fg & 0xffffff)
            let cellW = Int(gw) * span
            for y in 0..<H {
                let bits = dmc_vt_glyph_row(vt, c.ch, Int32(y * Int(h) / H))
                if bits == 0 { continue }
                for i in 0..<cellW where x * Int(gw) + i < W {
                    let gx = i * Int(w) / cellW
                    if (bits >> UInt32(Int(w) - 1 - gx)) & 1 != 0 {
                        px[y * W + x * Int(gw) + i] = fg
                    }
                }
            }
        }
        let img = px.withUnsafeBufferPointer { Self.image($0.baseAddress!, W, H) }
        if let img {
            Self.draw(img, in: CGRect(x: 0, y: 0, width: CGFloat(n) * cell.width, height: cell.height), ctx)
        }
    }

    // MARK: - the mouse

    private func send(_ buf: [CChar], _ n: Int32) {
        guard n > 0 else { return }
        host?.send(buf[0..<Int(n)].map { UInt8(bitPattern: $0) })
    }

    private func mods(_ f: NSEvent.ModifierFlags) -> Int32 {
        var m: Int32 = 0
        if f.contains(.shift) { m |= Int32(DMC_MOD_SHIFT) }
        if f.contains(.option) { m |= Int32(DMC_MOD_ALT) }
        if f.contains(.control) { m |= Int32(DMC_MOD_CTRL) }
        return m
    }

    private func mouseReport(_ e: NSEvent, _ button: Int, press: Bool, motion: Bool) -> Bool {
        guard let vt = host?.vt, dmc_vt_mouse_wanted(vt) != 0 else { return false }
        let p = convert(e.locationInWindow, from: nil)
        let col = max(0, Int((p.x - origin.x) / cell.width)), row = max(0, Int((p.y - origin.y) / cell.height))
        var buf = [CChar](repeating: 0, count: 64)
        let n = dmc_vt_mouse(vt, Int32(button), Int32(col), Int32(row), press ? 1 : 0, motion ? 1 : 0,
                             mods(e.modifierFlags), &buf, Int32(buf.count))
        send(buf, n)
        return true
    }

    override func mouseDown(with event: NSEvent) {
        window?.makeFirstResponder(self)
        buttons |= 1
        if mouseReport(event, 0, press: true, motion: false) { return }
        // a RIPscrip button's
        guard let vt = host?.vt else { return }
        let p = convert(event.locationInWindow, from: nil)
        let fx = Double((p.x - origin.x) / (CGFloat(dmc_vt_cols(vt)) * cell.width))
        let fy = Double((p.y - origin.y) / (CGFloat(dmc_vt_rows(vt)) * cell.height))
        var buf = [CChar](repeating: 0, count: 256)
        let n = dmc_vt_tap(vt, fx, fy, &buf, Int32(buf.count))
        if n > 0 {
            scroll = 0
            send(buf, n)
        }
    }

    override func mouseUp(with event: NSEvent) {
        buttons &= ~1
        _ = mouseReport(event, 0, press: false, motion: false)
    }

    override func mouseDragged(with event: NSEvent) {
        _ = mouseReport(event, 0, press: true, motion: true)
    }

    override func rightMouseDown(with event: NSEvent) {
        if mouseReport(event, 2, press: true, motion: false) {
            buttons |= 4
            return
        }
        super.rightMouseDown(with: event)
    }

    override func rightMouseUp(with event: NSEvent) {
        if buttons & 4 != 0 {
            buttons &= ~4
            _ = mouseReport(event, 2, press: false, motion: false)
        }
    }

    override func menu(for event: NSEvent) -> NSMenu? {
        if let vt = host?.vt, dmc_vt_mouse_wanted(vt) != 0 { return nil }
        let m = NSMenu()
        m.addItem(withTitle: "Paste", action: #selector(paste(_:)), keyEquivalent: "")
        return m
    }

    override func scrollWheel(with event: NSEvent) {
        guard let vt = host?.vt else { return }
        if event.scrollingDeltaY != 0 &&
            mouseReport(event, event.scrollingDeltaY > 0 ? 64 : 65, press: true, motion: false) { return }
        let d = Int((event.scrollingDeltaY / (event.hasPreciseScrollingDeltas ? cell.height : 1)).rounded())
        scroll = max(0, min(Int(dmc_vt_history(vt)), scroll + d))
        needsDisplay = true
    }

    // MARK: - keys

    override func keyDown(with event: NSEvent) {
        guard let vt = host?.vt else { return }
        let flags = event.modifierFlags
        if flags.contains(.command) {
            super.keyDown(with: event)
            return
        }
        scroll = 0
        let m = mods(flags)
        var buf = [CChar](repeating: 0, count: 64)
        if let k = special(event) {
            let n = dmc_vt_key_mods(vt, Int32(k), m, &buf, Int32(buf.count))
            send(buf, n)
            return
        }
        // Control and Option (as Meta) with a key: the terminal's controls,
        // ESC and it
        if m & Int32(DMC_MOD_CTRL | DMC_MOD_ALT) != 0,
           let c = event.charactersIgnoringModifiers?.unicodeScalars.first {
            let n = dmc_vt_char_bytes(vt, c.value, m & Int32(DMC_MOD_CTRL | DMC_MOD_ALT), &buf, Int32(buf.count))
            send(buf, n)
            return
        }
        guard let s = event.characters else { return }
        for c in s.unicodeScalars {
            let n = dmc_vt_char_bytes(vt, c.value, 0, &buf, Int32(buf.count))
            send(buf, n)
        }
    }

    private func special(_ e: NSEvent) -> Int? {
        switch Int(e.keyCode) {
        case 36: return Int(DMC_KEY_ENTER)
        case 76: return Int(DMC_KEY_KP_ENTER)
        case 51: return Int(DMC_KEY_BACKSPACE)
        case 53: return Int(DMC_KEY_ESCAPE)
        case 48: return e.modifierFlags.contains(.shift) ? Int(DMC_KEY_BACKTAB) : Int(DMC_KEY_TAB)
        default: break
        }
        guard let c = e.charactersIgnoringModifiers?.unicodeScalars.first else { return nil }
        switch Int(c.value) {
        case NSUpArrowFunctionKey: return Int(DMC_KEY_UP)
        case NSDownArrowFunctionKey: return Int(DMC_KEY_DOWN)
        case NSLeftArrowFunctionKey: return Int(DMC_KEY_LEFT)
        case NSRightArrowFunctionKey: return Int(DMC_KEY_RIGHT)
        case NSHomeFunctionKey: return Int(DMC_KEY_HOME)
        case NSEndFunctionKey: return Int(DMC_KEY_END)
        case NSPageUpFunctionKey: return Int(DMC_KEY_PGUP)
        case NSPageDownFunctionKey: return Int(DMC_KEY_PGDN)
        case NSInsertFunctionKey, NSHelpFunctionKey: return Int(DMC_KEY_INSERT)
        case NSDeleteFunctionKey: return Int(DMC_KEY_DELETE)
        case NSF1FunctionKey...NSF10FunctionKey:
            return Int(DMC_KEY_F1) + Int(c.value) - NSF1FunctionKey
        case NSF11FunctionKey: return Int(DMC_KEY_F11)
        case NSF12FunctionKey: return Int(DMC_KEY_F12)
        case NSF13FunctionKey...NSF20FunctionKey:
            return Int(DMC_KEY_F13) + Int(c.value) - NSF13FunctionKey
        case NSFindFunctionKey: return Int(DMC_KEY_FIND)
        case NSSelectFunctionKey: return Int(DMC_KEY_SELECT)
        default: return nil
        }
    }

    @objc func paste(_ sender: Any?) {
        guard let vt = host?.vt, let s = NSPasteboard.general.string(forType: .string) else { return }
        let bracket = dmc_vt_bracketed_paste(vt) != 0
        var out: [UInt8] = bracket ? Array("\u{1b}[200~".utf8) : []
        var buf = [CChar](repeating: 0, count: 64)
        // lines end in CR, as a terminal sends them
        let text = s.replacingOccurrences(of: "\r\n", with: "\r").replacingOccurrences(of: "\n", with: "\r")
        for c in text.unicodeScalars {
            let n = dmc_vt_char_bytes(vt, c.value, 0, &buf, Int32(buf.count))
            if n > 0 { out += buf[0..<Int(n)].map { UInt8(bitPattern: $0) } }
        }
        if bracket { out += Array("\u{1b}[201~".utf8) }
        scroll = 0
        host?.send(out)
    }
}
