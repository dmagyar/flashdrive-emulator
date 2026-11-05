#!/bin/sh
# RX_BA_WIN 6 -> 25, done properly this time.
#
# The first attempt measured nothing: it sed'd `CONFIG_ESP_WIFI_RX_BA_WIN=6` in sdkconfig, which
# build.sh's `idf.py set-target` then deleted and regenerated from sdkconfig.defaults - where the
# line did not exist. Every run so far used the Kconfig default of 6, and the "+2%" I attributed to
# the BA window was really just STATIC_RX_BUFFER_NUM 10 -> 25 (reverted; not worth 24 KiB).
#
# So: put the knob in sdkconfig.defaults, BUILD, and then assert the resolved sdkconfig before
# believing a single throughput number. If the value did not change, this script fails instead of
# reporting a fake null result.
set -eu
cd /home/hermes/board-schematics/bridge
K=/home/hermes/.ssh/aithinker_flash_ed25519
grep -q '^#define BRIDGE_DRY_RUN 0$' main/bridge_main.c || { echo "tree in dry-run state"; exit 1; }

if grep -q '^CONFIG_ESP_WIFI_RX_BA_WIN=' sdkconfig.defaults; then
    sed -i 's/^CONFIG_ESP_WIFI_RX_BA_WIN=.*/CONFIG_ESP_WIFI_RX_BA_WIN=25/' sdkconfig.defaults
else
    printf 'CONFIG_ESP_WIFI_RX_BA_WIN=25\n' >> sdkconfig.defaults
fi
grep -n "RX_BA_WIN" sdkconfig.defaults

bash build.sh > /tmp/ba2_build.log 2>&1 || { grep -E "error:" /tmp/ba2_build.log | head -6; exit 1; }
RESOLVED=$(grep '^CONFIG_ESP_WIFI_RX_BA_WIN=' sdkconfig || echo "MISSING")
echo "resolved: $RESOLVED"
[ "$RESOLVED" = "CONFIG_ESP_WIFI_RX_BA_WIN=25" ] || { echo "KNOB DID NOT TAKE EFFECT - not running a fake test"; exit 1; }

ssh -o StrictHostKeyChecking=no -i $K root@10.119.0.199 'pkill -f "detach_[t]est" 2>/dev/null; rm -f /tmp/bridge_boot_wait /tmp/sync.log; true'
scp -q -o StrictHostKeyChecking=no -i $K build/bridge.bin root@10.119.0.199:/root/bridge/bridge.bin
LOCAL=$(sha256sum build/bridge.bin | cut -c1-16)
REMOTE=$(ssh -o StrictHostKeyChecking=no -i $K root@10.119.0.199 'sha256sum /root/bridge/bridge.bin | cut -c1-16')
[ "$LOCAL" = "$REMOTE" ] && echo "binary verified $LOCAL" || { echo "SCP STALE"; exit 1; }

ssh -o StrictHostKeyChecking=no -o ServerAliveInterval=30 -i $K root@10.119.0.199 '
    set -e
    PORT=$(ls /dev/serial/by-id/*1a86* | head -1); cd /root/bridge
    esptool --chip esp32s3 -p $PORT -b 460800 --before default-reset --after no-reset \
        write-flash 0x20000 bridge.bin 2>&1 | grep -cE "Hash of data verified" | sed "s/^/flash=/"
    nohup sh ./detach_test.sh 170 > /tmp/detach_run.log 2>&1 & echo "harness started"
    sleep 160
    tr -d "\r" < /tmp/sync.log | grep -E "ba-add|get |chunk|  \+ |sync done|stalled|new connection" | head -14'
echo "BA2_DONE"
echo "BASELINE with overlap: see overlap_test output | network-only 482/554 KB/s | card 1430 KiB/s"
