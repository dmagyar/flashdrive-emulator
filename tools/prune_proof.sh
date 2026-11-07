#!/bin/sh
# Prove deletion propagation on the real card, WITHOUT needing write access to the share.
#
#   sh tools/prune_proof.sh [n_orphans]        # default 3
#
# The share account is read-only (as it should be), so "delete a track on the NAS" is not something
# the bench can do. But the code path does not care how a file came to be on the card: it asks the
# share whether each managed card file still exists. So plant files on the card that the share has
# never heard of - the mirror of the "rename the synced file" trick - and the pass must remove
# exactly those.
#
# The assertions that matter are the negative ones. Deleting the planted files is easy to get
# right; deleting a real track is the failure mode this whole feature could ship with. So the test
# also plants three decoys that must SURVIVE (wrong extension, in-progress .part, inside BRIDGE/),
# and compares the size of every pre-existing card file before and after: any real track that
# changed size or vanished is a FAIL, whatever the log says.
set -u
M=${M:-/mnt/card}
DEV=${DEV:-/dev/sdb}
LOG=${LOG:-/tmp/prune_proof.log}
MAXWAIT=${MAXWAIT:-900}
N=${1:-3}
TAB=$(printf '\t')
say() { echo "$(date +%T) $*"; }
bail() { echo "FAIL: $*"; exit 1; }
sum() { awk -F"$TAB" '{ s += $1 } END { print s + 0 }' "$1"; }

D=""
for d in /sys/bus/usb/devices/*; do
    [ -e "$d/idVendor" ] || continue
    [ "$(cat "$d/idVendor" 2>/dev/null)" = "303a" ] && D=$d
done
[ -n "$D" ] || bail "no ESP32 USB device on the host"
DAUTH="$D/authorized"
# A bail must not leave the bench hostage: this script de-authorizes the device, and a failure
# before the hand-back step leaves the bridge owning the volume with no /dev/sdb on the host.
trap 'echo 1 > "$DAUTH" 2>/dev/null' EXIT INT TERM
PORT=$(ls /dev/serial/by-id/*1a86* 2>/dev/null | head -1)
[ -n "$PORT" ] || bail "no console port"

card_list() { ( cd "$M" && find . -type f ! -path './BRIDGE/*' -printf '%s\t%p\n' ) | sed 's|\t\./|\t|' | sort -t"$TAB" -k2; }

echo "=== 1. card before, then planted orphans and decoys ==="
mkdir -p "$M"; umount "$M" 2>/dev/null || true
mount -o rw "$DEV" "$M" || bail "cannot mount $DEV rw - the bridge owns the volume right now"
card_list > /tmp/prune-before.tsv
[ -s /tmp/prune-before.tsv ] || bail "card is empty - run growth_sync_test.sh first, this test needs a synced library"
BEFORE_LINES=/tmp/prune-real.tsv
grep -v 'PRUNE-TEST\|PRUNE-KEEP' /tmp/prune-before.tsv > "$BEFORE_LINES"
say "card: $(wc -l < "$BEFORE_LINES") real file(s), $(( $(sum "$BEFORE_LINES") / 1048576 )) MiB"

i=1
while [ $i -le "$N" ]; do
    head -c 1048576 /dev/urandom > "$M/PRUNE-TEST-0$i.mp3"      # matches *.mp3, absent from the share
    i=$((i + 1))
done
head -c 4096 /dev/urandom > "$M/PRUNE-KEEP.jpg"                  # outside the filter
head -c 4096 /dev/urandom > "$M/PRUNE-KEEP.mp3.part"             # a resume point, never a target
mkdir -p "$M/BRIDGE" && head -c 4096 /dev/urandom > "$M/BRIDGE/PRUNE-INSIDE.mp3"
say "planted $N orphan .mp3 + 3 decoys that must survive"
sync; umount "$M" || bail "cannot unmount - something is holding $M"

echo "=== 2. one pass, host out of the way ==="
stty -F "$PORT" 115200 raw -echo
: > "$LOG"
nohup timeout $((MAXWAIT + 120)) cat "$PORT" > "$LOG" 2>/dev/null &
CATPID=$!
sleep 1
echo 0 > "$DAUTH"
i=0; ENDED=""
while [ $i -lt "$MAXWAIT" ]; do
    grep -aq "sync done:\|sync FAILED:" "$LOG" && { ENDED=marker; break; }
    sleep 5; i=$((i + 5))
    [ $((i % 60)) -eq 0 ] && say "  ... ${i}s  $(grep -a 'prune\|  - ' "$LOG" | tail -1 | cut -c1-70)"
done
[ -n "$ENDED" ] || { kill $CATPID 2>/dev/null; bail "no completed pass within ${MAXWAIT}s"; }
sleep 3; kill $CATPID 2>/dev/null || true
grep -a "sync start:\|  - \|prune\|sync done:\|sync FAILED:" "$LOG" | sed 's/^/  /'

echo "=== 3. what the firmware says it did ==="
# Strip CR first - every console line ends in CR, and a `.*` before a `\([0-9]*\)` capture happily
# matches up to " pruned" and captures nothing, so an empty string looks like a missing column.
LOGC=/tmp/prune_proof.clean
tr -d '\r' < "$LOG" > "$LOGC"
PRUNED=$(sed -n 's/.*prune: \([0-9][0-9]*\) stale.*/\1/p' "$LOGC" | tail -1)
SUMMARY=$(sed -n 's/.*, \([0-9][0-9]*\) pruned$/\1/p' "$LOGC" | tail -1)
LINES=$(grep -c "  - /PRUNE-TEST" "$LOGC" || true)
say "prune line says ${PRUNED:-none}; summary column says ${SUMMARY:-none}; per-file removals: $LINES"
[ "${PRUNED:-0}" = "$N" ] || bail "expected $N pruned, firmware reported ${PRUNED:-nothing}"
[ "${SUMMARY:-}" = "$N" ] || bail "the summary's pruned column (${SUMMARY:-empty}) disagrees with the prune line ($PRUNED) - reporting is lying"
[ "$LINES" = "$N" ] || bail "$LINES per-file removal lines, expected $N - the count and the log disagree"
grep -q "  - /PRUNE-KEEP" "$LOGC" && bail "the log shows a decoy being removed"

