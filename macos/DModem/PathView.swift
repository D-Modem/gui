// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 D-Modem Authors
//
// PathView.swift - the connection path at the top of the Settings page, as
// PCOMM's "Connection Overview" draws its link: each step's role above its
// icon, what it is below, one line joining them (libdmodemctl's dmc_path).
// Idle, how the line settings set it up; starting, calling and online, as
// it is now.

import SwiftUI

/// a step of the path, as dmc_path gives it
private struct PathStep: Identifiable {
    let id: Int
    let kind: Int32
    let state: Int32
    let role, title, detail: String
}

struct PathView: View {
    @EnvironmentObject var modem: Modem

    var body: some View {
        let (steps, links, headline, setup) = Self.make(modem)
        VStack(spacing: 8) {
            HStack(alignment: .top, spacing: 0) {
                ForEach(steps) { s in
                    StepView(
                        step: s,
                        left: s.id > 0 ? links[s.id - 1] : nil,
                        right: s.id < steps.count - 1 ? links[s.id] : nil
                    )
                    .frame(maxWidth: .infinity)
                }
            }
            Text(headline)
                .font(.callout)
                .foregroundStyle(setup ? Color.red : Color.secondary)
                .multilineTextAlignment(.center)
        }
        .padding(.vertical, 6)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(headline + ". " + steps.map { "\($0.role): \($0.title), \($0.detail)" }.joined(separator: ". "))
    }

    /// the steps, the links between them, the headline, and whether a step is still to set
    private static func make(_ modem: Modem) -> ([PathStep], [Int32], String, Bool) {
        let p = dmc_path_new()
        defer { dmc_path_free(p) }
        let c = modem.config
        let line: Int32
        let whereTo: String
        switch c.line {
        case .usb: line = Int32(DMC_LINE_USB); whereTo = c.usb
        case .device: line = Int32(DMC_LINE_DEVICE); whereTo = c.device
        case .sip: line = Int32(DMC_LINE_SIP); whereTo = ""
        }
        dmc_path_set_line(p, line, whereTo, c.sipAnonymous ? 1 : 0, c.sipUser, c.sipServer, c.listen ? 1 : 0)
        let st = Int32(modem.state)
        let online = st == Int32(DMC_ONLINE) || st == Int32(DMC_ONLINE_COMMAND)
        dmc_path_set_live(p, modem.running ? 1 : 0, st, online ? modem.detail : "")
        let n = Int(dmc_path_steps(p))
        var steps: [PathStep] = []
        var links: [Int32] = []
        for i in 0..<n {
            let k = Int32(i)
            steps.append(PathStep(
                id: i, kind: dmc_path_kind(p, k), state: dmc_path_state(p, k),
                role: cstr(dmc_path_role(p, k)), title: cstr(dmc_path_title(p, k)),
                detail: cstr(dmc_path_detail(p, k))
            ))
            if i < n - 1 { links.append(dmc_path_link(p, k)) }
        }
        return (steps, links, cstr(dmc_path_headline(p)), dmc_path_needs_setup(p) != 0)
    }
}

/// a step: its role, its icon on the line (its links to either side), its name and what it is
private struct StepView: View {
    let step: PathStep
    let left: Int32?
    let right: Int32?

    var body: some View {
        VStack(spacing: 4) {
            Text(step.role).font(.caption).foregroundStyle(.secondary).lineLimit(1)
            ZStack {
                // (the line through the icons' middles: half to the step before, half to the next)
                GeometryReader { g in
                    let y = g.size.height / 2
                    if let left {
                        Path { p in p.move(to: CGPoint(x: 0, y: y)); p.addLine(to: CGPoint(x: g.size.width / 2, y: y)) }
                            .stroke(Self.color(left), style: Self.stroke(left))
                    }
                    if let right {
                        Path { p in p.move(to: CGPoint(x: g.size.width / 2, y: y)); p.addLine(to: CGPoint(x: g.size.width, y: y)) }
                            .stroke(Self.color(right), style: Self.stroke(right))
                    }
                }
                Circle()
                    .fill(step.state == Int32(DMC_STEP_ACTIVE) ? Self.color(step.state).opacity(0.2) : Color(nsColor: .controlBackgroundColor))
                    .overlay(Circle().stroke(Self.color(step.state), lineWidth: 2))
                    .frame(width: 40, height: 40)
                Image(systemName: Self.symbol(step.kind))
                    .font(.system(size: 16, weight: .medium))
                    .foregroundStyle(Self.color(step.state))
            }
            .frame(height: 44)
            Text(step.title).font(.headline).multilineTextAlignment(.center).lineLimit(1)
            Text(step.detail)
                .font(.caption)
                .multilineTextAlignment(.center)
                .lineLimit(3)
                .foregroundStyle(step.state == Int32(DMC_STEP_SETUP) ? Color.red : Color.secondary)
        }
        .padding(.horizontal, 4)
    }

    /// a state's colour: ready the accent, to set red, dialling and online green, off faint
    static func color(_ s: Int32) -> Color {
        switch s {
        case Int32(DMC_STEP_SETUP): return .red
        case Int32(DMC_STEP_WORKING), Int32(DMC_STEP_ACTIVE): return .green
        case Int32(DMC_STEP_OFF): return Color(nsColor: .tertiaryLabelColor)
        default: return .accentColor
        }
    }

    /// a link's line: dashed where a step is still to set, or off
    static func stroke(_ s: Int32) -> StrokeStyle {
        let dashed = s == Int32(DMC_STEP_SETUP) || s == Int32(DMC_STEP_OFF)
        return StrokeStyle(lineWidth: 2, dash: dashed ? [6, 4] : [])
    }

    /// a step's SF Symbol
    static func symbol(_ k: Int32) -> String {
        switch k {
        case Int32(DMC_KIND_TERMINAL): return "terminal"
        case Int32(DMC_KIND_SOFTMODEM), Int32(DMC_KIND_MODEM): return "server.rack"
        case Int32(DMC_KIND_SIP): return "phone.arrow.up.right"
        case Int32(DMC_KIND_USB), Int32(DMC_KIND_SERIAL): return "cable.connector"
        case Int32(DMC_KIND_CODEC): return "waveform"
        case Int32(DMC_KIND_NETWORK): return "network"
        case Int32(DMC_KIND_SIP_ADDRESS): return "at"
        default: return "phone"
        }
    }
}
