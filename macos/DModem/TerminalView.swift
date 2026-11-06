// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, D-Modem
//
// TerminalView.swift - the modem's console and its full-screen setup:
// libdmodemctl's terminal screen (dmc_vt), drawn cell by cell

import AppKit
import SwiftUI

struct TerminalPane: View {
    @EnvironmentObject var modem: Modem

    var body: some View {
        VStack(spacing: 0) {
            HStack {
                Button("Full-screen setup") { modem.type("AT!CF\r") }
                    .help("The configurator in screens and menus (AT!CF)")
                Button("Command-line setup") { modem.type("AT!CT\r") }
                    .help("The configurator as commands, Cisco style (AT!CT)")
                Spacer()
                Button("Clear") { modem.clearScreen() }
            }
            .disabled(!modem.isIdle)
            .padding(8)
            TerminalView(modem: modem, version: modem.screenVersion)
        }
    }
}

struct TerminalView: NSViewRepresentable {
    let modem: Modem
    let version: Int

    func makeNSView(context: Context) -> TerminalNSView {
        let v = TerminalNSView()
        v.modem = modem
        return v
    }

    func updateNSView(_ v: TerminalNSView, context: Context) {
        v.modem = modem
        v.needsDisplay = true
    }
}

final class TerminalNSView: NSView {
    weak var modem: Modem?
    private let font = NSFont(name: "Menlo", size: 13) ?? .monospacedSystemFont(ofSize: 13, weight: .regular)
    private lazy var bold = NSFontManager.shared.convert(font, toHaveTrait: .boldFontMask)
    private var cell = NSSize(width: 8, height: 16)
    private var scroll = 0          // rows up into the scrollback

    static let background = NSColor(srgbRed: 0x0a / 255, green: 0x0f / 255, blue: 0x1c / 255, alpha: 1)
    static let foreground = NSColor(srgbRed: 0xe8 / 255, green: 0xf0 / 255, blue: 0xff / 255, alpha: 1)

    override init(frame: NSRect) {
        super.init(frame: frame)
        let w = ("M" as NSString).size(withAttributes: [.font: font]).width
        cell = NSSize(width: ceil(w), height: ceil(font.ascender - font.descender + font.leading))
    }

    required init?(coder: NSCoder) { fatalError("not used") }

    override var isFlipped: Bool { true }
    override var acceptsFirstResponder: Bool { true }

    override func viewDidMoveToWindow() {
        window?.makeFirstResponder(self)
    }

    override func mouseDown(with event: NSEvent) {
        window?.makeFirstResponder(self)
    }

    override func setFrameSize(_ size: NSSize) {
        super.setFrameSize(size)
        guard let vt = modem?.vt else { return }
        let cols = max(20, min(160, Int((size.width - 8) / cell.width)))
        let rows = max(5, min(100, Int((size.height - 8) / cell.height)))
        if cols != Int(dmc_vt_cols(vt)) || rows != Int(dmc_vt_rows(vt)) {
            dmc_vt_resize(vt, Int32(cols), Int32(rows))
        }
    }

    private func colour(_ c: Int32, _ fallback: NSColor) -> NSColor {
        guard c >= 0 && c < 16 else { return fallback }
        let rgb = dmc_vt_palette(c)
        return NSColor(srgbRed: CGFloat((rgb >> 16) & 0xff) / 255,
                       green: CGFloat((rgb >> 8) & 0xff) / 255,
                       blue: CGFloat(rgb & 0xff) / 255, alpha: 1)
    }

