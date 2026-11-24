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
| `dmc_path` | The connection path at the top of the Settings page: the terminal, the modem, the line and what the line reaches. For each step, its role, name, state and setting, from the line settings and the state of the session |
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
| Settings | The connection path. Groups of settings as native controls, with the changed settings marked. Defaults (ATZ), the init string and Keep |
| Call Stats | The rows of AT#UD and the report of ATI11, refreshed after each call |
| Log | The text that dmodemd printed, and the command line that started it |

The terminal sends Backspace as BS (0x08), the S5 of the modem, because the
modem does not accept DEL. The colours are the colours of the brand:
background `#0a0f1c`, text `#e8f0ff`.

## The connection path

The top of the Settings page shows how a call is made, as the "Connection
Overview" of PCOMM draws its link: four steps joined by one line. Each step
has its role above its icon and its setting below.

| Line | Program | Modem (or Port) | Line (or Modem) | Reaches (or Line) |
| --- | --- | --- | --- | --- |
| SIP account | Terminal | D-Modem | SIP: the account, registered when the modem takes calls | The phone network, through the server |
| SIP, anonymous | Terminal | D-Modem | SIP: nothing registered | SIP addresses, dialled directly |
| USB modem | Terminal | D-Modem | The USB modem | Its phone line |
| Codec (Linux) | Terminal | D-Modem | The ALSA device | Its phone line |
| A modem that already runs | Terminal | Its device, TCP address or COM port | The modem | Its phone line |

When idle, each step shows the line settings. A step that is ready is in the
accent colour. A step that you must still set (no SIP account, no device) is
red and its line is dashed, and the text below says what to set. When the
modem starts, dials and is online, the steps light green as the call goes,
and the text below shows the CONNECT line. `dmc_path` of libdmodemctl has the
words, so the three apps show the same text. Each app draws them:
`pathview.c`, `pathview.cpp` (GDI, with the Wingdings of Windows 95) and
`PathView.swift`. The D-Modem app has the same picture at the top of its
Settings, with its own lines (Wi-Fi and Bluetooth to another phone, the
mobile calls of the phone, external modems).

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

### Copy, capture and print (Windows app)

The Windows terminal control (`termview.cpp`) keeps a selection. The
selection uses the rows of `dmc_vt_cell` (a row below 0 is in the
scrollback). The selection moves up with its text when new lines push the
text into the scrollback.

- Drag with the left mouse button to select. Double-click to select a word.
  When the remote side uses the mouse, hold Shift.
- Copy (Ctrl+Insert, or the Edit menu, or the menu of the right mouse
  button) puts the text on the clipboard. The text comes from
  `dmc_vt_text_range`. The app removes the blanks at the end of each line.
  The clipboard gets Unicode text and the text of the ANSI code page.
- Paste to Host (Shift+Insert) types the clipboard text.

`ascii.cpp` has the ASCII Setup dialog (Modem menu). `dmodem.ini` keeps
the settings in the `[ascii]` section: `send_lf`, `echo`, `line_delay`,
`char_delay`, `add_lf`, `seven_bit` and `wrap`. The app applies them to
the two terminals:

- `send_lf` sets Enter to CR LF (`dmc_vt_set_enter`).
- `echo` shows the characters that you type on the terminal. The app does
  not show the escape sequences of the keys (for example, the arrow keys).
- `add_lf`, `seven_bit` and `wrap` go to `dmc_vt_set_ascii`. With
  `DMC_VT_ASCII_ADD_LF`, the terminal adds an LF after each CR that it
  receives. When an LF follows the CR, the terminal does not use that LF,
  thus a CR LF does not give two lines. `DMC_VT_ASCII_7BIT` removes bit 8
  of each byte. `DMC_VT_ASCII_NO_WRAP` stops the wrap at the right margin:
  the terminal writes each new character in the last column.

`termset.cpp` has the Terminal Settings dialog (Modem menu). It applies
to the terminal of the page that shows: the Remote page's, else the
modem's. `dmodem.ini` keeps the settings in `[terminal]` (the modem) and
`[remote_terminal]` (Remote). `termview_set_options` applies them:

- `backspace`: 0 sends Ctrl+H, 1 sends Del, 2 sends Ctrl+H, Space,
  Ctrl+H. The default of the modem is 0 (the configurator of the modem
  uses Ctrl+H). The default of Remote is 1.
