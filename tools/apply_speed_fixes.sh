#!/bin/sh
# Apply the two speed fixes that the stage-split measurement decides between, then measure.
#
# Why the earlier "32 KiB chunk is no better" conclusion is not trustworthy: with
# CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384, malloc(32768) returns PSRAM (proven by the heap log:
# internal free was 238 KiB with a 32 KiB chunk vs 230 KiB with an 8 KiB one - the bigger buffer
# took no internal RAM at all). On ESP32-S3 SOC_SDMMC_PSRAM_DMA_CAPABLE is NOT set, so a PSRAM
# buffer is not SDMMC-DMA-able and every card write from it is bounced through internal RAM.
#
# Fix 1 (always right): take the read buffer from internal DMA-capable RAM.
# Fix 2 (only if the radio is the limit): widen RX aggregation - CONFIG_ESP_WIFI_RX_BA_WIN is 6
#         while the AP offered 64, and IDF's own help asks for STATIC_RX_BUFFER_NUM >= RX_BA_WIN.
#         Cost ~42 KiB of internal DRAM.
#
# Usage: sh tools/apply_speed_fixes.sh [wifi]     (default: buffer fix only)
set -eu
cd /home/hermes/board-schematics/bridge
SRC=main/bridge_main.c
WIFI="${1:-}"

grep -q '^#define BRIDGE_DRY_RUN 0$' $SRC || { echo "tree is in dry-run state - not touching it"; exit 1; }

python3 - <<'PY'
p = "main/bridge_main.c"
s = open(p).read()
old = """    uint8_t *buf = malloc(chunk);"""
new = """    /* Internal, DMA-capable RAM on purpose. CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL is 16 KiB, so a
     * 32 KiB malloc() lands in PSRAM - and on ESP32-S3 the SDMMC path cannot DMA from PSRAM
     * (SOC_SDMMC_PSRAM_DMA_CAPABLE is unset for this chip), so every card write from that buffer
     * is bounced through internal RAM inside the very serial path we are trying to speed up. */
    uint8_t *buf = heap_caps_malloc(chunk, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);"""
assert s.count(old) == 1, "read-buffer allocation not found unchanged; refusing to patch"
open(p, "w").write(s.replace(old, new, 1))
print("read buffer -> internal DMA-capable RAM")
PY

if [ "$WIFI" = "wifi" ]; then
    for f in sdkconfig sdkconfig.defaults; do
        sed -i 's/^CONFIG_ESP_WIFI_RX_BA_WIN=6$/CONFIG_ESP_WIFI_RX_BA_WIN=32/; s/^CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=10$/CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=32/' $f
        grep -q "^CONFIG_ESP_WIFI_RX_BA_WIN=32$" $f || echo "note: $f has no RX_BA_WIN line"
    done
    echo "wifi: RX_BA_WIN 6->32, STATIC_RX_BUFFER_NUM 10->32"
fi

echo "--- building"
bash build.sh > /tmp/speed_build.log 2>&1 || { grep -E "error:" /tmp/speed_build.log | head -5; exit 1; }
grep -E "error:" /tmp/speed_build.log | head -3 || true
K=/home/hermes/.ssh/aithinker_flash_ed25519
scp -q -o StrictHostKeyChecking=no -i $K build/bridge.bin root@10.119.0.199:/root/bridge/bridge.bin
ssh -o StrictHostKeyChecking=no -i $K root@10.119.0.199 "
    set -e
    PORT=\$(ls /dev/serial/by-id/*1a86* | head -1); cd /root/bridge
    esptool --chip esp32s3 -p \$PORT -b 460800 --before default-reset --after no-reset \
        write-flash 0x20000 bridge.bin 2>&1 | grep -cE 'Hash of data verified' | sed 's/^/flash=/'
    sh /root/bridge/detach_test.sh 20 10 >/dev/null 2>&1 || true
    i=0; while [ ! -b /dev/sdb ] && [ \$i -lt 30 ]; do sleep 1; i=\$((i+1)); done
    mount /dev/sdb /mnt/card
    rm -f '/mnt/card/2222.mp3' '/mnt/card/panni-kelj-fel.mp3'
    sync; umount /mnt/card; sleep 2
    sh /root/bridge/detach_test.sh 120 15 >/dev/null 2>&1 || true
    tr -d '\r' < /tmp/sync.log | grep -E 'get |chunk|\+ |copied|FAILED' | head -8"
echo "BASELINE_TO_BEAT: 4.0 s / 3.8 s per 1 MiB (8 KiB chunk, PSRAM-free by luck)"
echo "SPEED_FIX_DONE"
