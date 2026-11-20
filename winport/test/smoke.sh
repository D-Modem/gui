#!/bin/sh
# The smoke test of the Linux build (winport/). It starts the app on an
# X display, with an empty home folder, two times:
#   1. With a TCP line (127.0.0.1:2323) to a test modem.
#   2. With a dmodemd that the app starts: the test dmodemd in this
#      folder, found on the PATH, through a pipe (CreatePipe,
#      CreateProcess with STARTF_USESTDHANDLES and PeekNamedPipe).
# Each time, the app must stay up and send its first AT commands.
#
#   DISPLAY=:99 winport/test/smoke.sh build/winport/dmodem-win32
set -eu
app=$1
here=$(cd "$(dirname "$0")" && pwd)
tmp=$(mktemp -d)
pid= modem=
trap 'kill $modem $pid 2>/dev/null || true; pkill -f "fake_modem.py .* $tmp/" 2>/dev/null || true; rm -rf "$tmp"' EXIT

# run NAME INI: starts the app with this settings file, and waits for AT
# commands in $tmp/NAME.log
run() {
	name=$1
	rm -rf "$tmp/home"
	mkdir -p "$tmp/home/.config/D-Modem"
	printf '%b' "$2" > "$tmp/home/.config/D-Modem/dmodem.ini"
	HOME="$tmp/home" XDG_CONFIG_HOME= PATH="$here:$PATH" \
		DMODEM_TEST_LOG="$tmp/$name.log" WINPORT_BACKEND=x11 "$app" &
	pid=$!
	i=0
	while [ $i -lt 20 ]; do
		sleep 1
		kill -0 $pid 2>/dev/null || { echo "FAIL ($name): the app stopped"; exit 1; }
		grep -q '^AT' "$tmp/$name.log" 2>/dev/null && break
		i=$((i + 1))
	done
	grep -q '^AT' "$tmp/$name.log" 2>/dev/null ||
		{ echo "FAIL ($name): no AT commands"; exit 1; }
	sleep 2
	kill -0 $pid 2>/dev/null || { echo "FAIL ($name): the app stopped"; exit 1; }
	echo "$name: the modem got:"
	cat "$tmp/$name.log"
	kill $pid
	wait $pid 2>/dev/null || true
	pid=
}

python3 "$here/fake_modem.py" 2323 "$tmp/tcp.log" &
modem=$!
sleep 1
run tcp '[line]\nline=1\ntcp=127.0.0.1:2323\n'
# (a USB modem: dmodemd needs no SIP account for it)
run dmodemd '[line]\nline=0\nusb=test0\n'
echo "PASS"