    override func draw(_ dirty: NSRect) {
        Self.background.setFill()
        bounds.fill()
        guard let vt = modem?.vt else { return }
        let rows = Int(dmc_vt_rows(vt)), cols = Int(dmc_vt_cols(vt))
        scroll = min(scroll, Int(dmc_vt_history(vt)))
        let ox: CGFloat = 4, oy: CGFloat = 4
        for y in 0..<rows {
            let row = Int32(y - scroll)
            var x = 0
            while x < cols {
                let fg = dmc_vt_fg(vt, row, Int32(x)), bg = dmc_vt_bg(vt, row, Int32(x))
                let at = Int(dmc_vt_attr(vt, row, Int32(x)))
                var text = ""
                var n = 0
                while x + n < cols &&
                        dmc_vt_fg(vt, row, Int32(x + n)) == fg &&
                        dmc_vt_bg(vt, row, Int32(x + n)) == bg &&
                        Int(dmc_vt_attr(vt, row, Int32(x + n))) == at {
                    let c = dmc_vt_char(vt, row, Int32(x + n))
                    text.unicodeScalars.append(Unicode.Scalar(c) ?? "?")
                    n += 1
                }
                // bold as the bright colours, as a PC's text mode
                let f = fg >= 0 && fg < 8 && at & Int(DMC_BOLD) != 0 ? fg + 8 : fg
                var fc = colour(f, Self.foreground), bc = colour(bg, Self.background)
                if at & Int(DMC_REVERSE) != 0 { swap(&fc, &bc) }
                let r = NSRect(x: ox + CGFloat(x) * cell.width, y: oy + CGFloat(y) * cell.height,
                               width: CGFloat(n) * cell.width, height: cell.height)
                if bc != Self.background {
                    bc.setFill()
                    r.fill()
                }
                var attrs: [NSAttributedString.Key: Any] = [
                    .font: at & Int(DMC_BOLD) != 0 ? bold : font, .foregroundColor: fc]
                if at & Int(DMC_UNDERLINE) != 0 {
                    attrs[.underlineStyle] = NSUnderlineStyle.single.rawValue
                }
                // each character in its cell, whatever the font
                for (i, ch) in text.enumerated() where ch != " " {
                    (String(ch) as NSString).draw(
                        at: NSPoint(x: r.minX + CGFloat(i) * cell.width, y: r.minY),
                        withAttributes: attrs)
                }
                if at & Int(DMC_UNDERLINE) != 0 && text.allSatisfy({ $0 == " " }) {
                    fc.setFill()
                    NSRect(x: r.minX, y: r.maxY - 1, width: r.width, height: 1).fill()
                }
                x += n
            }
        }
        if dmc_vt_cursor_visible(vt) != 0 && scroll == 0 {
            let r = NSRect(x: ox + CGFloat(dmc_vt_cursor_col(vt)) * cell.width,
                           y: oy + CGFloat(dmc_vt_cursor_row(vt)) * cell.height,
                           width: cell.width, height: cell.height)
            Self.foreground.withAlphaComponent(window?.firstResponder === self ? 0.7 : 0.3).setFill()
            r.fill(using: .sourceOver)
        }
    }

    override func scrollWheel(with event: NSEvent) {
        guard let vt = modem?.vt else { return }
        let d = Int((event.scrollingDeltaY / (event.hasPreciseScrollingDeltas ? cell.height : 1)).rounded())
        scroll = max(0, min(Int(dmc_vt_history(vt)), scroll + d))
        needsDisplay = true
    }

    // MARK: - keys

    override func keyDown(with event: NSEvent) {
        guard let modem else { return }
        scroll = 0
        let mods = event.modifierFlags
        if mods.contains(.command) {
            super.keyDown(with: event)
            return
        }
        if let k = special(event) {
            modem.key(k)
            return
        }
        guard var s = event.characters, !s.isEmpty else { return }
        // Option as Meta: ESC and the key
        if mods.contains(.option), let plain = event.charactersIgnoringModifiers {
            s = "\u{1b}" + plain
        }
        modem.type(s)
    }

    private func special(_ e: NSEvent) -> Int? {
        switch Int(e.keyCode) {
        case 36, 76: return Int(DMC_KEY_ENTER)
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
        default: return nil
        }
    }

    @objc func paste(_ sender: Any?) {
        guard let s = NSPasteboard.general.string(forType: .string) else { return }
        // lines end in CR, as the modem takes them
        modem?.type(s.replacingOccurrences(of: "\r\n", with: "\r")
                        .replacingOccurrences(of: "\n", with: "\r"))
    }

    override func menu(for event: NSEvent) -> NSMenu? {
        let m = NSMenu()
        m.addItem(withTitle: "Paste", action: #selector(paste(_:)), keyEquivalent: "")
        return m
    }
}
