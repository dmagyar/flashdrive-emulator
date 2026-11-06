#!/bin/sh
# One A/B arm: delete N files from the card, let a single pass re-fetch them, time that pass.
#
#   ba_win_ab.sh LABEL file1 [file2 ...]
#
# Why wipe instead of a fresh library: the pass has to transfer real bytes, and "missing on the
# card, present on the share" is the branch that triggers a fetch - no share-side privileges
# needed. Files are deleted from the host side while the host owns the volume, which is a normal
# host write, not a double ownership.
#
# Reported rate = bytes fetched / (sync done - sync start) from the firmware's own timestamps, so
# it includes the SMB connect (~3 s) and the walk. Consistent across arms, which is what an A/B
# needs. Never compare across arms with different file counts.
set -u
CRED=/root/.smbcreds
M=/mnt/card
DEV=${DEV:-/dev/sdb}
LOGDIR=/tmp/ba
LABEL=${1:?usage: ba_win_ab.sh LABEL file1 [file2 ...]}
shift
[ $# -ge 1 ] || { echo "no files listed"; exit 1; }

D=""
for d in /sys/bus/usb/devices/*; do
    [ -e "$d/idVendor" ] || continue
    [ "$(cat "$d/idVendor" 2>/dev/null)" = "303a" ] && D=$d
done
[ -n "$D" ] || { echo "FAIL: no ESP32 USB device"; exit 1; }
DAUTH="$D/authorized"

# bytes we intend to re-fetch, straight from the share (authoritative size)
NEED=0
for f in "$@"; do
    sz=$(smbclient //10.119.0.4/CAR-MEDIA -A "$CRED" -c "ls \"$f\"" 2>/dev/null \
         | awk '{ for (i = 2; i <= NF; i++) if ($i ~ /^[0-9]+$/ && $(i+1) ~ /^[A-Z][a-z][a-z]$/) { print $i; exit } }')
    [ -n "$sz" ] || { echo "FAIL: $f not on the share"; exit 1; }
    NEED=$((NEED + sz))
done
echo "arm=$LABEL  re-fetching $# file(s), $NEED B"

mkdir -p "$M"; umount "$M" 2>/dev/null || true
mount -o rw "$M" 2>/dev/null || mount -o rw "$DEV" "$M" || { echo "FAIL: cannot mount $DEV rw"; exit 1; }
for f in "$@"; do
    [ -e "$M/$f" ] && { rm -f "$M/$f"; echo "  wiped $f"; } || echo "  NOTE: $f was not on the card"
done
sync; umount "$M" || { echo "FAIL: cannot unmount"; exit 1; }

PORT=$(ls /dev/serial/by-id/*1a86* 2>/dev/null | head -1)
[ -n "$PORT" ] || { echo "FAIL: no console"; exit 1; }
stty -F "$PORT" 115200 raw -echo
mkdir -p "$LOGDIR"                  # before the redirect below, or the first run after a reboot dies
LOG=$LOGDIR/$LABEL-$(date +%s).log
nohup timeout 1500 cat "$PORT" > "$LOG" 2>/dev/null &
CATPID=$!
sleep 1
echo 0 > "$DAUTH"

i=0
GO=""
while [ $i -lt 1200 ]; do
    grep -aq "sync done:\|sync FAILED:" "$LOG" && break
    sleep 5; i=$((i + 5))
done
grep -aq "sync done:\|sync FAILED:" "$LOG" || { kill $CATPID 2>/dev/null; echo 1 > "$DAUTH"; echo "FAIL: no pass in ${i}s"; exit 1; }
sleep 3
kill $CATPID 2>/dev/null || true
echo 1 > "$DAUTH"

S=$(grep -a "sync start:" "$LOG" | tail -1 | sed -n 's/^I (\([0-9]*\)).*/\1/p')
E=$(grep -a "sync done:"  "$LOG" | tail -1 | sed -n 's/^I (\([0-9]*\)).*/\1/p')
RE=$(grep -a "  + " "$LOG" | grep -ac "")
GOT=$(grep -a "sync done:" "$LOG" | tail -1 | sed -n 's/.*(\([0-9]*\) bytes).*/\1/p')
case "$S:$E:$GOT" in
  *:*:|"::"*|*:*:"") echo "FAIL: could not read both stamps and the byte count from $LOG (S=$S E=$E GOT=$GOT)"; exit 1;;
esac
DUR=$(( (E - S) / 1000 ))
[ "$DUR" -gt 0 ] || { echo "FAIL: pass measured $((E - S)) ms - too short to time"; exit 1; }
REC=$(grep -ac "reconnects" "$LOG" || true)
[ "$GOT" = "$NEED" ] || echo "  NOTE: reported $GOT B, expected $NEED B (a resumed .part would do that)"
KB=$((GOT / DUR / 1000))
echo "RESULT arm=$LABEL files=$RE bytes=$GOT duration=${DUR}s rate=${KB}KB/s stallwarnings=$REC"
grep -a "  + \|continuing \|sync done:" "$LOG" | sed 's/^/    /'
