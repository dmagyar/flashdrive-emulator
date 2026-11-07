#!/bin/sh
# Where does the ~300 KB/s actually go? The sync is serial: read a chunk from Wi-Fi, THEN write
# it to a 1-bit SD bus (BOARD.md: D1/D2/D3 not routed, measured 790 KiB/s write on that bus).
# If the two stages are comparable, the serial sum lands near what we measure and the fix is to
# overlap them, not to enlarge chunks.
#
# Phase A: BRIDGE_DRY_RUN=1 -> reads the whole file, writes nothing  = the network stage alone.
# Phase B: back to 0       -> read + write, same files, same session = the serial sum.
# Network rate from A; card stage follows from 1/R_sum = 1/R_net + 1/R_card.
#
# Same two 1 MiB files each phase, and the tree ends in the shipping state (dry run off).
set -eu
cd /home/hermes/board-schematics/bridge
K=/home/hermes/.ssh/aithinker_flash_ed25519
B=/root/bridge
SRC=main/bridge_main.c

timed_run() {                              # $1 = label
    ssh -o StrictHostKeyChecking=no -o ServerAliveInterval=30 -i $K root@10.119.0.199 "
        set -e
        sh $B/detach_test.sh 120 15 >/dev/null 2>&1 || true
        echo '--- $1'
        tr -d '\r' < /tmp/sync.log | grep -E 'get |chunk|dry run|\+ |copied|FAILED' | head -8"
}

wipe_two() {
    ssh -o StrictHostKeyChecking=no -i $K root@10.119.0.199 "
        set -e
        sh $B/detach_test.sh 20 10 >/dev/null 2>&1 || true
        i=0; while [ ! -b /dev/sdb ] && [ \$i -lt 30 ]; do sleep 1; i=\$((i+1)); done
        mount /dev/sdb /mnt/card
        rm -f '/mnt/card/2222.mp3' '/mnt/card/panni-kelj-fel.mp3'
        sync; umount /mnt/card; sleep 2"
}

flash() {
    scp -q -o StrictHostKeyChecking=no -i $K build/bridge.bin root@10.119.0.199:$B/bridge.bin
    ssh -o StrictHostKeyChecking=no -i $K root@10.119.0.199 "
        set -e
        PORT=\$(ls /dev/serial/by-id/*1a86* | head -1); cd $B
        esptool --chip esp32s3 -p \$PORT -b 460800 --before default-reset --after no-reset \
            write-flash 0x20000 bridge.bin 2>&1 | grep -cE 'Hash of data verified' | sed 's/^/flash=/'"
}

build() {                                  # $1 = expected dry-run value, for the assertion
    grep -q "^#define BRIDGE_DRY_RUN $1\$" $SRC || { echo "dry-run flag not $1"; exit 1; }
    bash build.sh > /tmp/split_build.log 2>&1 || { grep -E "error:" /tmp/split_build.log | head -5; exit 1; }
}

echo "=== PHASE A: network only (no card writes)"
sed -i 's/^#define BRIDGE_DRY_RUN 0$/#define BRIDGE_DRY_RUN 1/' $SRC
build 1; flash; wipe_two; timed_run "A network-only"

echo "=== PHASE B: read + write (shipping path)"
sed -i 's/^#define BRIDGE_DRY_RUN 1$/#define BRIDGE_DRY_RUN 0/' $SRC
build 0; flash; wipe_two; timed_run "B read+write"

echo "SPLIT_DONE  (tree left with BRIDGE_DRY_RUN 0: $(grep -c '^#define BRIDGE_DRY_RUN 0$' $SRC) match)"
