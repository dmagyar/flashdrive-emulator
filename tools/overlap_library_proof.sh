#!/bin/sh
# Decide the overlap question on the REAL workload, not the 2 MiB test pair.
#
# On 2 x 1 MiB files the writer task measured 378/355 KB/s against a 376/371 KB/s serial baseline -
# no gain, so it was reverted. But the full-library proofs say the opposite, on the two big files
# (59.0 MB, which excludes per-file open noise):
#     serial   158.06 s  -> 373.6 KB/s
#     overlap  142.27 s  -> 415.0 KB/s   (+11%)
# Both are N=1, and the small-file runs are short enough that the first-request penalty and the SMB
# open dominate. This re-runs the same 61 MB cold-sync proof on the overlap build. Decision rule,
# written down BEFORE the measurement so it cannot be talked around afterwards: if the overlap's
# big-file rate lands within ~3% of the serial build, the serial loop stays (simpler wins ties);
# if it lands at ~415 KB/s again, the writer task comes back.
set -eu
cd /home/hermes/board-schematics/bridge
K=/home/hermes/.ssh/aithinker_flash_ed25519
grep -q '^#define BRIDGE_DRY_RUN 0$' main/bridge_main.c || { echo "tree in dry-run state"; exit 1; }

python3 tools/apply_overlap.py
grep -c "xTaskCreatePinnedToCore" main/bridge_main.c | sed 's/^/pinned-writer present (want 1): /'
bash build.sh > /tmp/ov2_build.log 2>&1 || { grep -E "error:" /tmp/ov2_build.log | head -6; exit 1; }
grep -cE "error:" /tmp/ov2_build.log | sed 's/^/errors=/'

ssh -o StrictHostKeyChecking=no -i $K root@10.119.0.199 'pkill -f "detach_[t]est" 2>/dev/null; true'
scp -q -o StrictHostKeyChecking=no -i $K build/bridge.bin root@10.119.0.199:/root/bridge/bridge.bin
LOCAL=$(sha256sum build/bridge.bin | cut -c1-16)
REMOTE=$(ssh -o StrictHostKeyChecking=no -i $K root@10.119.0.199 'sha256sum /root/bridge/bridge.bin | cut -c1-16')
[ "$LOCAL" = "$REMOTE" ] && echo "binary verified $LOCAL" || { echo "SCP STALE $LOCAL != $REMOTE"; exit 1; }
ssh -o StrictHostKeyChecking=no -o ServerAliveInterval=30 -i $K root@10.119.0.199 '
    set -e
    PORT=$(ls /dev/serial/by-id/*1a86* | head -1); cd /root/bridge
    esptool --chip esp32s3 -p $PORT -b 460800 --before default-reset --after no-reset \
        write-flash 0x20000 bridge.bin 2>&1 | grep -cE "Hash of data verified" | sed "s/^/flash_verified=/"'

echo "=== 61 MB cold sync on the overlap build ==="
ssh -o StrictHostKeyChecking=no -o ServerAliveInterval=30 -i $K root@10.119.0.199 \
    'sh /root/bridge/cold_sync_proof.sh 2>&1; echo "attribution - card stack lines (want >=1): $(grep -ac "card stack" /tmp/sync.log)"'
echo "OVERLAP_LIBRARY_PROOF_DONE"
