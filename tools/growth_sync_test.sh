#!/bin/sh
# The library on the share grew. Does the next pass pick up exactly what is new, RESUME what a
# previous pass left half-written, and skip everything it already has?
#
# This is the everyday scenario (someone drops tracks onto the NAS), tested against the real
# share instead of a synthetic probe. Per-file size agreement is checked for the WHOLE library
# and a few files are md5'd against the NAS.
#
# Console capture uses stty+cat; pyserial's open() pulses DTR/RTS and resets the board, which
# destroys the window being measured. A pickup run is long (~400 KB/s), so background it.
set -u
CRED=/root/.smbcreds
SRV=${SRV:-10.119.0.4}
SHARE=${SHARE:-CAR-MEDIA}
M=/mnt/card
DEV=${DEV:-/dev/sdb}
LOG=${LOG:-/tmp/growth.log}
MAXWAIT=${MAXWAIT:-2400}
TAB=$(printf '\t')

say() { echo "$*"; }
bail() { echo "FAIL: $*"; [ -n "${DAUTH:-}" ] && echo 1 > "$DAUTH" 2>/dev/null; exit 1; }
sum() { cut -f1 "$1" | awk '{ s += $1 } END { print s + 0 }'; }

# smbclient's ls puts names with spaces before the attribute/size, so find the size by the
# weekday that always follows it.
nas_list() {
    smbclient "//$SRV/$SHARE" -A "$CRED" -c "ls" 2>/dev/null | awk '
        { for (i = 2; i <= NF; i++)
            if ($i ~ /^[0-9]+$/ && $(i+1) ~ /^[A-Z][a-z][a-z]$/) {
                if ($i + 0 > 0) { nm = $1; for (j = 2; j <= i - 2; j++) nm = nm " " $j
                                  printf "%s\t%s\n", $i, nm }
                break } }' | sort -t"$TAB" -k2
}
card_list() {
    ( cd "$M" && find . -type f ! -path './BRIDGE/*' ! -name '*.part' -printf '%s\t%p\n' ) \
        | sed 's|\t\./|\t|' | sort -t"$TAB" -k2
}

