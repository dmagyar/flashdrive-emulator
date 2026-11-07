#!/bin/sh
# Prove the bytes that landed on the card are the bytes on the NAS.
#   card side: mount READ-ONLY (the bridge owns this volume's FAT state; rw from the host
#              while it thinks it owns the disk is how you get a corrupt tree)
#   NAS side : smbclient get, creds from the 600-perm file, never argv
# Assumes the bridge's current provisioning: SMB_PATH=/ and LOCAL_DIR=/ (root of the share
# onto the root of the card). Run on HermesBoard, where the card shows up as /dev/sdb.
set -u
CRED=/root/.smbcreds
SRV=${SRV:-10.119.0.4}
SHARE=${SHARE:-CAR-MEDIA}
M=/mnt/card
mkdir -p "$M"
umount "$M" 2>/dev/null || true
mount -o ro /dev/sdb "$M" || { echo "ABORT: cannot mount /dev/sdb read-only"; exit 1; }

W=$(mktemp -d)
RES=$W/results
( cd "$M" && find . -type f ! -path './BRIDGE/*' | sed 's|^\./||' ) | while IFS= read -r f; do
    card=$(md5sum "$M/$f" | cut -d' ' -f1)
    csize=$(stat -c%s "$M/$f")
    rm -f "$W/dl.bin"
    smbclient "//$SRV/$SHARE" -A "$CRED" -c "get \"$f\" \"$W/dl.bin\"" >/dev/null 2>&1
    if [ -f "$W/dl.bin" ]; then
        nas=$(md5sum "$W/dl.bin" | cut -d' ' -f1)
        nsize=$(stat -c%s "$W/dl.bin")
    else
        nas="MISSING" nsize=0
    fi
    if [ "$card" = "$nas" ] && [ "$csize" = "$nsize" ]; then
        echo "ok    $f  $csize B"
    else
        echo "MISMATCH $f  card=$csize B $card / nas=$nsize B $nas"
    fi
done > "$RES" 2>&1

cat "$RES"
OK=$(grep -c '^ok' "$RES" || true)
BAD=$(grep -c 'MISMATCH' "$RES" || true)
echo "--- $OK identical, $BAD mismatched ---"
umount "$M" 2>/dev/null || true
rm -rf "$W"
[ "$BAD" = "0" ] && [ "$OK" != "0" ]
