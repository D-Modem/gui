# Design

## One library, three native shells

The apps share everything that is not drawing. `core/` is libdmodemctl, a
small C library with no I/O and no threads ("sans-I/O"): each app opens the
modem in its own way, hands the bytes it reads to `dmc_feed()`, writes what
`dmc_output()` gives it, and calls `dmc_poll()` from a 50 ms timer. That
keeps it easy to call from C (GTK), C++ (Win32) and Swift, and easy to test
(`core/test`, run by CI with ASan and UBSan).

| Part | What it does |
| --- | --- |
| `dmc_session` | The AT conversation: a queue of commands and their responses, the call's state from the result codes (RING, CONNECT, NO CARRIER), caller ID, the escape (`+++` with its guard times) and hanging up, and the byte stream for the terminal |
| `dmc_model` | The modem's settings from AT!M, with each setting's check and the command that sets it |
| `dmc_diag` | The last call's statistics from AT#UD, as labelled rows (the V.58 code tables decoded) |
| `dmc_vt` | The terminal emulator of the D-Modem app (D-Modem/app's `shared/.../terminal`), ported to C: every kind of terminal it is (see below), drawn by each app from its cells |
| `dmc_net` | Telnet (option negotiation, terminal type, window size) and rlogin (the handshake, window size on the server's urgent request), for the Remote page |
| `dmc_daemon_*` | Reading what dmodemd prints when it starts: the device it made, or its error |

## Where the settings come from

dmodemd's core has one settings model (`Config::model`), which already
drives its configurators (AT!CF, AT!CT, AT!C). AT!M prints that model as
tab-separated lines: each setting's key, kind (pick, number or text),
value, default, range, AT form, group, label and help, then each choice.
The Settings page builds its controls from it, so a setting added to the
modem shows in every app with no change here. A change is checked by the
library, sent as its AT command, and kept once the modem says OK.

The modem keeps no stored profile, so the page shows AT!R's init string
and its **Keep** button saves it in the app's line settings, to be sent
each time the modem opens.

## The same screens everywhere

Each app has the same seven pages, with the same words:

| Page | Contents |
| --- | --- |
| Modem | The state ("Idle", "Dialling", "Online") and its detail (the CONNECT line, the caller), Dial, Answer, Hang Up, the line and the device, Start or Stop |
| Phone | A voice call's state, Call and Answer, the keypad (DTMF), Mute and Listen; the answering machine (on or off, rings, the greeting) and its messages |
| Terminal | The AT console; the buttons **Full-screen setup** (AT!CF) and **Command-line setup** (AT!CT); the kind of terminal |
| Remote | SSH, Telnet or rlogin; the server, its port (empty: the protocol's) and the user; Connect; a terminal of its own, whose size goes to the server as it changes, and its kind |
| Settings | Groups of settings as native controls, changed ones marked; Defaults (ATZ), the init string and Keep |
| Call Stats | AT#UD's rows and ATI11's report, refreshed after each call |
| Log | What dmodemd printed, and the command line it was started with |

The terminal sends Backspace as BS (0x08), the modem's S5: it does not take
DEL. Colours are the brand's: background `#0a0f1c`, text `#e8f0ff`.

## The terminals

Both terminals (the modem's console and the Remote page's) are
libdmodemctl's `dmc_vt`: the app's emulator in C, with the app's 29 kinds
of terminal (profiles), picked beside each terminal and kept with the
page's settings:

| Kind | What it is |
| --- | --- |
| xterm (the default) | A VT220 with colours (8, 16, 256, 24-bit), sixel, the mouse, bracketed paste, the alternate screen, titles, OSC 8 links and Tek mode |
| DEC VT52 to VT525 | Each model's answers and sequences: the VT52's own, VT100 to VT320's levels, the VT340's sixel, ReGIS and Tek mode, the VT420's rectangles, margins and pages, the VT525's colours; soft fonts (DECDLD), user-defined keys, the printer port, the status line |
| ANSI-BBS, Avatar, RIPscrip, Amiga ANSI | A PC's BBS terminal in code page 437 and the VGA's font (SyncTERM's font switching, CSI SP D, among the machines' fonts), ANSI music, iCE colours, doorway mode; Avatar's codes; RIPscrip's graphics and mouse buttons on 80x43; the Amiga's Topaz and palette |
| Commodore 64, 128 (40 and 80 col.), Plus/4, VIC-20, PET (40 and 80) | PETSCII and each machine's screen, colours and ROM font |
| Atari 8-bit (and international), Atari ST (80 and 40 col.) | ATASCII; the ST's TOS VT52 with its pens |
| Viewdata, Minitel | Prestel's 40x24 with its mosaics; the Minitel's videotex and its Téléinformatique (a VT100) |
| Tektronix 4014 | The storage tube, on its own |

The library is sans-I/O as the rest: bytes in (`dmc_vt_feed`), answers
out (`dmc_vt_reply`), and each key or character typed turned into what
the kind of terminal sends (`dmc_vt_key_mods`, `dmc_vt_char_bytes`: a
Commodore's PETSCII, an Atari's ATASCII). A view draws each cell
(`dmc_vt_cell`): its character and resolved colours, its attributes, the
bitmap of a machine's font (`dmc_vt_glyph_row`: SyncTERM's copies of the
ROMs, generated into `core/src/vt_data.c` by `tools/gen_vt_data.py`) and
a picture's tile (`dmc_vt_cell_image`: sixel, ReGIS, RIPscrip, the
Tektronix). A machine whose screen has its own size (a C64's 40x25) asks
for it (`dmc_vt_requested_cols`), and the view stretches its cells to
fill the window, by whole pixels where they fit.

| | The view |
| --- | --- |
| Windows | `termview.cpp`: GDI, a row at a time into a DIB section (text by `ExtTextOutW`, glyphs and pictures by pixel), copied (or stretched, for DEC's double width and height) to the window |
| Linux | `termview.c`: a GtkDrawingArea drawn with Cairo and Pango (VTE is no longer used) |
| macOS | `TerminalView.swift`: an NSView drawn with Core Graphics |

## Starting dmodemd

| | Linux | macOS | Windows |
| --- | --- | --- | --- |
| Started with | GSubprocess | Process | CreateProcess |
| Its DTE | a pty (it prints "Use `/dev/pts/N' as modem device") | a pty | `--tcp 127.0.0.1:port` |
| Settings kept in | `~/.config/dmodem-gui/settings.ini` (mode 0600) | user defaults, the SIP password in the keychain | `%APPDATA%\D-Modem\dmodem.ini` |

The SIP account goes to dmodemd in its environment (`SIP_LOGIN`,
`SIP_T38`, `SIP_V152`, `SIP_V150`, `SIP_SRTP`); the country, USB modem and
debug level as flags. On Linux and macOS the pty is made in the user's
own group (`-g`), so that dmodemd needs no root.

Each app can instead use a modem that already runs: a device path on
Linux and macOS; a TCP address or a COM port on Windows.

## Remote logins

The Remote page logs in to a server, beside the modem, in a terminal of
its own. Telnet and rlogin are a TCP connection each app makes in its own
way, with libdmodemctl's `dmc_net` for the protocol (sans-I/O, as the
session). SSH is the system's OpenSSH client, so that the user's keys,
agent, `known_hosts` and `~/.ssh/config` apply:

| | Telnet and rlogin | SSH | Kept in |
| --- | --- | --- | --- |
| Linux | GSocketClient, its socket on the main loop (G_IO_PRI for rlogin's urgent byte) | `ssh` on a pty of the page's own (forkpty) | `settings.ini`, `[remote]` |
| Windows | Winsock 1.1, `WSAAsyncSelect` (FD_OOB for rlogin), on every Windows from 95 | Windows' `ssh.exe` (the OpenSSH Client feature) in a pseudo console: Windows 10 1809 and later, looked up at run time | `dmodem.ini`, `[remote]` |
| macOS | a BSD socket on a dispatch source | `/usr/bin/ssh` on a pty (`PtySpawn.c`) | UserDefaults |

SSH's TERM and rlogin's terminal are the kind of terminal's (`xterm-256color`,
`vt220`, `ansi`, `petscii` ...), and Telnet's TERMINAL-TYPE its own
(`XTERM-256COLOR`, `DEC-VT220`, `ANSI` ...: `dmc_vt_profile_term`,
`dmc_vt_profile_telnet`).

Telnet tells the server the terminal type and the window's size (NAWS),
and sends a typed CR as CR NUL. rlogin sends the user as both the local
and the remote name; a server that insists on a client port below 1024
(classic BSD `rlogind`) won't take an unprivileged app, but BBSes and
most rlogin services do. Backspace sends DEL, as a host expects (the
modem's console sends BS).

## Voice calls and the answering machine

Voice uses dmodemd's V.253 voice mode: `AT+FCLASS=8`, `AT+VSM=4` (G.711
μ-law at 8 kHz), then `ATD` or `AT+VLS=1`, and `AT+VTR` for the full-duplex
stream. libdmodemctl does all of it: the DLE framing and its events
(hangup, busy, dial tone, digits, caller ID), μ-law, the pacing of the
stream by the clock given to `dmc_poll`, DTMF, and the answering machine.
The apps only move 8 kHz 16-bit samples between it and the sound card
(`dmc_voice_read`, `dmc_voice_write`):

| | Sound | Settings | Messages | Greeting files beyond the library's |
| --- | --- | --- | --- | --- |
| Linux | PulseAudio on the GLib main loop (or PipeWire's Pulse server) | `settings.ini`, `[voice]` | `~/.local/share/dmodem-gui/voicemail` | libsndfile, when built with it: MP3, FLAC, Ogg Vorbis, Opus |
| Windows | `waveOut` and `waveIn` with window callbacks (Windows 95 has them) | `dmodem.ini`, `[voice]` | `%APPDATA%\D-Modem\Voicemail`, or beside the exe | the system's ACM codecs: MP3 and compressed WAVs (GSM 6.10 and the rest installed) |
| macOS | AVAudioEngine, converted to 8 kHz | UserDefaults | `~/Library/Application Support/D-Modem/Voicemail` | AVAudioFile: MP3, AAC/M4A, FLAC, ALAC, CAF |

The answering machine is off until it is turned on. When on, after the
chosen number of rings (4 by default) it answers, plays the greeting (or
none), beeps, and records until the caller hangs up, 7 s of silence, or
2 minutes. Each message is a WAV file named for its time and the caller's
number; one under a second is dropped. A greeting can be recorded from the
microphone or chosen from a sound file, which is converted to 8 kHz mono.
The library reads WAV (8 to 32-bit PCM, floats, A-law, μ-law, IMA and
Microsoft ADPCM), AIFF and AIFF-C, and Sun/NeXT `.au` itself
(`dmc_audio_decode`); each app hands the rest to the system's decoders,
as in the table.
Pick Up turns a message being taken into a call; Listen plays it as it is
left. Messages stay on this computer.

## Windows 95 and later

The Windows app is plain Win32 in C++98, so that one binary runs from
Windows 95 to Windows 11:

- ANSI APIs, with UTF-8 from the library converted by hand (no `CP_UTF8`
  on 9x); the terminal draws with `ExtTextOutW`, which 9x has.
- The Windows 95 common controls (tabs, list views, the status bar),
  Winsock 1.1 (`wsock32`, `WSAAsyncSelect`) and the 4.0 `OPENFILENAME`.
- No `SetHandleInformation` (NT only): the pipe from dmodemd is made
  non-inheritable with `DuplicateHandle`.
- Built by Open Watcom 2.0 (`windows/build-watcom.sh`: `runtime
  windows=4.0`, the static C library), as CI does; MinGW and MSVC build the
  same code with CMake.

dmodemd itself needs Windows 7 or later. On Windows 9x and NT 4 the app
defaults to a COM port, and can reach a dmodemd on another machine over TCP.
