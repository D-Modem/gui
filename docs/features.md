# Features

This page gives the details of the pages and menus of the apps. The
[README](../README.md) gives a summary.

- **Remote**: log in to a server by SSH, Telnet or rlogin, in a separate terminal. SSH is built in, on libssh: it uses the keys, agent, config and known hosts of OpenSSH. It can use or import the `.ppk` keys of PuTTY. It uses Pageant on Windows, and an ssh-agent on Linux and macOS. On Windows 95, SSH needs CryptoAPI (Windows 95 OSR2, or Internet Explorer 3 or later)
- **Terminal types**: the two terminals use the emulator of the D-Modem app, ported to C:
  xterm, the VT52 to VT525 of DEC, ANSI-BBS (with Avatar, RIPscrip and Amiga ANSI),
  the PETSCII of the Commodores, the ATASCII of the Atari 8-bit and the ST, Viewdata,
  the Minitel and the Tektronix 4014. They have sixel, ReGIS and Tektronix
  graphics, and the font of each machine (the SyncTERM copies of their ROMs,
  LGPL-2.0-or-later: `core/licenses/syncterm-fonts-LGPL-2.0.txt`)
- **File transfers** (Windows app): send and receive files on the modem line or on a Telnet, rlogin or SSH login. The protocols are the same as the D-Modem app: ZMODEM (with Automatic Receive when the remote side starts sz, rz or kermit -s, with Crash Recovery, and with zstd compression to the D-Modem BBS and app), YMODEM, YMODEM-G, XMODEM, XMODEM-1K, Kermit, Intel HEX, Motorola S-records and plain text. Use the Transfer menu, or the Send and Receive buttons of the Terminal page
- **Copy and capture** (Windows app): select text with the mouse (Shift and the mouse when the remote side uses the mouse), then copy it. Paste to Host, Select All, Capture Text (to a file), Capture to Printer, and Print (the screen or the selection). Text that the remote side sends to its printer port goes to the printer. Use the Edit and Transfer menus, or click the right mouse button on the terminal
- **ASCII Setup** (Windows app, Modem menu): the same items as HyperTerminal. For the data that you send: Enter sends CR LF, the local echo, and the line delay and character delay of a text file. For the data that you receive: an LF after each CR, 7-bit ASCII, and the wrap of long lines. The settings apply to the modem terminal and to the Remote terminal
- **Terminal Settings** (Windows app, Modem menu): the same items as HyperTerminal, for the modem terminal and for the Remote terminal. They are what Backspace sends (Ctrl+H, Del, or Ctrl+H, Space, Ctrl+H), the function, arrow and Ctrl keys as terminal keys or Windows keys, the backscroll buffer lines, the font, the cursor, the Telnet terminal ID, and Exit program upon disconnecting. The status bar shows the connect timer during a call
- **Call menu** (Windows app): Call (sessions with a name and a number, as the connection files of HyperTerminal), Wait for a Call (ATS0=1), Disconnect, and Send Break (Ctrl+Break: a COM port or a Telnet connection). Line Settings has the COM port settings: bits per second, data bits, parity, stop bits and flow control
