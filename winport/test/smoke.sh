#!/bin/sh
# The smoke test of the Linux build (winport/): starts the app on an X
# display with an empty home folder, and makes sure that it stays up and
# sends its first AT commands to a test modem on the TCP port that it
# uses by default (127.0.0.1:2323).
#
#   DISPLAY=:99 winport/test/smoke.sh build/winport/dmodem-win32
set -eu
app=$1
here=$(cd "$(dirname "$0")" && pwd)
tmp=$(mktemp -d)
trap 'kill $modem $pid 2>/dev/null || true; rm -rf "$tmp"' EXIT

python3 "$here/fake_modem.py" 2323 "$tmp/modem.log" &
modem=$!
sleep 1
HOME="$tmp/home" WINPORT_BACKEND=x11 "$app" &
pid=$!

i=0
while [ $i -lt 20 ]; do
	sleep 1
	kill -0 $pid 2>/dev/null || { echo "FAIL: the app stopped"; exit 1; }
	grep -q '^AT' "$tmp/modem.log" 2>/dev/null && break
	i=$((i + 1))
done
grep -q '^AT' "$tmp/modem.log" || { echo "FAIL: no AT commands on the TCP port"; exit 1; }
sleep 2
kill -0 $pid 2>/dev/null || { echo "FAIL: the app stopped"; exit 1; }
echo "The modem got:"
cat "$tmp/modem.log"
echo "PASS"
