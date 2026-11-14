# Design

## One library, three native shells

The apps share all the code that does not draw. `core/` is libdmodemctl, a
small C library with no I/O and no threads ("sans-I/O"). Each app opens the
modem in its own way. Then it gives the bytes that it reads to `dmc_feed()`,
writes the data that `dmc_output()` gives it, and calls `dmc_poll()` from a
50 ms timer. Thus the library is easy to call from C (GTK), C++ (Win32) and
Swift. It is also easy to test (`core/test`, which CI runs with ASan and
UBSan).

| Part | What it does |
| --- | --- |
| `dmc_session` | The AT conversation: a queue of commands and their responses, the state of the call from the result codes (RING, CONNECT, NO CARRIER), caller ID, the escape (`+++` with its guard times) and the hang-up, and the byte stream for the terminal |
| `dmc_model` | The settings of the modem from AT!M, with the check of each setting and the command that sets it |
| `dmc_diag` | The statistics of the last call from AT#UD, as labelled rows (with the V.58 code tables decoded) |
| `dmc_vt` | The terminal emulator of the D-Modem app (`shared/.../terminal` of D-Modem/app), ported to C: each type of terminal that it emulates (see below). Each app draws it from its cells |
| `dmc_net` | Telnet (option negotiation, terminal type, window size) and rlogin (the handshake, and the window size on the urgent request of the server), for the Remote page |
| `dmc_daemon_*` | It reads the text that dmodemd prints when it starts: the device that it made, or its error |

## Where the settings come from

The core of dmodemd has one settings model (`Config::model`). This model
also controls its configurators (AT!CF, AT!CT, AT!C). AT!M prints that model
as tab-separated lines. Each line has the key of a setting, its kind (pick,
number or text), value, default, range, AT form, group, label and help, and
then each choice. The Settings page builds its controls from this output.
Thus, if a setting is added to the modem, it shows in each app without a
change here. The library checks a change and sends it as its AT command. The
app keeps the change when the modem says OK.

The modem keeps no stored profile. Thus the page shows the init string of
AT!R. Its **Keep** button saves the string in the line settings of the app.
The app sends the string each time that the modem opens.

## The same screens everywhere

Each app has the same seven pages, with the same words:

| Page | Contents |
| --- | --- |
| Modem | The state ("Idle", "Dialling", "Online") and its detail (the CONNECT line, the caller), Dial, Answer, Hang Up, the line and the device, Start or Stop |
| Phone | The state of a voice call, Call and Answer, the keypad (DTMF), Mute and Listen. The answering machine (on or off, rings, the greeting) and its messages |
| Terminal | The AT console. The buttons **Full-screen setup** (AT!CF) and **Command-line setup** (AT!CT). The type of terminal |
| Remote | SSH, Telnet or rlogin. The server, its port (empty: the port of the protocol) and the user. The login method of SSH (keys and the agent, the agent only, a key file, the import of a PuTTY key). Connect. A separate terminal, with its type. The terminal sends its size to the server when the size changes |
| Settings | Groups of settings as native controls, with the changed settings marked. Defaults (ATZ), the init string and Keep |
| Call Stats | The rows of AT#UD and the report of ATI11, refreshed after each call |
| Log | The text that dmodemd printed, and the command line that started it |

The terminal sends Backspace as BS (0x08), the S5 of the modem, because the
modem does not accept DEL. The colours are the colours of the brand:
background `#0a0f1c`, text `#e8f0ff`.

## The terminals

The two terminals (the console of the modem and the terminal of the Remote
page) use `dmc_vt` of libdmodemctl. This is the emulator of the app in C,
with the 29 types of terminal (profiles) of the app. You select the type
next to each terminal, and the app keeps it with the settings of the page:

| Kind | What it is |
| --- | --- |
| xterm (the default) | A VT220 with colours (8, 16, 256, 24-bit), sixel, the mouse, bracketed paste, the alternate screen, titles, OSC 8 links and Tek mode |
| DEC VT52 to VT525 | The answers and sequences of each model: the sequences of the VT52, the levels of the VT100 to VT320, the sixel, ReGIS and Tek mode of the VT340, the rectangles, margins and pages of the VT420, the colours of the VT525. Soft fonts (DECDLD), user-defined keys, the printer port, the status line |
| ANSI-BBS, Avatar, RIPscrip, Amiga ANSI | The BBS terminal of a PC in code page 437 and the font of the VGA (the SyncTERM font switch, CSI SP D, with the fonts of the machines), ANSI music, iCE colours, doorway mode. The codes of Avatar. The graphics and mouse buttons of RIPscrip on 80x43. The Topaz and palette of the Amiga |
| Commodore 64, 128 (40 and 80 col.), Plus/4, VIC-20, PET (40 and 80) | PETSCII, and the screen, colours and ROM font of each machine |
| Atari 8-bit (and international), Atari ST (80 and 40 col.) | ATASCII. The TOS VT52 of the ST with its pens |
| Viewdata, Minitel | The 40x24 of Prestel with its mosaics. The videotex of the Minitel and its Téléinformatique (a VT100) |
| Tektronix 4014 | The storage tube, as a separate type |

