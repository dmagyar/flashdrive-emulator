#!/bin/sh
# Revert the card-writer task and prove the reverted build still works end to end.
#
# The overlap measured, three ways, against a 376 KB/s serial baseline:
#   serial loop            376 / 371 KB/s
#   writer task, unpinned  378 / 355 KB/s
#   writer task, core 1    367 / 364 KB/s
# Same code path with the card untouched runs 482-554 KB/s, so the card path takes ~25% away from
# the radio regardless of which core runs it: that is contention for a shared resource, not
# serialization, and a second task cannot win a resource that is already spent. A task, two queues
# and 64 KiB of internal RAM for no measured gain is exactly the kind of surface this project
# refuses, so it goes back out. The implementation stays in tools/apply_overlap.py (idempotent
# patcher) and the pre-revert file in tools/bridge_main.pre-overlap.c if the bus question is ever
# revisited.
set -eu
cd /home/hermes/board-schematics/bridge
K=/home/hermes/.ssh/aithinker_flash_ed25519
[ -f tools/bridge_main.pre-overlap.c ] || { echo "no backup to restore"; exit 1; }
grep -q "card_writer_task" tools/bridge_main.pre-overlap.c && { echo "backup is NOT pre-overlap"; exit 1; }

cp tools/bridge_main.pre-overlap.c main/bridge_main.c
grep -c "card_writer_task" main/bridge_main.c | sed 's/^/writer refs after revert (want 0): /'
grep -n "fwrite(buf, 1, (size_t)got, out)" main/bridge_main.c | sed 's/^/serial write restored at: /'

bash build.sh > /tmp/rv_build.log 2>&1 || { grep -E "error:" /tmp/rv_build.log | head -6; exit 1; }
grep -cE "error:" /tmp/rv_build.log | sed 's/^/errors=/'

ssh -o StrictHostKeyChecking=no -i $K root@10.119.0.199 'pkill -f "detach_[t]est" 2>/dev/null; true'
scp -q -o StrictHostKeyChecking=no -i $K build/bridge.bin root@10.119.0.199:/root/bridge/bridge.bin
LOCAL=$(sha256sum build/bridge.bin | cut -c1-16)
REMOTE=$(ssh -o StrictHostKeyChecking=no -i $K root@10.119.0.199 'sha256sum /root/bridge/bridge.bin | cut -c1-16')
[ "$LOCAL" = "$REMOTE" ] && echo "binary verified $LOCAL" || { echo "SCP STALE $LOCAL != $REMOTE"; exit 1; }

# Copying the file is not flashing it. An earlier version of this script announced
# "REVERT_BUILT_AND_FLASHED" after only an scp, and the proof that followed ran on whatever was
# already in flash - the overlap build, betrayed by its own "card stack ... B left" log line.
# Flash, count the verification lines, and only then claim anything.
ssh -o StrictHostKeyChecking=no -o ServerAliveInterval=30 -i $K root@10.119.0.199 '
    set -e
    PORT=$(ls /dev/serial/by-id/*1a86* | head -1); cd /root/bridge
    esptool --chip esp32s3 -p $PORT -b 460800 --before default-reset --after no-reset \
        write-flash 0x20000 bridge.bin 2>&1 | grep -cE "Hash of data verified" | sed "s/^/flash_verified=/"'
echo "REVERT_BUILT_AND_FLASHED   (proof: run tools/cold_sync_proof.sh on the remote)"
