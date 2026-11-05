#!/bin/sh
# Is ~500 KB/s the radio's real ceiling, or just what a 6-frame RX aggregation window allows?
#
# CONFIG_ESP_WIFI_RX_BA_WIN=6 means we accept at most ~6 aggregated frames per A-MPDU
# opportunity, while the AP's own session (log: winSize:64) offers 64. IDF's help asks for
# STATIC_RX_BUFFER_NUM >= RX_BA_WIN; on this chip static buffers are capped at 25, so the
# balanced maximum is 25/25 (it is not an HE chip, where the range goes to 64/128).
#
# Baseline to beat: network-only stage measured 482 / 554 KB/s (dry run, 32 KiB chunk).
# RAM price: static RX buffers 10 -> 25 is ~+24 KiB of internal DRAM.
#
# An earlier attempt at raising the RX buffers looked harmful - but that run still had the
# broken transport, so it proves nothing. This one runs on the fixed stack.
set -eu
cd /home/hermes/board-schematics/bridge
K=/home/hermes/.ssh/aithinker_flash_ed25519
B=/root/bridge

grep -q '^#define BRIDGE_DRY_RUN 0$' main/bridge_main.c || { echo "tree in dry-run state"; exit 1; }

for f in sdkconfig sdkconfig.defaults; do
    sed -i 's/^CONFIG_ESP_WIFI_RX_BA_WIN=6$/CONFIG_ESP_WIFI_RX_BA_WIN=25/;
            s/^CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=10$/CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=25/' $f
done
echo "now: $(grep -E 'RX_BA_WIN|STATIC_RX_BUFFER_NUM' sdkconfig | grep -v WIFI_RMT | tr '\n' ' ')"

bash build.sh > /tmp/ba_build.log 2>&1 || { grep -E "error:" /tmp/ba_build.log | head -5; exit 1; }
grep -E "error:" /tmp/ba_build.log | head -3 || true
scp -q -o StrictHostKeyChecking=no -i $K build/bridge.bin root@10.119.0.199:$B/bridge.bin
ssh -o StrictHostKeyChecking=no -o ServerAliveInterval=30 -i $K root@10.119.0.199 "
    set -e
    PORT=\$(ls /dev/serial/by-id/*1a86* | head -1); cd $B
    esptool --chip esp32s3 -p \$PORT -b 460800 --before default-reset --after no-reset \
        write-flash 0x20000 bridge.bin 2>&1 | grep -cE 'Hash of data verified' | sed 's/^/flash=/'
    sh $B/detach_test.sh 20 10 >/dev/null 2>&1 || true
    i=0; while [ ! -b /dev/sdb ] && [ \$i -lt 30 ]; do sleep 1; i=\$((i+1)); done
    mount /dev/sdb /mnt/card
    rm -f '/mnt/card/2222.mp3' '/mnt/card/panni-kelj-fel.mp3'
    sync; umount /mnt/card; sleep 2
    sh $B/detach_test.sh 120 15 >/dev/null 2>&1 || true
    tr -d '\r' < /tmp/sync.log | grep -E 'get |chunk|ba-add|\+ |copied|FAILED' | head -8"
echo "BASELINES: network-only 482/554 KB/s  |  read+write 376/371 KB/s"
echo "BA_WIN_TEST_DONE"
