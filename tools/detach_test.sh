#!/bin/sh
# Run the real end-to-end detach test on HermesBoard (the SBC wired to the CAM board's
# console + OTG port): boot the app, capture the console PASSIVELY, make the host let go of
# the USB disk, and print what the bridge did.
#
# Two rules this encodes, both learned the hard way:
#   * capture with `cat` on the tty after `stty raw -echo`. pyserial is forbidden: its
#     open() pulses DTR/RTS, which resets the board and destroys the DETACHED window.
#   * an app-only flash leaves the board in the bootloader, so flash-id --after hard-reset
#     is what actually starts the app before we start listening.
#
# Usage: detach_test.sh [quiet-seconds] [capture-seconds]   (defaults 150 20)
set -eu
QUIET=${1:-150}
TAIL=${2:-20}
PORT=$(ls /dev/serial/by-id/*1a86* | head -1)
[ -n "$PORT" ] || { echo "no 1a86 console port"; exit 1; }

esptool --chip esp32s3 -p "$PORT" --before default-reset --after hard-reset flash-id >/dev/null 2>&1
sleep 6
stty -F "$PORT" 115200 raw -echo
rm -f /tmp/sync.log
nohup timeout $((QUIET + TAIL + 40)) cat "$PORT" > /tmp/sync.log 2>/dev/null &
CATPID=$!
sleep 5

D=""
for d in /sys/bus/usb/devices/*; do
    [ -e "$d/idVendor" ] || continue
    [ "$(cat "$d/idVendor" 2>/dev/null)" = "303a" ] && D=$d
done
[ -n "$D" ] || { echo "no ESP32 USB device on the host"; kill $CATPID 2>/dev/null; exit 1; }

echo "[$(date +%T)] deauthorizing $D - the car 'ignition off' equivalent"
echo 0 > "$D/authorized"
sleep "$QUIET"

echo "=== log during the sync window ==="
grep -a -E "config:|let go|owner=app|SMB|sync|opendir|copied|  \+ |FAILED|handback|owner=host|E \(|W \(" /tmp/sync.log | head -40
echo "=== last lines ==="
tail -8 /tmp/sync.log
kill $CATPID 2>/dev/null || true
echo 1 > "$D/authorized"
echo "[$(date +%T)] re-attached - the car would see the volume again"