The library is sans-I/O, as the remaining parts are. Bytes go in
(`dmc_vt_feed`) and answers come out (`dmc_vt_reply`). The library changes
each typed key or character into the bytes that the type of terminal sends
(`dmc_vt_key_mods`, `dmc_vt_char_bytes`: the PETSCII of a Commodore, the
ATASCII of an Atari). A view draws each cell (`dmc_vt_cell`) with these
items:

- Its character and resolved colours, and its attributes.
- The bitmap of the font of a machine (`dmc_vt_glyph_row`). These are the
  SyncTERM copies of the ROMs, generated into `core/src/vt_data.c` by
  `tools/gen_vt_data.py`.
- The tile of a picture (`dmc_vt_cell_image`: sixel, ReGIS, RIPscrip, the
  Tektronix).

If the screen of a machine has its own size (40x25 for a C64), the library
requests that size (`dmc_vt_requested_cols`). The view stretches its cells
to fill the window, by whole pixels where they fit.

| | The view |
| --- | --- |
| Windows | `termview.cpp`: GDI, one row at a time into a DIB section (text by `ExtTextOutW`, glyphs and pictures by pixel). The view copies the section to the window (or stretches it, for the double width and height of DEC) |
| Linux | `termview.c`: a GtkDrawingArea drawn with Cairo and Pango (the app does not use VTE now) |
| macOS | `TerminalView.swift`: an NSView drawn with Core Graphics |

## How the apps start dmodemd

| | Linux | macOS | Windows |
| --- | --- | --- | --- |
| Started with | GSubprocess | Process | CreateProcess |
| Its DTE | a pty (it prints "Use `/dev/pts/N' as modem device") | a pty | `--tcp 127.0.0.1:port` |
| Settings kept in | `~/.config/dmodem-gui/settings.ini` (mode 0600) | user defaults, the SIP password in the keychain | `%APPDATA%\D-Modem\dmodem.ini` |

The app gives the SIP account to dmodemd in its environment (`SIP_LOGIN`,
`SIP_T38`, `SIP_V152`, `SIP_V150`, `SIP_SRTP`). It gives the country, the
USB modem and the debug level as flags. On Linux and macOS, the app makes
the pty in the group of the user (`-g`). Thus dmodemd does not need root.

Each app can also use a modem that already runs: a device path on Linux and
macOS, or a TCP address or a COM port on Windows.

## Remote logins

The Remote page logs in to a server, next to the modem, in a separate
terminal. Telnet and rlogin are a TCP connection that each app makes in its
own way. The app uses `dmc_net` of libdmodemctl for the protocol (sans-I/O,
as the session). SSH is libdmodemssh (`ssh/`), the client of D-Modem on
libssh 0.11. The Android and iOS apps use the same engine. Thus SSH is the
same on all platforms, and you do not need to install anything:

- One thread for each connection calls `dmc_ssh_poll()` and gives each event
  to the thread of the UI. The UI sends typed text, the window size and the
  answers directly to the connection (it locks).
- The app checks the key of the host against `known_hosts` of OpenSSH. If
  the key is known, the login continues. If the key is new, the app shows it
  (its SHA256 fingerprint) and adds it if the user accepts it. If the key
  changed, the app refuses it with a warning.
- The login uses the order of OpenSSH: first the agent and the keys of
  `~/.ssh`, then keyboard-interactive or a password (three tries), in the
  dialogs of the app. The app reads `~/.ssh/config` (host aliases, users,
  ports).
- Alternatively, you can select next to the server (the key menu on Linux,
  Login... on Windows, the key popover on macOS) one of these: the keys of
  the agent only, or a key file. The key file can be in the format of
  OpenSSH, PEM or the `.ppk` of PuTTY (versions 2 and 3). `ssh/src/ppk.c`
  reads `.ppk` files, with Argon2, the same with each libssh build. The app
  asks for the passphrase in its dialog. You can also import a `.ppk`. The
  app writes it to `.ssh` in the OpenSSH format with the same passphrase,
  and selects it.
- On Linux and macOS, the agent is the socket of an ssh-agent
  (`SSH_AUTH_SOCK`, or a socket that you give, as `IdentityAgent`). On
  Windows, it is the Pageant of PuTTY (the app asks it by WM_COPYDATA, as the
  tools of PuTTY do). If Pageant is not available, it is the agent of OpenSSH
  (`\\.\pipe\openssh-ssh-agent`), or a named pipe that you give.
  `ssh/src/agent.c` connects libssh to those two agents with a pair of
  loopback sockets and a thread.
- Ed25519, ECDSA and RSA keys. A shell on a pty with TERM `xterm-256color`.
  The app sends the size of the pty when it changes. The engine also has
  commands, direct channels (`-L`, `-D`), remote listens (`-R`), agent
  channels (`-A`), and it can make keys or encrypt them again. The mobile
  apps use these functions.

If the system has libssh 0.11 or later (Linux), libssh and mbedTLS come from
the system. If not, the build makes them from their release tarballs, and
checks them by hash (`ssh/cmake/BundledLibssh.cmake`). This always occurs on
Windows and macOS. The Windows 95 build (Open Watcom) has no libssh. If it
finds the `ssh.exe` of Windows (Windows 10 1809 and later), it runs it in a
pseudo console.

