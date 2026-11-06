#!/bin/sh
# Card integrity + content audit. HOST-SIDE ONLY: no flash, no provisioning, the board is not
# touched — safe to run any time the bridge has released the volume.
#
#   sh card_integrity_audit.sh [min-bytes-per-chunk]
#
# Three car-safety questions about a card that has been interrupted mid-write many times
# (ignition-off is a surprise removal in the middle of a FAT write, and this bench has done that
# on purpose more times than a car will):
#
#   1. FAT INTEGRITY - does the volume still check out read-only? (`fsck.fat -n`: reports, writes
#      nothing. Never run a repairing fsck on this volume: the bridge owns its FAT state.)
#   2. NO PHANTOMS / NO TRUNCATIONS - every track the card offers must be real, complete audio.
#      Content that matches the share's CURRENT window is proven identical by md5. Content that has
#      rotated out of that 5-hour window cannot be re-proved from the share, so it is checked by
#      size: the recorder's chunks are all ~43.2 MB (1800 s at 192 kbps), so a file materially
#      smaller than one chunk is a truncated transfer the head unit would play as noise.
#   3. NO SCRATCH LEFT - a .part on the card means either an interrupted file the sweep has not
#      expired yet, or a scratch leak.
#
# Bench rules inherited from the other harnesses: exclusive flock on /tmp/bridge.lock (never audit
# a disk another harness is driving), the card found by USB vendor 303a via udevadm (never by
# /dev/sdX name, never "the first vfat" — this bench has a second 1e3d stick), READ-ONLY mount,
# SMB creds from the 600 file and never in argv.
set -u
MIN_BYTES=${1:-40000000}
CRED=/root/.smbcreds
SRV=${SRV:-10.119.0.4}
SHARE=${SHARE:-CAR-MEDIA}
M=/mnt/card

exec 9>/tmp/bridge.lock
if ! flock -n 9; then
    echo "REFUSING TO RUN: another bench harness holds /tmp/bridge.lock." >&2
    exit 1
fi

DEV=""
for b in /sys/block/sd*; do
    [ -e "$b" ] || continue
    if udevadm info -q property -n "/dev/$(basename "$b")" 2>/dev/null | grep -q '^ID_VENDOR_ID=303a$'; then
        [ -n "$DEV" ] && { echo "ABORT: more than one Espressif MSC volume"; exit 1; }
        DEV="/dev/$(basename "$b")"
    fi
done
[ -n "$DEV" ] || { echo "ABORT: no Espressif (303a) mass-storage volume - the bridge holds the disk (that is normal mid-pass)"; exit 1; }
findmnt -S "$DEV" >/dev/null 2>&1 && { echo "ABORT: $DEV is already mounted"; exit 1; }
echo "== card $DEV =="

echo "== 1. FAT check (read-only) =="
FS=$(fsck.fat -n -v "$DEV" 2>&1); RC=$?
printf '%s\n' "$FS" | sed 's/^/   /'
echo "   fsck.fat exit=$RC  (0 = clean)"
FAT_OK=1
[ "$RC" = 0 ] || FAT_OK=0

echo "== 2. tree =="
mkdir -p "$M"
mount -o ro "$DEV" "$M" || { echo "ABORT: cannot mount $DEV read-only"; exit 1; }
TREE=$M; cd "$TREE"
TOTAL=$(find . -type f -name '*.mp3' | wc -l)
# Scratch under BRIDGE/ is the firmware's own write-probe (BRIDGE/zz-speedtest.part), which the
# sweep deliberately never touches - counting it as a leftover would fail a healthy card.
PARTS=$(find . -type f -name '*.part' ! -path './BRIDGE/*' | wc -l)
OTHERS=$(find . -type f ! -name '*.mp3' ! -name '*.part' ! -path './BRIDGE/*' | wc -l)
echo "   mp3: $TOTAL   .part(non-BRIDGE): $PARTS   other(non-BRIDGE): $OTHERS"
find . -mindepth 1 -maxdepth 1 -type d | sed 's/^/   dir /'
# One name only: FAT is case-insensitive, so asking for both cases prints the same file twice and
# reads like a duplicate-entry bug in the firmware.
for s in BRIDGE/STATUS.TXT; do
    [ -f "$s" ] && { echo "   --- $s ---"; sed 's/^/   /' "$s"; }
done

W=$(mktemp -d); trap 'cd /; umount "$M" 2>/dev/null; rm -rf "$W"' EXIT INT TERM
echo "== 3. share window (download + md5) =="
smbclient "//$SRV/$SHARE" -A "$CRED" -c 'ls' 2>/dev/null | tr -s ' ' > "$W/ls"
awk '{ for(i=2;i<=NF;i++) if ($i ~ /^[ADRHS]+$/) { sz=$(i+1)+0; if (sz && $1 !~ /^\.\.?$/) print $1 } }' < "$W/ls" \
    | grep -i '\.mp3$' | sort -u > "$W/names"
echo "   share mp3 entries: $(wc -l < "$W/names")"
: > "$W/share.md5"
while read -r n; do
    rm -f "$W/dl"
    smbclient "//$SRV/$SHARE" -A "$CRED" -c "get \"$n\" \"$W/dl\"" >/dev/null 2>&1
    [ -f "$W/dl" ] && md5sum "$W/dl" | awk -v n="$n" '{print $1, n}' >> "$W/share.md5"
done < "$W/names"
echo "   hashed: $(wc -l < "$W/share.md5")"

echo "== 4. every card track vs the share =="
# Expect this stage to be SLOW and to look stuck: every byte is read THROUGH the bridge (1-bit
# SDMMC behind TinyUSB MSC measures 0.38 MB/s on this bench), so ~500 MB of library is tens of
# minutes. Lines stream as they complete - that stream is also the live read-progress bar.
find . -type f -name '*.mp3' | sed 's|^\./||' | sort | while IFS= read -r f; do
    cs=$(stat -c%s "$f"); cm=$(md5sum "$f" | cut -d' ' -f1)
    hit=$(awk -v m="$cm" '$1==m{print $2; exit}' "$W/share.md5")
    if [ "$cs" -lt "$MIN_BYTES" ]; then
        echo "TRUNCATED $f  $cs B (< $MIN_BYTES)"
    elif [ -n "$hit" ]; then
        echo "identical $f  $cs B  == share $hit"
    else
        echo "archived  $f  $cs B  (not in the share's current window)"
    fi
done | tee "$W/res" | sed 's/^/   /'
OK=$(grep -c '^identical' "$W/res" || true)
AGED=$(grep -c '^archived' "$W/res" || true)
BAD=$(grep -c '^TRUNCATED' "$W/res" || true)

cd /; umount "$M" 2>/dev/null; trap - EXIT INT TERM
echo "--- identical=$OK archived=$AGED truncated=$BAD .part=$PARTS fsck_ok=$FAT_OK ---"
if [ "$FAT_OK" = 1 ] && [ "$BAD" = 0 ] && [ "$PARTS" = 0 ] && [ "$OK" != 0 ]; then
    echo "== VERDICT: PASS =="; exit 0
fi
echo "== VERDICT: FAIL =="; exit 1
