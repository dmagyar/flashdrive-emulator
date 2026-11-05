#!/bin/sh
# How fast can this card actually take app writes? Never measured on this card: BOARD.md has
# 790 KiB/s write, but that was a different card (APPSD 120 MiB) with 256 KiB writes, and the
# stage-split arithmetic implies ~1.5 MB/s - the two cannot both be right, and the answer decides
# whether overlapping the read and write stages is worth a task and two queues.
#
# Needs no detach: the app owns the volume for a moment at boot, before handing it to USB, so a
# compile-time probe can write 8 MiB into the BRIDGE marker directory, report the rate, and unlink
# itself. Unbuffered FILE, because a buffered fwrite would measure a memcpy, not the card.
set -eu
cd /home/hermes/board-schematics/bridge
K=/home/hermes/.ssh/aithinker_flash_ed25519
SRC=main/bridge_main.c
grep -q '^#define BRIDGE_DRY_RUN 0$' $SRC || { echo "tree in dry-run state"; exit 1; }

python3 - <<'PY'
p = "main/bridge_main.c"
s = open(p).read()

if "BRIDGE_WRITE_ONLY" not in s:
    anchor = "#define BRIDGE_DRY_RUN 0\n"
    assert s.count(anchor) == 1, "dry-run define anchor moved"
    s = s.replace(anchor, anchor + """
/* Boot-time card throughput probe: writes 8 MiB into the marker directory, reports KiB/s and the
 * cost of flush+fsync, then removes the file. Compile-time only, like the dry run above, because
 * the answer decides whether the sync loop needs to overlap its two stages. */
#define BRIDGE_WRITE_ONLY 0
""", 1)

anchor2 = '    write_status_file(s_storage, "boot", 0);\n'
if "zz-speedtest" not in s:
    assert s.count(anchor2) == 1, "boot status-file anchor moved"
    s = s.replace(anchor2, anchor2 + """
#if BRIDGE_WRITE_ONLY
    {   /* self-cleaning: nothing is left on the volume the car will read */
        char wop[PATHMAX];
        snprintf(wop, sizeof(wop), "%s/BRIDGE", MSC_PATH);
        mkdir(wop, 0777);                 /* usually created by the status file; do not rely on it */
        snprintf(wop, sizeof(wop), "%s/BRIDGE/zz-speedtest.part", MSC_PATH);
        FILE *wf = fopen(wop, "wb");
        uint8_t *wb = wf ? heap_caps_malloc(READ_CHUNK_MAX, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA) : NULL;
        if (wf && wb) {
            setvbuf(wf, NULL, _IONBF, 0);       /* buffered fwrite would measure a memcpy */
            int64_t t0 = esp_timer_get_time();
            long long wo_total = 0;
            bool wo_bad = false;
            for (int i = 0; i < (8 * 1048576) / READ_CHUNK_MAX; i++)
                if (fwrite(wb, 1, READ_CHUNK_MAX, wf) != (size_t)READ_CHUNK_MAX) { wo_bad = true; break; }
                else wo_total += READ_CHUNK_MAX;
            int64_t t1 = esp_timer_get_time(), t2 = t1;
            if (!wo_bad) {
                if (fflush(wf) != 0 || fsync(fileno(wf)) != 0) wo_bad = true;
                t2 = esp_timer_get_time();
            }
            ESP_LOGI(TAG, "write-only probe: %lld B in %lld ms = %lld KiB/s%s; flush+fsync +%lld ms",
                     wo_total, (long long)((t1 - t0) / 1000),
                     (long long)(wo_total / 1024.0 / ((t1 - t0) / 1e6)),
                     wo_bad ? " THEN ERROR" : "", (long long)((t2 - t1) / 1000));
        } else {
            ESP_LOGE(TAG, "write-only probe: cannot open/write buffer (%s)", strerror(errno));
        }
        if (wf) fclose(wf);
        free(wb);
        unlink(wop);
    }
#endif
""", 1)
open(p, "w").write(s)
print("probe inserted")
PY

# turn the probe on, build, flash with a hard reset so the app boots straight away
sed -i 's/^#define BRIDGE_WRITE_ONLY 0$/#define BRIDGE_WRITE_ONLY 1/' $SRC
grep -q '^#define BRIDGE_WRITE_ONLY 1$' $SRC || { echo "probe macro not found"; exit 1; }
bash build.sh > /tmp/wo_build.log 2>&1 || { grep -E "error:" /tmp/wo_build.log | head -5; exit 1; }

scp -q -o StrictHostKeyChecking=no -i $K build/bridge.bin root@10.119.0.199:/root/bridge/bridge.bin
ssh -o StrictHostKeyChecking=no -o ServerAliveInterval=30 -i $K root@10.119.0.199 '
    set -e
    PORT=$(ls /dev/serial/by-id/*1a86* | head -1); cd /root/bridge
    esptool --chip esp32s3 -p $PORT -b 460800 --before default-reset --after hard-reset \
        write-flash 0x20000 bridge.bin 2>&1 | grep -cE "Hash of data verified" | sed "s/^/flash=/"
    sleep 5; stty -F $PORT 115200 raw -echo
    timeout 25 cat $PORT > /tmp/probe.log 2>/dev/null || true
    tr -d "\r" < /tmp/probe.log | grep -E "write-only|volume|Card|mount" | head -6'

# leave the tree and the board in the shipping state
sed -i 's/^#define BRIDGE_WRITE_ONLY 1$/#define BRIDGE_WRITE_ONLY 0/' $SRC
bash build.sh > /tmp/wo_build2.log 2>&1 || { grep -E "error:" /tmp/wo_build2.log | head -3; exit 1; }
scp -q -o StrictHostKeyChecking=no -i $K build/bridge.bin root@10.119.0.199:/root/bridge/bridge.bin
ssh -o StrictHostKeyChecking=no -i $K root@10.119.0.199 '
    set -e
    PORT=$(ls /dev/serial/by-id/*1a86* | head -1); cd /root/bridge
    esptool --chip esp32s3 -p $PORT -b 460800 --before default-reset --after no-reset \
        write-flash 0x20000 bridge.bin 2>&1 | grep -cE "Hash of data verified" | sed "s/^/restored=/"'
echo "CARD_PROBE_DONE  (BRIDGE_WRITE_ONLY back to 0: $(grep -c '^#define BRIDGE_WRITE_ONLY 0$' $SRC))"
