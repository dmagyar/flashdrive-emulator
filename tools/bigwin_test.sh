#!/bin/sh
# Does a bigger TCP window actually help, once it is properly backed?
#
# Baseline measured today: 8 KiB chunk / 16 KiB window / SACK off / pool 16  -> 4.0 s and 3.8 s
# per 1 MiB. The 32 KiB chunk run under that stack took 23.0 s and 12.8 s. This script rebuilds
# with 64 KiB window + SACK + a 48-buffer pbuf pool + 32 KiB reads and repeats the same timed
# two-file workload, so the comparison is the same files over the same link.
#
# Sequential on purpose: a rebuild running while the binary is being scp'd ships the old image
# (build/bridge.bin is momentarily absent), which already fooled one test.
set -eu
cd /home/hermes/board-schematics/bridge
K=/home/hermes/.ssh/aithinker_flash_ed25519
B=/root/bridge

echo "[1/4] build (full: sdkconfig changed)"
if ! bash build.sh > /tmp/bigwin_build.log 2>&1; then
    echo "BUILD FAILED:"; grep -E "error:|overflow|region" /tmp/bigwin_build.log | head -10; exit 1
fi
grep -E "error:" /tmp/bigwin_build.log | head -5 || true
echo "built: $(ls -l build/bridge.bin | awk '{print $5}') bytes"

echo "[2/4] flash"
scp -q -o StrictHostKeyChecking=no -i $K build/bridge.bin root@10.119.0.199:$B/bridge.bin
ssh -o StrictHostKeyChecking=no -i $K root@10.119.0.199 "
    set -e
    PORT=\$(ls /dev/serial/by-id/*1a86* | head -1); cd $B
    esptool --chip esp32s3 -p \$PORT -b 460800 --before default-reset --after no-reset \
        write-flash 0x20000 bridge.bin 2>&1 | grep -cE 'Hash of data verified' | sed 's/^/flash=/'"

echo "[3/4] boot, take the two test files off the card"
ssh -o StrictHostKeyChecking=no -i $K root@10.119.0.199 "
    set -e
    sh $B/detach_test.sh 20 10 >/dev/null 2>&1 || true
    i=0; while [ ! -b /dev/sdb ] && [ \$i -lt 30 ]; do sleep 1; i=\$((i+1)); done
    mount /dev/sdb /mnt/card
    rm -f '/mnt/card/2222.mp3' '/mnt/card/panni-kelj-fel.mp3'
    sync; umount /mnt/card; sleep 2"

echo "[4/4] timed sync (32 KiB chunk / 64 KiB window / SACK / pool 48)"
ssh -o StrictHostKeyChecking=no -o ServerAliveInterval=30 -i $K root@10.119.0.199 "
    set -e
    sh $B/detach_test.sh 120 15 >/dev/null 2>&1 || true
    tr -d '\r' < /tmp/sync.log | grep -E 'get |chunk|\+ |copied|rename|FAILED' | head -8"
echo "BASELINE_TO_BEAT: 4.0 s and 3.8 s per 1 MiB"
echo "WINDOW_TEST_DONE"