echo "=== 4. what the card actually looks like ==="
echo 1 > "$DAUTH"
i=0; while [ ! -b "$DEV" ] && [ $i -lt 40 ]; do sleep 1; i=$((i + 1)); done
mount -o ro "$DEV" "$M" || bail "cannot mount ro to verify"
GONE=0; STUCK=0
i=1
while [ $i -le "$N" ]; do
    if [ -e "$M/PRUNE-TEST-0$i.mp3" ]; then STUCK=$((STUCK + 1)); echo "  still there: PRUNE-TEST-0$i.mp3"; else GONE=$((GONE + 1)); fi
    i=$((i + 1))
done
[ $GONE -eq "$N" ] || bail "only $GONE/$N planted orphans were removed"
say "all $N planted orphans are gone from the card"
for keep in PRUNE-KEEP.jpg PRUNE-KEEP.mp3.part; do
    [ -e "$M/$keep" ] || bail "COLLATERAL: $keep was deleted - the guard for it does not work"
done
[ -e "$M/BRIDGE/PRUNE-INSIDE.mp3" ] || bail "COLLATERAL: the pruner reached into BRIDGE/"
say "all three decoys survived (.jpg, .part, BRIDGE/)"

card_list | grep -v 'PRUNE-TEST\|PRUNE-KEEP' > /tmp/prune-after.tsv
if diff -q "$BEFORE_LINES" /tmp/prune-after.tsv >/dev/null; then
    say "every real track is byte-for-byte identical in size to before - no collateral damage"
else
    echo "  before:"; diff "$BEFORE_LINES" /tmp/prune-after.tsv | sed 's/^/    /'
    bail "a real track changed or vanished - this is the failure this test exists to catch"
fi
umount "$M" 2>/dev/null || true

# clean up the decoys so the next test starts from a clean card
mount -o rw "$DEV" "$M" 2>/dev/null || true
if [ -d "$M" ]; then rm -f "$M/PRUNE-KEEP.jpg" "$M/PRUNE-KEEP.mp3.part"; sync; umount "$M" 2>/dev/null || true; fi

echo "PRUNE_PROOF_OK $N orphan file(s) deleted, 3 decoys kept, $(wc -l < /tmp/prune-after.tsv) real tracks untouched"
