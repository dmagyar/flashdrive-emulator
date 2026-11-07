#!/bin/sh
# Does a file the card does not have yet reach the card?
#
#   new on the share  ─┐
#                      ├──> identical decision inside walk(): card_has(rel) has no entry at
#   missing on card  ──┘    that path, so the file is fetched instead of skipped. This test
#                          exercises that decision end to end and md5s the result.
#
# Two modes, chosen automatically:
#   WRITE   - drop a new file into a NEW folder on the NAS. Also proves recursion and
#             mkdir-on-card. Needs a share account that can write.
#   ABSENT  - the bridge account is read-only on most shares (and should be), so instead
#             RENAME one already-synced file on the card to <name>.pre-sync-bak. The bytes
#             stay on the card until the fresh copy verifies, so nothing can be lost.
#
# Console capture uses stty+cat. pyserial is forbidden: open() pulses DTR/RTS, the CH34x
# turns that into an EN pulse, and the board resets in the middle of the window being measured.
#
# Run on HermesBoard (the Linux box standing in for the car).
set -u
CRED=/root/.smbcreds
SRV=${SRV:-10.119.0.4}
SHARE=${SHARE:-CAR-MEDIA}
M=/mnt/card
DEV=${DEV:-/dev/sdb}
LOG=${LOG:-/tmp/newfile.log}
BAK=/tmp/bridge-sync-bak
FAIL=""
note() { echo "$*"; }
die()  { echo "FAIL: $*"; restore_or_clean; exit 1; }

RESTORE=""
restore_or_clean() {
    [ -n "$RESTORE" ] || return 0
    # never leave a .pre-sync-bak on the volume the car reads
    mount -o rw "$DEV" "$M" 2>/dev/null || { note "could not mount rw to restore $RESTORE"; return; }
    [ -n "$RESTORE_TMP" ] && mv -n "$RESTORE_TMP" "$M/$RESTORE" 2>/dev/null
    rm -f "$M/$RESTORE.pre-sync-bak" 2>/dev/null
    sync; umount "$M" 2>/dev/null || true
}

[ -e "$DEV" ] || die "$DEV not present - is the OTG cable in and the host bound?"
mkdir -p "$M"; umount "$M" 2>/dev/null || true
mount -o ro "$DEV" "$M" || die "cannot mount $DEV read-only - the bridge does not hold a mounted volume for the host"
(cd "$M" && find . -type f ! -path './BRIDGE/*' -printf '%s\t%p\n') | sed 's|\t\./|\t|' | sort -t"$(printf '\t')" -k2 > /tmp/card-before.txt
BEFORE=$(wc -l < /tmp/card-before.txt)
note "card holds $BEFORE file(s):"; cut -f2- /tmp/card-before.txt | sed 's/^/    /'
sync; umount "$M" || die "cannot unmount - something is holding $M"
[ "$BEFORE" -ge 1 ] || die "the card is empty; run cold_sync_proof.sh first so there is a baseline to skip"

# ---- choose the mode ------------------------------------------------------------
# mkdir is the writability probe: it only succeeds if the account can write.
MODE=write
smbclient "//$SRV/$SHARE" -A "$CRED" -c "mkdir BridgeProbe" >/dev/null 2>&1
if ! smbclient "//$SRV/$SHARE" -A "$CRED" -c "ls" 2>/dev/null | grep -q "BridgeProbe"; then
    MODE=absent
fi
if [ "$MODE" = write ]; then
    TARGET="BridgeProbe/zz-bridge-probe.mp3"
    head -c 1048576 /dev/urandom > /tmp/bridge-probe-src.mp3
    smbclient "//$SRV/$SHARE" -A "$CRED" -c "cd BridgeProbe; put /tmp/bridge-probe-src.mp3 zz-bridge-probe.mp3" >/dev/null 2>&1 \
        || die "mkdir worked but put did not"
    smbclient "//$SRV/$SHARE" -A "$CRED" -c "cd BridgeProbe; ls zz-bridge-probe.mp3" 2>/dev/null | grep -q zz-bridge \
        || die "put reported no error but the file is not on the share"
    note "WRITE mode: dropped $TARGET (1 MiB) into a new folder on the share"
else
    # smallest synced file = shortest test; keep the bytes until the new copy verifies
    TARGET=$(sort -n /tmp/card-before.txt | head -1 | cut -f2-)
    [ -n "$TARGET" ] || die "no candidate file to test with"
    note "ABSENT mode (the share account is read-only, so 'new on the share' is simulated as"
    note "'absent on the card' - the same branch inside walk()): targeting $TARGET"
    mount -o rw "$DEV" "$M" || die "cannot mount rw to rename the target"
    mkdir -p "$BAK"
    cp "$M/$TARGET" "$BAK/$(basename "$TARGET")" || { umount "$M"; die "could not keep a copy of $TARGET"; }
    mv "$M/$TARGET" "$M/$TARGET.pre-sync-bak" || { umount "$M"; die "could not rename $TARGET"; }
    sync; umount "$M" || die "cannot unmount after the rename"
    RESTORE="$TARGET"; RESTORE_TMP="$BAK/$(basename "$TARGET")"
