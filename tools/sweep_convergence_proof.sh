#!/bin/sh
# Scratch-sweep and convergence proof, run ON THE BENCH.
#
#   sh sweep_convergence_proof.sh <bridge.bin> <log-name> [max-seconds-per-pass]
#
# Two claims, both decided by the board's own log and by what the card looks like afterwards:
#
#   1. SWEEP - a .part the share can no longer finish is deleted, and the two kinds of file it
#      must never touch (a name the media filter excludes, and the BRIDGE status folder) survive.
#      The bench plants all three before the pass, so "what changed" is a measured diff, not an
#      inference: exactly one file goes.
#   2. CONVERGENCE - a second pass over the same card, with nothing touched in between, must copy
#      nothing. If the recorder rotated the share between the two passes, the copy is only allowed
#      to be that new chunk; the share listing from before each pass is printed so the difference
#      is visible in the report rather than argued about.
#
# Inherits every bench rule from archive_proof.sh: the flock on fd 9 (ONE harness owns the bench),
# capture with stty+cat because pyserial resets the board, flash-id --after hard-reset because an
# app-only flash leaves the board in the bootloader, children closing fd 9 with 9>&-, the card found
# by USB vendor 303a, and the volume handed back on every exit path. The bridge decides who holds
# the disk, so there is a warm-up pass before the planted files: authorized=1 alone does not hand
# it over, a finished pass does.
#
# Nothing here wipes the card. The planted files are removed at the end, whatever the verdict.
set -eu
BIN=$1; LOG=$2; MAX=${3:-900}
PORT=$(ls /dev/serial/by-id/*1a86* | head -1)
SHARE_LIST_S1=/tmp/share-before-pass1.txt
SHARE_LIST_S2=/tmp/share-before-pass2.txt

exec 9>/tmp/bridge.lock
if ! flock -n 9; then
    echo "REFUSING TO RUN: another bench harness holds /tmp/bridge.lock." >&2
    ps -eo pid,etime,cmd | grep -E 'archive_proof|sweep_convergence|one_pass|prune_proof' | grep -v grep >&2
    exit 1
fi
[ -n "$PORT" ] || { echo "no 1a86 console port"; exit 1; }
[ -f "$BIN" ] || { echo "no image at $BIN"; exit 1; }

D=""
for d in /sys/bus/usb/devices/*; do
    [ -e "$d/idVendor" ] || continue
    [ "$(cat "$d/idVendor" 2>/dev/null)" = 303a ] && D=$d
done
[ -n "$D" ] || { echo "ESP32 not on the bus"; exit 1; }
DAUTH="$D/authorized"

card_dev() {
    _c=""
    for _b in /sys/block/sd*; do
        [ -e "$_b" ] || continue
        if udevadm info -q property -n "/dev/$(basename "$_b")" 2>/dev/null | grep -q '^ID_VENDOR_ID=303a$'; then
            [ -n "$_c" ] && { echo "card_dev: more than one Espressif MSC volume" >&2; return 1; }
            _c="/dev/$(basename "$_b")"
        fi
    done
    [ -n "$_c" ] || { echo "card_dev: no Espressif (303a) mass-storage volume on the bus" >&2; return 1; }
    printf '%s\n' "$_c"
}
authorize () {
    i=0
    while [ "$i" -lt 10 ]; do
        if echo "$1" > "$DAUTH" 2>/dev/null; then
            [ "$(cat "$DAUTH" 2>/dev/null)" = "$1" ] && return 0
        fi
        i=$((i + 1)); sleep 2
    done
    echo "  WARN: authorized=$1 not reached (now $(cat "$DAUTH" 2>/dev/null))"
    return 1
}
wait_for_volume () {          # the bridge hands the volume over a few seconds after a pass ends
    DEV=""
    i=0
    while [ "$i" -lt 60 ]; do
        DEV=$(card_dev 2>/dev/null) && break
        sleep 2; i=$((i + 2))
    done
    [ -n "$DEV" ] || { echo "  no Espressif MSC volume appeared" >&2; return 1; }
    sleep 3
    printf '%s\n' "$DEV"
}
passes_done () { tr -d '\r' < "/tmp/$LOG" | grep -ac 'sync done:\|sync FAILED:' || true; }
wait_passes () {              # wait for the Nth completed pass; never hand the disk back on a timer
    want=$1; i=0
    while [ "$i" -lt "$MAX" ]; do
        [ "$(passes_done)" -ge "$want" ] && return 0
        if [ $((i % 60)) -eq 0 ]; then
            last=$(tr -d '\r' < "/tmp/$LOG" | grep -a ' bridge: ' | tail -1 | cut -c1-78)
            echo "  [${i}s] pass $want of 3: ${last:-waiting for the bridge}"
        fi
        sleep 5; i=$((i + 5))
    done
    echo "  BAILOUT: pass $want still running after ${MAX}s"
    return 1
}
share_list () {               # creds from the 600 file, never in argv; name + attr + size + time
    smbclient //10.119.0.4/CAR-MEDIA -A /root/.smbcreds -c 'ls' 2>/dev/null \
        | grep -i '\.mp3' | tr -s ' ' | cut -d' ' -f2- | sort > "$1"
    echo "  share: $(wc -l < "$1") entries -> $1"
}
mount_ro_card () {
    # Find the volume, mount it read-only, and hand the device name back to the caller. A function
    # that mounts and stays silent leaves the caller with an empty $DEV: the first attempt at the
    # rw mount then failed "Bad value for source" on a perfectly healthy card.
    DEV=$(wait_for_volume) || return 1
    mount -o ro "$DEV" /mnt/car 2>/dev/null || { echo "  could not mount $DEV read-only" >&2; return 1; }
    printf '%s\n' "$DEV"
}

trap 'authorize 1 >/dev/null 2>&1 || true' EXIT INT TERM

if [ "${PREFLIGHT:-0}" = "1" ]; then
    # Bench plumbing only: no flash, no pass. Proves card_dev/wait_for_volume/mount_ro_card and the
    # rw remount hand the right device over, in seconds. A harness that dies at its first mount has
    # already cost a flash and a pass to get there, so this runs first and costs nothing.
    echo "== preflight: bench plumbing only =="
    authorize 1 || true
    D0=$(mount_ro_card) || { echo "PREFLIGHT: no volume to mount - is the bridge idle with the host authorised?"; exit 1; }
    echo "  found $D0, mounted ro: $(find /mnt/car -maxdepth 1 -mindepth 1 | wc -l) entries at the root"
    sync; umount /mnt/car
    if mount -o rw "$D0" /mnt/car; then
        echo "  rw remount OK: $(find /mnt/car -name '*.mp3' | wc -l) mp3, $(find /mnt/car -name '*.part' | wc -l) .part"
        sync; umount /mnt/car
    else
        echo "PREFLIGHT: rw remount FAILED on $D0"; exit 1
    fi
    echo "== PREFLIGHT: OK =="
    exit 0
fi

echo "== flash $BIN =="
# Record WHICH BYTES go into flash: a proof belongs to the image in the chip, not to the source tree,
# and the bench deletes the copy it was handed. Without this line the question "did the board really
# run the build under test?" is unanswerable after the run.
echo "image sha256: $(sha256sum "$BIN" | cut -d' ' -f1)  size: $(stat -c %s "$BIN")"
authorize 1 || true
esptool --chip esp32s3 -p "$PORT" --before default-reset --after no-reset \
        write_flash 0x20000 "$BIN" 2>&1 | tail -2
esptool --chip esp32s3 -p "$PORT" --before default-reset --after hard-reset \
        flash-id >/dev/null 2>&1
sleep 8

stty -F "$PORT" 115200 raw -echo
: > "/tmp/$LOG"
nohup timeout $((MAX * 3 + 600)) cat "$PORT" > "/tmp/$LOG" 2>/dev/null 9>&- &
CAT=$!
sleep 1

echo "== warm-up pass: the bridge takes the disk, then gives it up =="
authorize 0 || true
wait_passes 1 || { kill "$CAT" 2>/dev/null; exit 1; }
sleep 2
authorize 1
share_list "$SHARE_LIST_S1"

echo "== plant three files on the card (host owns the volume now) =="
DEV=$(mount_ro_card) || { kill "$CAT" 2>/dev/null; exit 1; }
umount /mnt/car
if ! mount -o rw "$DEV" /mnt/car; then
    echo "plant: $DEV would not mount rw - refusing to continue"; exit 1
fi
mkdir -p /mnt/car/20261105
echo "not a whole track, and its share entry is long gone" > /mnt/car/20261105/235959_00.mp3.part
echo "the owner of this card put this here"                > /mnt/car/owner-notes.txt.part
echo "the write probes scratch, not the bridges"           > /mnt/car/BRIDGE/zz-host-probe.part
MP3_BEFORE=$(find /mnt/car -name '*.mp3' | wc -l)
echo "  planted: 20261105/235959_00.mp3.part (abandoned), owner-notes.txt.part (not ours)," \
     "BRIDGE/zz-host-probe.part (status folder)"
echo "  card before: $MP3_BEFORE mp3, $(find /mnt/car -name '*.part' | wc -l) .part"
sync; umount /mnt/car

echo "== PASS 1: the measured sweep =="
authorize 0
wait_passes 2 || { kill "$CAT" 2>/dev/null; exit 1; }
sleep 2
authorize 1
DEV=$(mount_ro_card) || { kill "$CAT" 2>/dev/null; exit 1; }
echo "  card after pass 1:"
find /mnt/car -name '*.part' -printf '    %P\n' | sort
MP3_AFTER=$(find /mnt/car -name '*.mp3' | wc -l)
# Three separate existence checks, not a count: a count would also move when the boot write
# probe leaves its own scratch down, and that is not what this proof is about.
GONE=0; if [ ! -e /mnt/car/20261105/235959_00.mp3.part ]; then GONE=1; fi
GUARD_FILTER=0; if [ -e /mnt/car/owner-notes.txt.part ]; then GUARD_FILTER=1; fi
GUARD_BRIDGE=0; if [ -e /mnt/car/BRIDGE/zz-host-probe.part ]; then GUARD_BRIDGE=1; fi
SYNC1=$(tr -d '\r' < "/tmp/$LOG" | grep -a 'sync done:\|sync FAILED:' | sed -n '2p' | sed 's/^I ([0-9]*) bridge:/  /')
echo "    mp3 count: $MP3_AFTER (was $MP3_BEFORE)"
echo "    abandoned .part gone: $GONE   filter-guarded survived: $GUARD_FILTER   BRIDGE-guarded survived: $GUARD_BRIDGE"
echo "    pass 1 reported:$SYNC1"
sync; umount /mnt/car

echo "== PASS 2: convergence over an untouched card =="
share_list "$SHARE_LIST_S2"
authorize 0
wait_passes 3 || { kill "$CAT" 2>/dev/null; exit 1; }
sleep 2
authorize 1
DEV=$(mount_ro_card) || { kill "$CAT" 2>/dev/null; exit 1; }
SYNC2=$(tr -d '\r' < "/tmp/$LOG" | grep -a 'sync done:\|sync FAILED:' | sed -n '3p' | sed 's/^I ([0-9]*) bridge:/  /')
MP3_FINAL=$(find /mnt/car -name '*.mp3' | wc -l)
echo "    mp3 count: $MP3_FINAL"
echo "    pass 2 reported:$SYNC2"

# ---- clean up the planted files; the verdict is already in the log and the listing above ----
umount /mnt/car
if mount -o rw "$DEV" /mnt/car 2>/dev/null; then
    rm -f /mnt/car/owner-notes.txt.part /mnt/car/BRIDGE/zz-host-probe.part
    rmdir /mnt/car/20261105 2>/dev/null || true
    echo "  cleanup: planted files removed, $(find /mnt/car -name '*.part' | wc -l) .part left"
    sync; umount /mnt/car
else
    echo "  cleanup SKIPPED: $DEV would not mount rw - the planted files are still on the card"
fi

echo "== the bridge's own report =="
tr -d '\r' < "/tmp/$LOG" | grep -aE 'config:|x .*\.mp3|scratch:|filed |retired |archive:|dates:|prune |sync done:|sync FAILED|boot|Guru|abort' \
    | sed -e 's/^I ([0-9]*) bridge:/  /' -e 's/^W ([0-9]*) bridge:/  W /' -e 's/^E ([0-9]*) bridge:/  E /'
BOOT=$(tr -d '\r' < "/tmp/$LOG" | grep -acE 'cpu_start|boot: ESP-IDF|Reset reason' || true)
kill "$CAT" 2>/dev/null
trap - EXIT INT TERM

echo "== the share, before each pass =="
if diff -q "$SHARE_LIST_S1" "$SHARE_LIST_S2" >/dev/null; then
    echo "  identical between the passes - so a converged pass must copy 0 bytes"
    SHARE_CHANGED=0
else
    echo "  the recorder rotated the share between the passes; the delta is:"
    diff "$SHARE_LIST_S1" "$SHARE_LIST_S2" | sed 's/^/    /' | head -12
    SHARE_CHANGED=1
fi

# -------------------------------------------------------------- the verdict
FAIL=""
[ "$MP3_AFTER" = "$MP3_BEFORE" ] || FAIL="$FAIL mp3-count-changed-in-pass1($MP3_BEFORE->$MP3_AFTER)"
# the abandoned scratch is gone and the two guarded ones are still there
[ "$GONE" = "1" ] || FAIL="$FAIL abandoned-part-still-on-card"
[ "$GUARD_FILTER" = "1" ] || FAIL="$FAIL filter-guarded-part-was-deleted"
[ "$GUARD_BRIDGE" = "1" ] || FAIL="$FAIL BRIDGE-folder-part-was-deleted"
tr -d '\r' < "/tmp/$LOG" | grep -aq 'x 20261105/235959_00.mp3' \
    || FAIL="$FAIL sweep-did-not-report-the-abandoned-part"
tr -d '\r' < "/tmp/$LOG" | grep -aq 'scratch: 1 abandoned .part file(s) removed' \
    || FAIL="$FAIL scratch-tally-not-1"
tr -d '\r' < "/tmp/$LOG" | grep -a 'sync done:\|sync FAILED:' | sed -n '2p' | grep -aq '0 deferred' \
    || FAIL="$FAIL pass1-had-deferrals"
if [ "$SHARE_CHANGED" = "0" ]; then
    echo "$SYNC2" | grep -aq '0 copied (0 bytes)' \
        || FAIL="$FAIL pass2-copied-what-was-already-there"
fi
echo "$SYNC2" | grep -aq '0 deferred' || FAIL="$FAIL pass2-had-deferrals"
[ "$BOOT" = "0" ] || FAIL="$FAIL reboot-happened(boot-banners=$BOOT)"
if tr -d '\r' < "/tmp/$LOG" | grep -aqE 'Guru|abort\(\)'; then FAIL="$FAIL crash-in-log"; fi

if [ -z "$FAIL" ]; then
    echo "== VERDICT: PASS =="
else
    echo "== VERDICT: FAIL ==$FAIL"
fi