D=""
for d in /sys/bus/usb/devices/*; do
    [ -e "$d/idVendor" ] || continue
    [ "$(cat "$d/idVendor" 2>/dev/null)" = "303a" ] && D=$d
done
[ -n "$D" ] || bail "no ESP32 USB device on the host"
DAUTH="$D/authorized"

nas_list > /tmp/nas-before.tsv
[ -s /tmp/nas-before.tsv ] || bail "could not list the share"
say "share: $(wc -l < /tmp/nas-before.tsv) file(s), $(( $(sum /tmp/nas-before.tsv) / 1048576 )) MiB"

mkdir -p "$M"; umount "$M" 2>/dev/null || true
mount -o ro "$DEV" "$M" || bail "cannot mount $DEV - the bridge owns the volume right now"
card_list > /tmp/card-before.tsv
CARDN=$(wc -l < /tmp/card-before.tsv)
CARDB=$(sum /tmp/card-before.tsv)
PARTS=$(cd "$M" && ls *.part 2>/dev/null | tr '\n' ' ')
say "card:  $CARDN file(s), $((CARDB / 1048576)) MiB; half-written: ${PARTS:-none}"
sync; umount "$M" || bail "cannot unmount - something is holding $M"

PORT=$(ls /dev/serial/by-id/*1a86* 2>/dev/null | head -1)
[ -n "$PORT" ] || bail "no console port"
stty -F "$PORT" 115200 raw -echo
: > "$LOG"
nohup timeout $((MAXWAIT + 180)) cat "$PORT" > "$LOG" 2>/dev/null &
CATPID=$!
sleep 1
say "[$(date +%T)] de-authorizing - the bridge should take the disk and pick up the new files"
echo 0 > "$DAUTH"

# Completion markers. "sync done:"/"sync FAILED:" is the normal one; a pass interrupted by the
# host writing to the card ends WITHOUT either, so also accept the first heartbeat that reports
# owner=host after the takeover line - that only happens once the volume is handed back.
i=0
GO_LINE=""
ENDED=""
while [ $i -lt "$MAXWAIT" ]; do
    if grep -aq "sync done:\|sync FAILED:" "$LOG"; then ENDED=marker; break; fi
    [ -z "$GO_LINE" ] && GO_LINE=$(grep -an "let go" "$LOG" 2>/dev/null | head -1 | cut -d: -f1)
    if [ -n "$GO_LINE" ] && [ "$(wc -l < "$LOG")" -gt "$GO_LINE" ]; then
        if tail -n +"$((GO_LINE + 1))" "$LOG" | grep -aq "\] owner=host"; then ENDED=handback; break; fi
    fi
    sleep 5; i=$((i + 5))
    [ $((i % 120)) -eq 0 ] && say "  ... ${i}s  $(grep -a '\.\.\. /' "$LOG" | tail -1 | cut -c1-64)"
done
[ -n "$ENDED" ] || bail "no completed pass within ${MAXWAIT}s (last lines below)"
sleep 5
kill $CATPID 2>/dev/null || true
echo 1 > "$DAUTH"
say "[$(date +%T)] re-authorized (ended on: $ENDED)"

say "--- the pass ---"
grep -a -E "let go|sync start:|continuing |  \+ |sync (done|FAILED):" "$LOG"
say "--- resumed half-written files: $(grep -ac 'continuing ' "$LOG") (prefix verified before continuing)"
say "--- warnings/errors during the pass ---"
grep -a -E "^[WE] \(" "$LOG" | grep -av "host let go" | head -12 || say "  (none)"

sleep 8
mount -o ro "$DEV" "$M" || bail "cannot remount after the pass"
card_list > /tmp/card-after.tsv
CARDN2=$(wc -l < /tmp/card-after.tsv)
CARDB2=$(sum /tmp/card-after.tsv)
say "card after: $CARDN2 file(s), $((CARDB2 / 1048576)) MiB  (was $CARDN / $((CARDB / 1048576)) MiB)"
say "still half-written: $(cd "$M" && ls *.part 2>/dev/null | tr '\n' ' ' || echo none)"

say "--- every file on the share, checked against the card ---"
BAD=0
while IFS="$TAB" read -r SZ NM; do
    CSZ=$(awk -F"$TAB" -v n="$NM" '$2 == n { print $1; exit }' /tmp/card-after.tsv)
    if [ -z "$CSZ" ]; then say "  MISSING  $NM ($SZ B on share)"; BAD=$((BAD + 1))
    elif [ "$CSZ" != "$SZ" ]; then say "  SIZE     $NM share=$SZ card=$CSZ"; BAD=$((BAD + 1))
    else say "  ok       $NM  $SZ B"; fi
done < /tmp/nas-before.tsv

if [ -n "${MD5_ALL:-}" ]; then
    N=$(wc -l < /tmp/nas-before.tsv)
    say "--- byte-level checks: ALL $N files (reads the whole library back over USB, slow) ---"
    cp /tmp/nas-before.tsv /tmp/md5-pick.tsv
else
    say "--- byte-level checks (the 2 smallest files, cheap to pull; MD5_ALL=1 for every file) ---"
    sort -t"$TAB" -k1 -n /tmp/nas-before.tsv | head -2 > /tmp/md5-pick.tsv
fi
while IFS="$TAB" read -r SZ NM; do
    [ -e "$M/$NM" ] || { say "  skip  $NM (not on the card)"; continue; }
    cm=$(md5sum "$M/$NM" | cut -d' ' -f1)
    tmp=$(mktemp)
    smbclient "//$SRV/$SHARE" -A "$CRED" -c "get \"$NM\" \"$tmp\"" >/dev/null 2>&1
    nmd=$(md5sum "$tmp" | cut -d' ' -f1); rm -f "$tmp"
    if [ "$cm" = "$nmd" ]; then say "  ok    md5 $NM"
    else say "  FAIL  md5 $NM card=$cm nas=$nmd"; BAD=$((BAD + 1)); fi
done < /tmp/md5-pick.tsv
umount "$M" 2>/dev/null || true

echo
if [ "$BAD" -eq 0 ]; then
    say "PASS: the pass copied what was new, skipped what was already there, and every file on"
    say "      the share is on the card at the right size, with the md5 spot checks matching."
else
    say "RESULT: $BAD mismatch(es) - see the list above."
fi