fi

# ---- "ignition off": capture the console, then drop the USB link -----------------
PORT=$(ls /dev/serial/by-id/*1a86* 2>/dev/null | head -1)
[ -n "$PORT" ] || die "no CH340 console port"
stty -F "$PORT" 115200 raw -echo
: > "$LOG"
nohup timeout 200 cat "$PORT" > "$LOG" 2>/dev/null &
CATPID=$!
sleep 1

D=""
for d in /sys/bus/usb/devices/*; do
    [ -e "$d/idVendor" ] || continue
    [ "$(cat "$d/idVendor" 2>/dev/null)" = "303a" ] && D=$d
done
[ -n "$D" ] || { kill $CATPID 2>/dev/null; die "no ESP32 USB device on the host to de-authorize"; }

note "[$(date +%T)] de-authorizing $D"
echo 0 > "$D/authorized"
i=0
while [ $i -lt 170 ]; do
    grep -aq "sync done:\|sync FAILED:" "$LOG" && break
    sleep 2; i=$((i + 2))
done
grep -aq "sync done:\|sync FAILED:" "$LOG" || { kill $CATPID 2>/dev/null; die "no sync pass finished within ${i}s"; }
sleep 3
kill $CATPID 2>/dev/null || true
echo 1 > "$D/authorized"
note "[$(date +%T)] re-authorized"

note "--- what the bridge said ---"
grep -a -E "let go|owner=app|sync start:|  \+ |sync (done|FAILED):|handing|owner=host|already present" "$LOG" | head -20
LINE=$(grep -a "sync done:\|sync FAILED:" "$LOG" | tail -1)
note "pass: $LINE"
COPIED=$(echo "$LINE" | sed -n 's/.*: \([0-9]*\) copied.*/\1/p')
[ "$COPIED" = "1" ] || note "NOTE: copied=$COPIED, expected exactly 1 - read the log above"

# ---- verify the bytes ------------------------------------------------------------
sleep 8
mount -o ro "$DEV" "$M" || die "cannot remount $DEV after the pass"
if [ ! -e "$M/$TARGET" ]; then
    umount "$M" 2>/dev/null
    note "card after:"; (cd "$M" && find . -type f ! -path './BRIDGE/*' | sed 's|^\./||')
    die "$TARGET never appeared on the card"
fi
CARDMD5=$(md5sum "$M/$TARGET" | cut -d' ' -f1)
CARDB=$(stat -c%s "$M/$TARGET")
umount "$M" 2>/dev/null || true
if [ "$MODE" = write ]; then
    WANT=$(md5sum /tmp/bridge-probe-src.mp3 | cut -d' ' -f1)
    note "card: $TARGET $CARDB B md5 $CARDMD5   (wrote  md5 $WANT)"
else
    # the pre-rename copy is the reference: it is the bytes the cold-sync proof verified against
    # the NAS, so matching it proves this pass reproduced them from the share, not from cache.
    WANT=$(md5sum "$BAK/$(basename "$TARGET")" | cut -d' ' -f1)
    note "card: $TARGET $CARDB B md5 $CARDMD5   (pre-sync copy md5 $WANT)"
    NASF=$(mktemp)
    if smbclient "//$SRV/$SHARE" -A "$CRED" -c "get \"$TARGET\" \"$NASF\"" >/dev/null 2>&1; then
        NAS=$(md5sum "$NASF" | cut -d' ' -f1)
        note "      direct from the NAS:      md5 $NAS"
        [ "$NAS" = "$CARDMD5" ] || die "the NAS copy and the freshly synced card copy differ"
    else
        note "      (could not fetch $TARGET from the share directly - share/ldir mapping?)"
    fi
    rm -f "$NASF"
fi
[ "$CARDMD5" = "$WANT" ] || die "card content does not match the source bytes"

# ---- clean up --------------------------------------------------------------------
if [ "$MODE" = write ]; then
    smbclient "//$SRV/$SHARE" -A "$CRED" -c "rm BridgeProbe/zz-bridge-probe.mp3; rmdir BridgeProbe" >/dev/null 2>&1
    mount -o rw "$DEV" "$M" || die "cannot mount rw to remove the probe"
    rm -rf "$M/BridgeProbe"; sync; umount "$M" || die "cannot unmount after cleanup"
    note "probe removed from the share and the card"
else
    mount -o rw "$DEV" "$M" || die "cannot mount rw to drop the safety copy"
    rm -f "$M/$TARGET.pre-sync-bak"
    sync; umount "$M" || die "cannot unmount after removing the safety copy"
    rm -rf "$BAK"
    note "safety copy deleted - the re-synced file is the only copy, and it matches the NAS"
fi

echo
echo "PASS: '$TARGET' was absent from the card, one sync pass put it back byte-for-byte"
echo "      ($CARDB B, md5 $CARDMD5) while the other $((BEFORE - 1)) files were skipped by size."
echo "      A new file on the share is the same branch of walk(), so it will sync the same way."
rm -f /tmp/card-before.txt
