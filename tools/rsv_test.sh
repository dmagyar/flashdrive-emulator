#!/bin/bash
# Prove the preallocation runs: flash, capture the console, give the board the volume, and show
# every line about reserving / resuming. Under the terminal's foreground ceiling on purpose.
set -u
DEV=/sys/bus/usb/devices/1-1.1.2
PORT=$(ls /dev/serial/by-id/*1a86* | head -1)

esptool --chip esp32s3 -p "$PORT" --before default-reset --after no-reset \
        write_flash 0x20000 /root/bridge/bridge-ro-rsv.bin > /tmp/flash6.out 2>&1
grep -q "Hash of data verified" /tmp/flash6.out || { tail -4 /tmp/flash6.out; echo FLASH_FAILED; exit 1; }
esptool --chip esp32s3 -p "$PORT" --after hard-reset flash-id > /dev/null 2>&1
sleep 3
stty -F "$PORT" 115200 raw -echo -crtscts
cat "$PORT" > /tmp/rsv.log 2>/dev/null &
CPID=$!
sleep 5
echo 0 > $DEV/authorized                       # board gets the volume, a pass starts
sleep 200
kill -9 $CPID 2>/dev/null
echo "console bytes: $(wc -c < /tmp/rsv.log)"
echo "=== reservation / resume lines ==="
grep -E "reserved|continuing" /tmp/rsv.log | tr -d '\r' | head -6 | cut -c1-126
echo "=== pass state ==="
grep -E "sync (done|yielded|FAILED)|copied|MB " /tmp/rsv.log | tr -d '\r' | tail -4 | cut -c1-110
echo DONE
