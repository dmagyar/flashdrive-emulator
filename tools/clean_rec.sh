#!/bin/bash
# Remove the FSCKnnnn.REC files dosfstools left behind after the earlier FAT inconsistency. They are
# 53 MB of orphaned clusters, not music, and the head unit lists them. Deleting them needs a LUN the
# host may write, so: flash the writable variant, delete from the host while the board is not the FAT
# driver, then restore the read-only car build and prove both the removal and the write-protect.
set -u
DEV=/sys/bus/usb/devices/1-1.1.2
PORT=$(ls /dev/serial/by-id/*1a86* | head -1)
card() { lsblk -d -no NAME,TRAN,VENDOR 2>/dev/null | awk '$2=="usb" && $3=="TinyUSB"{print $1; exit}'; }
flash() {
  esptool --chip esp32s3 -p "$PORT" --before default-reset --after no-reset \
          write_flash 0x20000 "$1" > /tmp/cl-flash.log 2>&1
  grep -q "Hash of data verified" /tmp/cl-flash.log || { echo "FLASH FAILED: $1"; tail -3 /tmp/cl-flash.log; return 1; }
  esptool --chip esp32s3 -p "$PORT" --after hard-reset flash-id > /dev/null 2>&1
}
wait_card() { for i in $(seq 1 24); do sleep 5; [ -n "$(card)" ] && return 0; done; return 1; }

echo "=== 1. writable variant ==="
echo 1 > $DEV/authorized
flash /root/bridge/bridge-rw.bin || exit 1
wait_card || { echo "no card appeared"; exit 1; }
sleep 5
CARD=/dev/$(card); echo "  $CARD  RO=$(lsblk -d -no RO "$CARD")   (want 0)"
[ "$(lsblk -d -no RO "$CARD")" = "0" ] || { echo "LUN still read-only - refusing to continue"; exit 1; }

echo "=== 2. delete the recovered-cluster files ==="
mkdir -p /mnt/car
mount -o flush "$CARD" /mnt/car || { echo MOUNT_FAILED; exit 1; }
BEFORE=$(find /mnt/car -name 'FSCK*.REC' | wc -l)
find /mnt/car -name 'FSCK*.REC' -print -delete
sync
AFTER=$(find /mnt/car -name 'FSCK*.REC' | wc -l)
echo "  FSCK*.REC: $BEFORE -> $AFTER"
echo "  tracks still on the card: $(find /mnt/car -name '*.mp3' | wc -l)"
umount /mnt/car || umount -l /mnt/car
sleep 2

echo "=== 3. restore the read-only car build ==="
echo 1 > $DEV/authorized
flash /root/bridge/bridge-ro-car.bin || exit 1
wait_card || { echo "no card appeared after the car build"; exit 1; }
sleep 5
CARD=/dev/$(card)
echo "  $CARD  RO=$(lsblk -d -no RO "$CARD")   (want 1)"
lsblk -d -o NAME,RO,SIZE "$CARD" | tail -1
mkdir -p /mnt/car
mount -o ro,flush "$CARD" /mnt/car || { echo MOUNT_FAILED; exit 1; }
echo "  after: FSCK*.REC=$(find /mnt/car -name 'FSCK*.REC' | wc -l)  mp3=$(find /mnt/car -name '*.mp3' | wc -l)  scratch=$(find /mnt/car -name '*.part*' | wc -l)"
umount /mnt/car 2>/dev/null || umount -l /mnt/car
command -v fsck.fat >/dev/null && { echo "  fat: $(fsck.fat -n "$CARD" 2>&1 | grep -E 'files,|dirty|clean' | head -1)"; }
echo DONE