| | Telnet and rlogin | SSH | Kept in |
| --- | --- | --- | --- |
| Linux | GSocketClient, its socket on the main loop (G_IO_PRI for the urgent byte of rlogin) | libdmodemssh on a GThread, events by `g_main_context_invoke`, AdwAlertDialogs | `settings.ini`, `[remote]` |
| Windows | Winsock 1.1, `WSAAsyncSelect` (FD_OOB for rlogin), on all Windows versions from 95 | libdmodemssh on a thread, posted events (`WM_APP_SSH`), the `.ssh` of the profile | `dmodem.ini`, `[remote]` |
| macOS | a BSD socket on a dispatch source | libdmodemssh (`SSHConnection.swift`) on a thread, events on the main queue, SwiftUI alerts | UserDefaults |

The TERM of SSH and the terminal of rlogin come from the type of terminal
(`xterm-256color`, `vt220`, `ansi`, `petscii` ...). The TERMINAL-TYPE of
Telnet is a separate value
(`XTERM-256COLOR`, `DEC-VT220`, `ANSI` ...: `dmc_vt_profile_term`,
`dmc_vt_profile_telnet`).

Telnet tells the server the terminal type and the window size (NAWS). It
sends a typed CR as CR NUL. rlogin sends the user as the local name and as
the remote name. Some servers accept only a client port below 1024 (classic
BSD `rlogind`). These servers do not accept an unprivileged app, but BBSes
and most rlogin services accept it. Backspace sends DEL, as a host expects
(the console of the modem sends BS).

## Voice calls and the answering machine

Voice uses the V.253 voice mode of dmodemd: `AT+FCLASS=8`, `AT+VSM=4` (G.711
μ-law at 8 kHz), then `ATD` or `AT+VLS=1`, and `AT+VTR` for the full-duplex
stream. libdmodemctl does all of this work. It does the DLE framing and its
events (hangup, busy, dial tone, digits, caller ID), μ-law, the pacing of
the stream by the clock that the app gives to `dmc_poll`, DTMF, and the
answering machine. The apps only move 8 kHz 16-bit samples between the
library and the sound card (`dmc_voice_read`, `dmc_voice_write`):

| | Sound | Settings | Messages | Greeting files beyond the library's |
| --- | --- | --- | --- | --- |
| Linux | PulseAudio on the GLib main loop (or the Pulse server of PipeWire) | `settings.ini`, `[voice]` | `~/.local/share/dmodem-gui/voicemail` | libsndfile, if the build includes it: MP3, FLAC, Ogg Vorbis, Opus |
| Windows | `waveOut` and `waveIn` with window callbacks (Windows 95 has them) | `dmodem.ini`, `[voice]` | `%APPDATA%\D-Modem\Voicemail`, or next to the exe | the ACM codecs of the system: MP3 and compressed WAVs (GSM 6.10 and the other installed codecs) |
| macOS | AVAudioEngine, converted to 8 kHz | UserDefaults | `~/Library/Application Support/D-Modem/Voicemail` | AVAudioFile: MP3, AAC/M4A, FLAC, ALAC, CAF |

The answering machine is disabled until you enable it. When it is enabled,
it answers after the selected number of rings (4 by default). It plays the
greeting (or no greeting) and a beep. Then it records until one of these
conditions occurs: the caller hangs up, 7 s of silence, or 2 minutes. Each
message is a WAV file. Its name is its time and the number of the caller.
The app deletes a message that is shorter than one second.

You can record a greeting from the microphone, or select a sound file. The
app converts the file to 8 kHz mono. The library itself reads WAV (8 to
32-bit PCM, floats, A-law, μ-law, IMA and Microsoft ADPCM), AIFF and AIFF-C,
and Sun/NeXT `.au` (`dmc_audio_decode`). Each app gives all other files to
the decoders of the system, as the table shows. Pick Up changes a message
into a call while the caller leaves it. Listen plays the message while the
caller leaves it. Messages stay on this computer.

## Windows 95 and later

The Windows app is plain Win32 in C++98. Thus one binary runs from
Windows 95 to Windows 11:

- ANSI APIs. The app converts the UTF-8 from the library with its own code
  (9x has no `CP_UTF8`). The terminal draws with `ExtTextOutW`, which 9x has.
- The Windows 95 common controls (tabs, list views, the status bar),
  Winsock 1.1 (`wsock32`, `WSAAsyncSelect`) and the 4.0 `OPENFILENAME`.
- No `SetHandleInformation` (NT only). The app uses `DuplicateHandle` to
  make the pipe from dmodemd non-inheritable.
- Open Watcom 2.0 builds the app (`windows/build-watcom.sh`: `runtime
  windows=4.0`, the static C library), as CI does. MinGW and MSVC build the
  same code with CMake.

dmodemd itself needs Windows 7 or later. On Windows 9x and NT 4, the app
uses a COM port by default. It can also connect to a dmodemd on another
machine through TCP.
