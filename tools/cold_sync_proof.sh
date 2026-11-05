#!/bin/sh
# Cold-sync proof on HermesBoard: wipe every synced file off the card, let the bridge rebuild
# the whole library from the NAS while the host is detached, then compare the card against the
# NAS byte for byte. Run as: sh /root/bridge/cold_sync_proof.sh   (takes ~9 min)
#
# Ordering matters: the card is only reachable from Linux while the host owns the disk, which
# happens after a detach cycle hands it back - see detach_test.sh for why the console has to be
# captured with cat and not pyserial.
set -eu
WAIT="${1:-330}"

echo "[1/4] boot the app and hand the disk back to the host"
sh /root/bridge/detach_test.sh 20 10 >/dev/null 2>&1 || true
i=0
while [ ! -b /dev/sdb ] && [ $i -lt 25 ]; do sleep 1; i=$((i+1)); done
[ -b /dev/sdb ] || { echo "FAIL: the card never enumerated"; exit 1; }

echo "[2/4] wipe the synced files (keep the BRIDGE marker)"
mount /dev/sdb /mnt/card
find /mnt/card -maxdepth 1 -name '*.mp3' -print -delete
find /mnt/card -maxdepth 1 -name '*.part' -print -delete
sync; umount /mnt/card
sleep 2

echo "[3/4] cold sync, ${WAIT} s detached"
sh /root/bridge/detach_test.sh "$WAIT" 20 >/dev/null 2>&1 || true
tr -d '\r' < /tmp/sync.log | grep -E 'get |\+ |copied|FAILED|continuing|rename'

echo "[4/4] card vs NAS"
# the re-attached volume needs a few seconds to enumerate; mounting straight away fails with
# "Can't lookup blockdev"
i=0
while [ ! -b /dev/sdb ] && [ $i -lt 30 ]; do sleep 1; i=$((i+1)); done
sh /root/bridge/verify_card_vs_nas.sh 2>&1 | tail -8
echo "PROOF_DONE"
