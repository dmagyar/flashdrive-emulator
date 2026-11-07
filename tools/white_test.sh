#!/bin/bash
# Prove the host-read detection that drives the white LED: flash the writable build, read the volume
# from the host, and show the burst lines and the counters that the LED pattern is built from.
set -u
DEV=/sys/bus/usb/devices/1-1.1.2
PORT=$(ls /dev/serial/by-id/*1a86* | head -1)
card() { lsblk -d -no NAME,TRAN,VENDOR 2>/dev/null | awk '$2=="usb" && $3=="TinyUSB"{print $1; exit}'; }

echo 1 > $DEV/authorized
esptool --chip esp32s3 -p "$PORT" --before default-reset --after no-reset \
        write_flash 0x20000 /root/bridge/bridge-rw.bin > /tmp/w-flash.log 2>&1
grep -q "Hash of data verified" /tmp/w-flash.log || { echo FLASH_FAILED; tail -3 /tmp/w-flash.log; exit 1; }
esptool --chip esp32s3 -p "$PORT" --after hard-reset flash-id > /dev/null 2>&1
sleep 3
stty -F "$PORT" 115200 raw -echo -crtscts
cat "$PORT" > /tmp/white.log 2>/dev/null &
CPID=$!
for i in $(seq 1 24); do sleep 5; [ -n "$(card)" ] && break; done
CARD=/dev/$(card)
[ -b "$CARD" ] || { echo "card never appeared"; kill -9 $CPID; exit 1; }
echo "=== LUN state ==="
lsblk -d -o NAME,RO,SIZE "$CARD" | tail -1
mkdir -p /mnt/car
mount -o ro,flush "$CARD" /mnt/car 2>/dev/null || mount -o flush "$CARD" /mnt/car || { echo MOUNT_FAILED; kill -9 $CPID; exit 1; }

echo "=== generating read traffic (the car scanning / playing looks like this) ==="
dd if="$CARD" of=/dev/null bs=1M count=48 2>/dev/null
sleep 3
find /mnt/car -name '*.mp3' -size +1M | head -2 | while read -r F; do dd if="$F" of=/dev/null bs=1M count=12 2>/dev/null; sleep 3; done
sleep 4
echo "=== burst lines the white LED is driven by ==="
grep "the consumer accepted the medium" /tmp/white.log | tr -d '\r' | head -5 | cut -c1-122
echo "=== counters ==="
grep -E "host_rd=" /tmp/white.log | tr -d '\r' | tail -2 | cut -c1-118
umount /mnt/car 2>/dev/null || umount -l /mnt/car
sleep 2
kill -9 $CPID 2>/dev/null
echo "=== board left on the WRITABLE build for the car test ==="
echo DONE
