#!/bin/sh
# Builds dmodem.exe with Open Watcom 2.0, for Windows 95 and later:
# ANSI Win32, the Windows 95 common controls, Winsock 1.1, the static
# Watcom C library.  WATCOM must name Open Watcom's directory, and its
# binaries be on the PATH (on Linux, $WATCOM/binl64 or binl).
#
#     windows/build-watcom.sh [the output directory, build/watcom]
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
out=${1:-$root/build/watcom}
mkdir -p "$out"
cd "$out"

: "${WATCOM:?set WATCOM to Open Watcom's directory}"
inc="-i=$WATCOM/h -i=$WATCOM/h/nt -i=$root/core/include -i=$root/core/src"
# 386 code, the stack calling convention's names as Win32's, C99
copts="-q -bt=nt -bm -3r -ox -w4 -zq -za99 $inc"
cppopts="-q -bt=nt -bm -3r -ox -w4 -zq -xs $inc -i=$here/src"

objs=""
for f in "$root"/core/src/*.c; do
	o=$(basename "$f" .c).obj
	wcc386 $copts -fo="$o" "$f"
	objs="$objs $o"
done
for f in "$here"/src/*.cpp; do
	o=$(basename "$f" .cpp).obj
	wpp386 $cppopts -fo="$o" "$f"
	objs="$objs $o"
done
wrc -q -r -bt=nt -i="$WATCOM/h/nt" -i="$here/src" -i="$here/res" \
	-fo=app.res "$here/res/app.rc"

{
	echo "system nt_win"
	echo "name dmodem.exe"
	echo "option quiet"
	echo "option stack=256k"
	# Windows 95 and NT 4 take a subsystem version of 4.0
	echo "runtime windows=4.0"
	for o in $objs; do echo "file $o"; done
	echo "library comctl32.lib, wsock32.lib, comdlg32.lib"
	echo "option resource=app.res"
} > dmodem.lnk
wlink @dmodem.lnk
ls -l dmodem.exe
