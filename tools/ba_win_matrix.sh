#!/bin/sh
# Run one A/B arm of the throughput measurement against a prepared image.
#
#   sh tools/ba_win_matrix.sh ARM_LABEL /path/to/bridge.bin [reps] [files...]
#
# Intended to be called once per arm, from the dev host, by ba_win_arms.sh which owns the
# comparison. What this does: flash the given image, boot it, wait for the card to enumerate, then
# run `reps` timed passes over the SAME file list. Every pass deletes those files from the card
# first, so each pass transfers the same bytes - the only thing an arm changes is the firmware.
#
# Design rules this follows, each learned the hard way in this project:
#   - ssh gets ConnectTimeout. A morning was lost to an scp against a dead host with no timeout.
#   - nothing is reported unless a pass actually completed; a missing pass is a FAIL, not a zero.
#   - the rate is bytes *fetched* (from the share) over the firmware's own measured duration, never
#     s_copied_bytes, which counts a resumed prefix it did not transfer.
set -u
K=${KEY:-/home/hermes/.ssh/aithinker_flash_ed25519}
HOST=${BENCH:-root@10.119.0.199}
B=/root/bridge
SSH="ssh -o StrictHostKeyChecking=no -o ConnectTimeout=6 -o ServerAliveInterval=15 -i $K $HOST"
SCP="scp -q -o StrictHostKeyChecking=no -o ConnectTimeout=6 -i $K"

ARM=${1:?usage: ba_win_matrix.sh ARM_LABEL image [reps] [files...]}; shift
IMG=${1:?usage: ba_win_matrix.sh ARM_LABEL image [reps] [files...]}; shift
REPS=${1:-2}; shift || true
FILES=${*:-"/01.mp3 /02.mp3 /03.mp3"}

[ -f "$IMG" ] || { echo "FAIL: no image at $IMG"; exit 1; }
SHA=$(sha256sum "$IMG" | cut -c1-16)

echo "== arm $ARM  image $(basename "$IMG") sha256:$SHA  reps=$REPS  files:$FILES"

$SSH 'echo bench-ok' >/dev/null 2>&1 || { echo "FAIL: bench $HOST unreachable (ConnectTimeout 6s) - nothing was measured"; exit 1; }

echo "[1/4] flash"
$SCP "$IMG" "$HOST:$B/bridge.bin" || { echo "FAIL: scp"; exit 1; }
$SSH "
    set -e
    PORT=\$(ls /dev/serial/by-id/*1a86* | head -1); cd $B
    esptool --chip esp32s3 -p \$PORT -b 460800 --before default-reset --after no-reset \
        write-flash 0x20000 bridge.bin 2>&1 | grep -c 'Hash of data verified' | sed 's/^/verified=/'
    # --after no-reset leaves it in the bootloader; detach_test.sh re-enters flash-id with
    # --after hard-reset, which is what actually starts the app.
    sh $B/detach_test.sh 20 10 >/dev/null 2>&1 || true
    i=0; while [ ! -b /dev/sdb ] && [ \$i -lt 40 ]; do sleep 1; i=\$((i+1)); done
    [ -b /dev/sdb ] || { echo 'FAIL: card never enumerated - the app is not running'; exit 1; }
    echo 'card=up'" || { echo "FAIL: flash/boot"; exit 1; }

echo "[2/4] the file list, sized from the share (authoritative)"
NEED=0
for f in $FILES; do
    sz=$($SSH "smbclient //10.119.0.4/CAR-MEDIA -A /root/.smbcreds -c 'ls \"$f\"' 2>/dev/null | \
        awk '{ for (i = 2; i <= NF; i++) if (\$i ~ /^[0-9]+\$/ && \$(i+1) ~ /^[A-Z][a-z][a-z]\$/) { print \$i; exit } }'")
    [ -n "$sz" ] || { echo "FAIL: $f is not on the share"; exit 1; }
    NEED=$((NEED + sz))
    echo "     $f  $sz B"
done
echo "     per pass: $NEED B"

echo "[3/4] timed passes"
i=1
while [ $i -le "$REPS" ]; do
    $SCP tools/ba_win_ab.sh "$HOST:$B/ba_win_ab.sh" || { echo "FAIL: cannot install ba_win_ab.sh"; exit 1; }
    $SSH "sh $B/ba_win_ab.sh $ARM-r$i $FILES" | sed 's/^/   /'
    i=$((i + 1))
done

echo "[4/4] arm $ARM: $NEED B per pass over $REPS reps - see the RESULT lines above"