- `windows_keys`: Ctrl+C copies, Ctrl+V pastes, Ctrl+A selects all. The
  arrow keys, Page Up and Page Down scroll the backscroll. The function
  keys go to the window (F10 opens the menu). The terminal does not send
  these keys.
- `backscroll`: the lines that the terminal keeps
  (`dmc_vt_set_scrollback`). When the number is smaller, the terminal
  keeps the newest lines.
- `font` and `font_size`: a fixed-pitch font. When the name is empty, the
  terminal uses the font of the app.
- `cursor` (0 block, 1 underline) and `cursor_blink`. A DECSCUSR from the
  remote side has priority.
- `telnet_id` (Remote only): the TERMINAL-TYPE that Telnet sends. When it
  is empty, Telnet sends the name of the terminal type.
- `exit_on_disconnect` (modem only): the app closes when a call ends.

The status bar shows the connect timer during a call ("Online 0:01:05").
Send Break (Call menu, or Ctrl+Break in a terminal) sends a break. On a
COM port, the app holds the line at space for 250 ms (`SetCommBreak`). On
Telnet, the app sends IAC BRK (`dmc_net_break`). TCP to dmodemd, rlogin
and SSH cannot send a break, and the status bar tells you.

`calls.cpp` has the Call menu, as HyperTerminal has it:

- Call opens the Call dialog. A session is a name and a number (or a SIP
  address). `dmodem.ini` keeps the sessions in the `[sessions]` section
  (`count`, `name1`, `number1` ...). Save adds or changes a session, and
  Delete removes it. Call dials the number (ATD) and shows the Terminal
  page.
- Wait for a Call sends ATS0=1: the modem answers at the first ring. The
  app sends ATS0=1 again each time the modem opens. When you clear the
  check mark, the app sends ATS0=0.
- Disconnect hangs up the call (`dmc_hangup`), or closes the Remote
  connection when the Remote page shows.

The Line Settings dialog has the settings of a COM port, as the Configure
dialog of HyperTerminal: bits per second (110 to 921,600), data bits (5 to
8), parity (none, odd, even, mark, space), stop bits (1, 1.5, 2) and flow
control (none, Xon/Xoff, hardware). `dmodem.ini` keeps them as `com-bps`,
`com-data-bits`, `com-parity`, `com-stop-bits` and `com-flow`. The default
is 115,200 bps, 8 data bits, no parity, 1 stop bit and hardware flow
control.

`capture.cpp` has the captures. Each capture is of one line (the modem or
the Remote login). The capture does not get the bytes of a file transfer.

- Capture Text writes the bytes from the line to a file, as they come
  (with the escape sequences). The app adds the bytes to the end of a file
  that exists, as HyperTerminal does. `dmodem.ini` keeps the folder in the
  `[capture]` section.
- Capture to Printer keeps the text from the line. The app removes the
  escape sequences and the control characters (but not tabs, line ends and
  form feeds). When you stop the capture, the app prints the text on the
  default printer.
- Print (Modem menu) prints the screen or the selection, with the print
  dialog.
- When the remote side prints (`CSI 5 i`, `dmc_vt_printed`), the app prints
  the text on the default printer after the remote side stops for 3
  seconds. During Capture to Printer, this text goes into the capture.

The app prints with GDI: Courier New at 10 points, with margins of half an
inch. The app wraps long lines, and a form feed starts a new page.

## File transfers

The file transfer protocols are in libdmodemctl (`core/src/xfer*.c`). They
are a C port of the protocols of the D-Modem app (`ZModem.kt`,
`XYModem.kt`, `Kermit.kt`, `HexRecords.kt` and `TextTransfer.kt`). The
protocols are:

- ZMODEM: CRC-16 and CRC-32 headers, ZDLE escapes, streaming data (ZCRCG)
  with a ZCRCQ each 16 KB, and recovery from errors with ZRPOS.
