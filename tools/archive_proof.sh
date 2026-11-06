#!/bin/sh
# Dated-archive proof, run ON THE BENCH.
#
#   sh archive_proof.sh <bridge.bin> <log-name> [max-seconds]
#
# Flashes the image, lets the board take the volume back, waits for its own report, hands the
# volume back to the host, and prints the lines that decide the claim:
#
#   filed  ...  (same bytes: a directory write, not a transfer)   <- migration by rename
#   sync done: 0 copied (0 bytes), N already present              <- nothing re-downloaded
#   prune skipped: dated archive on                               <- pruner stood down
#
# Three rules this encodes, each learned the hard way on this bench:
#   * capture with `cat` on the tty after `stty raw -echo`. pyserial is forbidden: its open()
#     pulses DTR/RTS, resets the board and destroys the DETACHED window mid-transfer.
#   * an app-only flash leaves the board in the bootloader; a second esptool with
#     --after hard-reset is what actually starts the app before listening.
#   * never hand the volume back on a timer. Wait for 'sync done:' / 'sync FAILED:' or a
#     bail-out is announced, or the harness itself aborts the transfer it is measuring.
# The EXIT trap hands the volume back whatever happens, so a bail-out cannot leave the bench
# holding a disk the host can no longer see.
set -eu
BIN=$1; LOG=$2; MAX=${3:-1500}; WIPE=${4:-0}
PORT=$(ls /dev/serial/by-id/*1a86* | head -1)

# ONE harness owns the bench. A second script that writes .../authorized can abort the first
# one's transfer from halfway across the room, and the aborted pass reports the *firmware* as
# failed: a leftover harness from an earlier attempt fired its forced hand-back 11 minutes into
# this proof and turned a healthy sync into "sync FAILED: 5 copied". The port has no owner field,
# so the mutex has to be the harness's own discipline. Held for the script's lifetime on fd 9.
exec 9>/tmp/bridge.lock
if ! flock -n 9; then
    echo "REFUSING TO RUN: another bench harness holds /tmp/bridge.lock." >&2
    echo "It will hand the volume back mid-pass and invalidate this run." >&2
    ps -eo pid,etime,cmd | grep -E 'archive_proof|one_pass|dates_legs|repair_v|prune_proof' | grep -v grep >&2
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

# Resolve the bridge's card by the USB vendor ID of the device itself - Espressif is 303a - never
# by a vendor/model string and never by position. Two traps measured on this bench: an unrelated
# 1.9 GB CHIPSBNK vfat stick sits next to the card (1e3d), so "the first vfat" mounts the wrong
# disk; and `lsblk -r` escapes the vendor string's trailing space as "TinyUSB\x20", so an exact
# string match silently returns nothing and a proof aborts while the hardware is healthy.
card_dev() {
    _c=""
    for _b in /sys/block/sd*; do
        [ -e "$_b" ] || continue
        if udevadm info -q property -n "/dev/$(basename "$_b")" 2>/dev/null | grep -q '^ID_VENDOR_ID=303a$'; then
            [ -n "$_c" ] && { echo "card_dev: more than one Espressif MSC volume: $_c and /dev/$(basename "$_b")" >&2; return 1; }
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
# Hand the disk back on any exit path, including a control-C here.
trap 'authorize 1 >/dev/null 2>&1 || true' EXIT INT TERM

echo "== flash $BIN =="
authorize 1 || true
esptool --chip esp32s3 -p "$PORT" --before default-reset --after no-reset \
        write_flash 0x20000 "$BIN" 2>&1 | tail -2
# The flash leaves it in the bootloader; this reset is what boots the app.
esptool --chip esp32s3 -p "$PORT" --before default-reset --after hard-reset \
        flash-id >/dev/null 2>&1
sleep 8

stty -F "$PORT" 115200 raw -echo
: > "/tmp/$LOG"
# 9>&- : the capture must not inherit the bench lock (it would hold the bench
# for its full timeout after this script exits, blocking the next run).
nohup timeout $((MAX + 180)) cat "$PORT" > "/tmp/$LOG" 2>/dev/null 9>&- &
CAT=$!
sleep 1
# Clean-slate run. The bridge, not the harness, decides who holds the volume: authorized=1 alone
# does NOT hand it over - the firmware gives it up when a pass finishes. So the wipe runs between
# passes: take the volume back, let it complete (fast - the archive already holds these tracks),
# let it hand over, and only then is the host allowed to clear the card.
if [ "$WIPE" = "1" ]; then
    echo "== warm-up pass, so the bridge hands the volume over =="
    authorize 0 || true
    i=0
    while [ "$i" -lt 600 ]; do
        grep -aq 'sync done:\|sync FAILED:' "/tmp/$LOG" && break
        sleep 5; i=$((i + 5))
    done
    sleep 3
    authorize 1 || true
    DEV=""
    i=0
    while [ "$i" -lt 40 ]; do
        DEV=$(card_dev 2>/dev/null) && break
        sleep 2; i=$((i + 2))
    done
    [ -n "$DEV" ] || { echo "wipe: no Espressif MSC volume appeared after hand-back"; exit 1; }
    sleep 3
    if ! mount -o rw "$DEV" /mnt/car 2>/dev/null; then
        echo "wipe: $DEV would not mount rw - refusing to continue (the bridge no longer formats it)"
        exit 1
    fi
    find /mnt/car -mindepth 1 -maxdepth 1 ! -name lost+found -exec rm -rf {} + 2>/dev/null || true
    echo "wipe: card cleared, $(find /mnt/car -mindepth 1 -name '*.mp3' | wc -l) mp3 left"
    sync; umount /mnt/car
    # The pass counter greps for a completion line; the warm-up pass already left one in the file.
    : > "/tmp/$LOG"
fi

authorize 0                                  # the host lets go; the bridge runs a pass
i=0; done_=0
while [ "$i" -lt "$MAX" ]; do
    if grep -aq 'sync done:\|sync FAILED:' "/tmp/$LOG"; then done_=1; break; fi
    if [ $((i % 60)) -eq 0 ]; then
        last=$(tr -d '\r' < "/tmp/$LOG" | grep -a ' bridge: ' | tail -1 | cut -c1-78)
        echo "  [${i}s] ${last:-waiting for the bridge to start its pass}"
    fi
    sleep 5; i=$((i + 5))
done
sleep 2
kill "$CAT" 2>/dev/null
[ "$done_" = 1 ] || echo "  BAILOUT: pass still running after ${MAX}s - hand-back forced"
authorize 1
trap - EXIT INT TERM
i=0
while [ ! -b /dev/sdb ] && [ "$i" -lt 40 ]; do sleep 1; i=$((i + 1)); done

if [ "$WIPE" = "1" ]; then
    echo "== card as a reader sees it =="
    CARD=$(card_dev 2>/dev/null || true)
    if [ -n "$CARD" ] && mount -o ro "$CARD" /mnt/car 2>/dev/null; then
        echo "  root mp3  : $(find /mnt/car -maxdepth 1 -name '*.mp3' | wc -l)   (want 0 in archive mode)"
        echo "  in folders: $(find /mnt/car -mindepth 2 -name '*.mp3' | wc -l)"
        echo "  .part left: $(find /mnt/car -name '*.part' | wc -l)"
        find /mnt/car -mindepth 1 | sed "s|/mnt/car/||" | sort | head -24
        df -h /mnt/car | tail -1
        sync; umount /mnt/car
    else
        echo "  could not mount the card read-only"
    fi
fi

echo "== the bridge's own report =="
tr -d '\r' < "/tmp/$LOG" | grep -aE 'config:|filed |retired |archive:|dates:|prune |sync done:|sync FAILED' \
    | sed -e 's/^I ([0-9]*) bridge:/  /' -e 's/^W ([0-9]*) bridge:/  W /'
