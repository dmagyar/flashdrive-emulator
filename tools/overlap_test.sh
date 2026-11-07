#!/bin/sh
# Overlap the two stages and measure. Card stage measured 1430 KiB/s (0.70 ms/KiB), network stage
# 482-554 KB/s (2.0 ms/KiB), and the loop pays both in series - so this should move the whole sync
# toward the network rate. Same harness, same two files, same link as every run before it.
#
# Also drops CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM back to 10: at 25 it bought +2% and 24 KiB of the
# internal DRAM these slots need. CONFIG_ESP_WIFI_RX_BA_WIN was never actually tested - build.sh
# runs `idf.py set-target`, which regenerates sdkconfig from sdkconfig.defaults, so a knob that is
# not in defaults does not exist. It is not in defaults, so every run so far used 6.
set -eu
cd /home/hermes/board-schematics/bridge
K=/home/hermes/.ssh/aithinker_flash_ed25519
grep -q '^#define BRIDGE_DRY_RUN 0$' main/bridge_main.c || { echo "tree in dry-run state"; exit 1; }

sed -i 's/^CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=25$/CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=10/' sdkconfig.defaults
grep -n "STATIC_RX_BUFFER_NUM" sdkconfig.defaults

python3 tools/apply_overlap.py
bash build.sh > /tmp/ov_build.log 2>&1 || { grep -E "error:" /tmp/ov_build.log | head -8; exit 1; }
grep -cE "error:" /tmp/ov_build.log | sed 's/^/errors=/'

# NB the bracket: `pkill -f detach_test` also matches THIS remote shell (its own argv contains the
# literal), so it kills itself, ssh returns non-zero and `set -e` aborts the run. The character
# class matches the harness but not the killer's own command line.
ssh -o StrictHostKeyChecking=no -o ServerAliveInterval=30 -i $K root@10.119.0.199 'pkill -f "detach_[t]est" 2>/dev/null; rm -f /tmp/bridge_boot_wait /tmp/sync.log; true'
scp -q -o StrictHostKeyChecking=no -i $K build/bridge.bin root@10.119.0.199:/root/bridge/bridge.bin
LOCAL=$(sha256sum build/bridge.bin | cut -c1-16)
REMOTE=$(ssh -o StrictHostKeyChecking=no -i $K root@10.119.0.199 'sha256sum /root/bridge/bridge.bin | cut -c1-16')
[ "$LOCAL" = "$REMOTE" ] && echo "binary verified $LOCAL" || { echo "SCP STALE $LOCAL != $REMOTE"; exit 1; }

ssh -o StrictHostKeyChecking=no -o ServerAliveInterval=30 -i $K root@10.119.0.199 '
    set -e
    PORT=$(ls /dev/serial/by-id/*1a86* | head -1); cd /root/bridge
    esptool --chip esp32s3 -p $PORT -b 460800 --before default-reset --after no-reset \
        write-flash 0x20000 bridge.bin 2>&1 | grep -cE "Hash of data verified" | sed "s/^/flash=/"
    # The card has to be missing something or the sync does nothing at all - a run measured 0 KB/s
    # and reported "0 copied, 4 already present". Boot the app so the host can see the volume, then
    # delete the two small test files from the host side; the detach below makes the board notice
    # and re-download exactly those. NB: no apostrophes inside this single-quoted remote block - one
    # closes the quote and the remainder of the script runs on the local shell instead.
    esptool --chip esp32s3 -p $PORT --before default-reset --after hard-reset flash-id >/dev/null 2>&1 || true
    sleep 12
    i=0; while [ ! -b /dev/sdb ] && [ $i -lt 30 ]; do sleep 1; i=$((i+1)); done
    [ -b /dev/sdb ] || { echo "no /dev/sdb - the host never got the volume"; exit 1; }
    mount /dev/sdb /mnt/card
    echo "card before: $(ls /mnt/card | wc -l) entries"
    rm -f /mnt/card/2222.mp3 /mnt/card/panni-kelj-fel.mp3   # unquoted: inner quotes would close this single-quoted block
    sync; umount /mnt/card; sleep 2
    echo "wiped the two test files"
    nohup sh ./detach_test.sh 170 > /tmp/detach_run.log 2>&1 & echo "harness started"
    sleep 160
    tr -d "\r" < /tmp/sync.log | grep -E "get |chunk|first pread|  \+ |sync done|stalled|new connection|Guru|abort" | head -16'
echo "OVERLAP_TEST_DONE"
echo "BASELINES: network-only 482/554 KB/s  |  read+write 376/371 KB/s  |  card 1430 KiB/s"