- ZMODEM crash recovery (`dmc_xfer_options.resume`, "ZMODEM with Crash
  Recovery" in the dialogs). The receiver continues a part of a file that
  is in the folder: it opens the file to add to it, and it sends ZRPOS with
  the length of the part. The receiver also does this when the sender sets
  ZF0 to ZCRECOV (`sz -r`). If the file is there whole, the receiver sends
  ZSKIP. When a transfer stops, the receiver keeps the part that it
  received. The sender with this option sets ZF0 to ZCRECOV.
- ZMODEM with zstd (the specification is `docs/zmodem-zstd.md` in the
  D-Modem repository). The D-Modem BBS, GUI and app use this mode. The
  receiver sets `CANZSTD` (0x40) in ZF1 of its ZRINIT. The sender sets
  `ZTZSTD` (8) in ZF2 of the ZFILE for each file that it compresses. Then
  the sender sends the file as independent zstd frames, one frame for each
  chunk of 64 KB. After a ZRPOS, the chunk is half as large (minimum 1 KB).
  After an ACK, the chunk is two times as large (maximum 64 KB). The
  receiver writes only complete frames. All positions are file offsets,
  thus ZRPOS and crash recovery operate as without zstd. The sender does
  not compress a file of 0 bytes, a file with a compressed extension (for
  example `.zip`, `.gz` or `.jpg`), or a file whose first 64 KB do not
  become 5% smaller. When the other side does not use this mode, the
  transfer is normal ZMODEM. To stop zstd, set `dmc_xfer_options.no_zstd`
  (the Windows app: Transfer, ZMODEM Compression (zstd)).
  `dmc_xfer_zstd` tells if the file now goes with zstd. The zstd encoder
  and decoder are in `core/src/zstd_codec.c`. They are a C port of the
  codec of the D-Modem app (`Zstd.kt`), and they do not use libzstd.
- YMODEM, XMODEM and XMODEM-1K.
- YMODEM-G: the receiver asks with G. The sender sends the blocks and does
  not wait for an ACK. Use it only on a line that corrects its errors (V.42).
  An error stops the transfer, because a block cannot come again. The
  sender does plain YMODEM when the receiver asks with C (lrzsz's `rb`).
  lrzsz's `sb` sends no empty block 0 at the end in YMODEM-G, thus the
  receiver ends the batch when no block 0 comes after a file.
- Kermit: long packets, block checks 1 to 3, attribute packets,
  repeat counts and 8-bit prefixes.
- Intel HEX and Motorola S-records: the app sends a file of records as it
  is, and it encodes other files as records. When it receives, it decodes
  the records to a binary image.
- Plain text: the app sends the bytes of the file as they are. With
  `line_delay_ms` and `char_delay_ms`, the app waits after each line end
  and after each character (as the ASCII Setup of HyperTerminal). When it
  receives, it keeps the text until the line is quiet. A cancel keeps the
  text that came.

`dmc_xfer_skip` skips the file that the app receives now. ZMODEM sends
ZSKIP. Kermit puts X in the ACK of a data packet, and the sender ends the
file with Z and D. The progress dialog has a Skip file button for these
two protocols.

Each transfer runs on its thread (CreateThread on Windows, pthreads on
other systems). The thread and the app use two queues with a lock:

1. The app gives the bytes from the line to `dmc_xfer_feed`.
2. The app gets the bytes for the line from `dmc_xfer_output`. The app
   gets only as much as the line can send now. The queue holds 32 KB
   maximum, thus the thread waits when the line is slow.
3. The notify function of `dmc_xfer_set_notify` tells the app when there
   are bytes to send. On Windows, it posts `WM_APP_XFER` to the window.
4. `dmc_xfer_get_status` gives the file, the bytes, the size, the retries
   and the result.

The protocol code reads files only through `xfer_src_read`, and writes
files only through `xfer_dst_write`. The positions in the protocols are
file offsets. When a transfer fails or you cancel it, the app deletes the
part of the file that it received (but not for plain text).
`dmc_xfer_cancel` sends the cancel sequence of the protocol: 10 CAN and
10 BS for ZMODEM, an E packet for Kermit.

`dmc_xfer_detect` finds the start of a transfer in the received data: the
ZRQINIT of sz, the ZRINIT of rz, and the Send-Init of kermit -s. When
Automatic Receive is on, the app starts a receive, or it opens the Send
File dialog for rz.

The Windows app (`windows/src/xfer.cpp`) has these parts:

- The Transfer menu: Send File, Receive File, Automatic Receive and
  Cancel Transfer. The Terminal page also has Send and Receive buttons.
- The Send File and Receive File dialogs, and a progress dialog as in
  HyperTerminal. The progress dialog shows the file, the protocol, the
  retries, the throughput, the time and the bytes.
- The transfer uses the line of the page that you see: the modem on the
  Terminal page, or the login on the Remote page (`struct XferLine`).
- `dmodem.ini` keeps the folder, the protocols and Automatic Receive in
  the `[transfer]` section. The default folder is `Received` in the data
  folder of the app.

`core/test/xfer_test.c` tests each protocol on a clean line and on a line
with noise. It also does a test with `sz` and `rz` (lrzsz) and with
C-Kermit on a pty, when they are installed. Set `DMODEM_XFER_INTEROP=1` to
make these tests mandatory.

The Linux and macOS apps do not have a transfer UI at this time.

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

The Linux, macOS and Windows apps do not start dmodemd by default. They run the
modem in a thread (`libdmodem_embed`), and its DTE is a socket. Refer to
[The built-in modem](embedded-modem.md).

## Hardware modems: the MultiTech MT9234MU

The "device" line can also open a hardware modem. The apps send AT commands
to it and read its result codes, as they do with dmodemd. The Settings page
needs `AT!M`, thus it operates only with dmodemd.

The MultiTech MultiMobile MT9234MU (USB 06E0:F114) and the MT9234ZBA-USB
(06E0:F115, 06E0:0319) are V.92 data and fax modems. They have a TI TUSB3410
USB UART. The TUSB3410 has no firmware of its own. The host downloads the
firmware into the chip each time you connect the modem.

| | Linux | macOS | Windows |
| --- | --- | --- | --- |
| Driver | `ti_usb_3410_5052` (in the kernel) | we do not know a driver for the MT9234MU. The CDC-ACM models (MT9234MU-CDC) use the driver of the system | the driver of MultiTech |
| Firmware | `mts_mt9234mu.fw` in `/lib/firmware` | not applicable | the driver of MultiTech has it |
| The port | `/dev/ttyUSB0` | `/dev/cu.usbmodem*` (CDC-ACM models) | a COM port |

Linux does not include `mts_mt9234mu.fw` after June 2026. MultiTech gave no
license that lets other persons distribute it, thus linux-firmware removed
it. This repository includes the file, with a notice
(`linux/firmware/README.md`). The AGPL-3.0 license does not apply to it.
`cmake --install` puts it in `share/dmodem-gui/firmware`, and
`-DDMODEM_GUI_INSTALL_FIRMWARE=ON` also puts it in `/lib/firmware`. Else,
copy it to `/lib/firmware` (for the MT9234ZBA-USB, as `mts_mt9234zba.fw`),
then disconnect the modem and connect it again.

On Linux, the app finds the modem in sysfs (`linux/src/usbmodem.c`). The
line settings show where the modem is, with a "Use" button. If the modem
has no firmware, they show the command that copies the file. When the app opens the port of the
modem, it sets RTS/CTS flow control (`CRTSCTS`). The modem uses `&K3` from
the factory.

Set the country of the line with `AT%T19,0,<code>` and `AT&F&W` in the
"Commands when the modem opens" field one time, or in the Terminal page.
`AT%T19,0,34` is the default (North America and Europe).

`ATI11` on these modems (and on other Agere modems) shows its report on two
pages. After each page, the modem waits for a key ("Press any key"). The
library sends a space when a command of the app gets this prompt, thus the
Call Stats page gets the full report. It does this for 8 pages at most. In
the Terminal page, you push the key.

We did not test this yet with a real MT9234MU.

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
Windows and macOS. A build with no libssh (`-DDMODEM_GUI_SSH=OFF`) runs the
`ssh.exe` of Windows (Windows 10 1809 and later) in a pseudo console, if it
finds it.

The Windows 95 build also has libssh. libssh and mbedTLS use some functions
that Windows 95 does not have. Thus the build changes them as follows
(`ssh/cmake/PatchMbedtls.cmake` and `PatchLibssh.cmake`, only for targets
before Windows Vista):

| Function | Windows that have it | In the Windows 95 build |
| --- | --- | --- |
| `BCryptGenRandom` (mbedTLS, the random numbers) | Vista and later | `CryptGenRandom` of CryptoAPI, found at run time. CryptoAPI is in Windows 95 OSR2, 98, NT 4.0 and later, and Internet Explorer 3 and later installs it. If CryptoAPI is not there, SSH cannot connect. The other pages operate |
| `SHGetSpecialFolderPathA` (libssh, the home folder) | 98, 2000, and 95 and NT 4.0 with the shell of Internet Explorer 4 | found at run time, else `USERPROFILE` |
| `if_nametoindex` (libssh, IPv6 link-local addresses) | Vista and later | found at run time, else no interface names |
| `WSAStringToAddressA` (libssh, the address check) | 98, and 95 with Winsock 2 | found at run time, else IPv4 with `inet_addr` of Winsock 1.1 |
| `gai_strerrorA` of mingw-w64 (it calls `FormatMessageW`) | NT | `ssh/src/gai_strerror.c`, with `FormatMessageA` |

Thus the app imports only DLLs that each Windows 95 has, as before. libssh
uses Winsock 1.1 (`wsock32`) for its sockets. `getaddrinfo` is the
`WspiapiGetAddrInfo` of mingw-w64, which uses `gethostbyname` if Winsock 2
is not there.

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
- clang-retro builds the app (`cmake/clang-retro.cmake`, a CMake toolchain
  file), as CI does. Its toolchain package comes from the
  `clang-retro/dos-sdk/.github/actions/setup-toolchain` action, or from
  `packaging/get-toolchain.sh` in that repository. `CLANG_DOS` is the
  directory of the package. `CLANG_RETRO_CONFIG` is the `--config` file:
  - `win32-picolibc` (the default): Windows 95, 98, ME, NT 4.0 and later.
    picolibc is the C library, and it is linked into the app. Thus the app
    needs no `msvcrt.dll`, which some installations of Windows 95 do not
    have. This build has libssh too (see "Remote logins", above).
  - `win7`: Windows 7 and later. This is the minimum Windows version of
    dmodemd too.
- CI examines the imports of the Windows 95 build with `check-imports.py`
  of clang-retro, for Windows 9x and for NT 4.0. The build fails if the app
  uses a function that these versions do not have. CI also runs the tests
  of the two builds in Wine.
- picolibc has no `_snprintf`, `_stricmp` or `_strnicmp`. `app.h` gives
  these names to `snprintf`, `strcasecmp` and `strncasecmp`.
- MinGW and MSVC also build the same code with CMake.

dmodemd itself needs Windows 7 or later. On Windows 9x and NT 4, the app
uses a COM port by default. It can also connect to a dmodemd on another
machine through TCP.

## The Windows app on Linux and macOS

The Windows app also builds for Linux and macOS. clang-retro MFC has a
portable Win32 layer (`port/`, `libwinport.a`). The layer supplies the
Win32 API on X11, Wayland and Cocoa: windows and controls, Winsock,
files, COM ports, processes and pipes, and sound (`waveOut`, `waveIn`
and ACM). The app links with this layer and not with Windows. Its source
code is the same as on Windows.

- Use `-DDMODEM_GUI_WINPORT=ON` and set `CLANG_RETRO_MFC` to a checkout of
  clang-retro MFC. Use clang. `winport/CMakeLists.txt` builds
  `libwinport.a` with the Makefile of MFC. It converts `app.rc` to C++
  with `port/tools/res2cpp.py` (this needs `llvm-rc`). The app is
  `winport/dmodem-win32` in the build directory. `windows/sources.cmake`
  is the list of sources for the two builds.
- `DMODEM_WINPORT` is defined for this build. The app uses it in these
  locations only:
  - The settings file is in the `CSIDL_APPDATA` folder of the layer
    (`~/.config/D-Modem` on Linux, `~/Library/Application Support/D-Modem`
    on macOS). On Windows, the app uses `APPDATA`, because Windows 95 has
    no `SHGetFolderPath`.
  - The line is dmodemd by default. The layer tells the app that Windows
    is version 5, and on Windows the app then uses a COM port by default.
  - dmodemd is `dmodemd` next to the app, or else on the `PATH`.
  - SSH uses `~/.ssh` and `/` in paths, because libssh uses POSIX paths
    on these systems.
- The layer changes a COM port name to a device: `COM1` is `/dev/ttyS0`.
- CI builds the app on Linux and macOS. On Linux, `winport/test/smoke.sh`
  starts the app in Xvfb two times: with a TCP line, and with a dmodemd
  that the app starts (`winport/test/dmodemd`, a test dmodemd). Each time,
  the test makes sure that the app stays up and sends AT commands to a
  test modem.
