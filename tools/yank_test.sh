#!/bin/bash
# Second half of the preallocation proof. Take the disk away mid-file (what the car does when it
# sleeps), read the scratch back off the card, then hand it back and prove the resume path trusts
# the committed-length sidecar rather than the file's own (preallocated) length.
set -u
DEV=/sys/bus/usb/devices/1-1.1.2
PORT=$(ls /dev/serial/by-id/*1a86* | head -1)

echo 1 > $DEV/authorized                     # host takes the disk mid-transfer
sleep 15
CARD=/dev/$(lsblk -no NAME,VENDOR 2>/dev/null | grep -i espress | awk '{print $1}' | head -1)
echo "=== card $CARD after the cut ==="
[ -b "$CARD" ] || { echo "no Espressif card visible"; exit 1; }
mkdir -p /mnt/car
mount -o ro,flush "$CARD" /mnt/car || echo MOUNT_FAILED
for F in $(find /mnt/car -name "*.part" -o -name "*.part.len" 2>/dev/null | head -10); do
  KIND=physical; case "$F" in *.len) KIND=committed;; esac
  printf "  %-11s %12d  %s\n" "$KIND" "$(stat -c %s "$F")" "$(basename "$F")"
done
umount /mnt/car 2>/dev/null || umount -l /mnt/car

echo "=== resume: disk back to the board, expecting it to continue mid-file ==="
sleep 2; echo 0 > $DEV/authorized
sleep 3
stty -F "$PORT" 115200 raw -echo -crtscts
cat "$PORT" > /tmp/resume.log 2>/dev/null &
CPID=$!
for i in $(seq 1 30); do sleep 5; grep -q "continuing" /tmp/resume.log && break; done
grep -m1 "continuing" /tmp/resume.log | tr -d '\r' | cut -c1-135 || echo "NO RESUME LINE"
sleep 6
kill -9 $CPID 2>/dev/null
echo "console bytes: $(wc -c < /tmp/resume.log)"
echo DONE
