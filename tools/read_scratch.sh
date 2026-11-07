#!/bin/bash
# Read the scratch off the card after the mid-file handover, then hand the disk back and prove the
# resume path uses the committed-length sidecar, not the file's own (preallocated) length.
set -u
DEV=/sys/bus/usb/devices/1-1.1.2
PORT=$(ls /dev/serial/by-id/*1a86* | head -1)

# The bridge's LUN reports VENDOR=TinyUSB, MODEL="TEST MSC Storage". Match on the USB transport
# plus that vendor: the bench also carries a 14.8 GB mmcblk0, so size alone picks the wrong disk.
CARD=/dev/$(lsblk -d -no NAME,TRAN,VENDOR 2>/dev/null | awk '$2=="usb" && $3=="TinyUSB"{print $1; exit}')
echo "=== card $CARD, held by the host after the mid-file cut ==="
[ -b "$CARD" ] || { echo "no TinyUSB card visible"; exit 1; }
mkdir -p /mnt/car
mount -o ro,flush "$CARD" /mnt/car || echo MOUNT_FAILED
for F in $(find /mnt/car -name "*.part" -o -name "*.part.len" 2>/dev/null | head -10); do
  KIND=physical; case "$F" in *.len) KIND=committed;; esac
  printf "  %-11s %12d  %s\n" "$KIND" "$(stat -c %s "$F")" "$(basename "$F")"
done
umount /mnt/car 2>/dev/null || umount -l /mnt/car

echo "=== resume: disk back to the board ==="
sleep 2; echo 0 > $DEV/authorized
sleep 3
stty -F "$PORT" 115200 raw -echo -crtscts
cat "$PORT" > /tmp/resume.log 2>/dev/null &
CPID=$!
for i in $(seq 1 30); do sleep 5; grep -q "continuing" /tmp/resume.log && break; done
grep -m1 "continuing" /tmp/resume.log | tr -d '\r' | cut -c1-135 || echo "NO RESUME LINE"
sleep 6; kill -9 $CPID 2>/dev/null
echo "console bytes: $(wc -c < /tmp/resume.log)"
echo DONE
