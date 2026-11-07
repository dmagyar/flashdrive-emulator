#!/bin/bash
# What an uncontrolled power cut actually left behind. The console went down with the cable, so the
# card is the only witness: is the FAT still structurally sound, is the scratch at its reserved size,
# and what did the firmware last record about itself?
set -u
CARD=/dev/$(lsblk -d -no NAME,TRAN,VENDOR 2>/dev/null | awk '$2=="usb" && $3=="TinyUSB"{print $1; exit}')
[ -b "$CARD" ] || { echo "no TinyUSB card visible"; exit 1; }

echo "=== host view of the LUN ==="
lsblk -d -o NAME,RO,SIZE "$CARD" | tail -1
dmesg 2>/dev/null | grep -E "Write Protect|Mode Sense" | tail -2

mkdir -p /mnt/car
mount -o ro,flush "$CARD" /mnt/car || { echo MOUNT_FAILED - "the volume did not mount after the cut"; exit 1; }

echo "=== structural check (read-only, the bridge owns FAT state) ==="
if command -v fsck.fat >/dev/null; then
  fsck.fat -n "$CARD" 2>&1 | grep -E "^0x|files,|Free cluster|dirty|clean|Error|error" | head -6 | cut -c1-104
else
  echo "  fsck.fat absent"
fi

echo "=== scratch left by the cut ==="
find /mnt/car -name "*.part" -o -name "*.part.len" 2>/dev/null | while read -r F; do
  K=physical; case "$F" in *.len) K=committed;; esac
  printf "  %-10s %11d  %s\n" "$K" "$(stat -c %s "$F")" "$(basename "$F")"
done
[ -z "$(find /mnt/car -name "*.part" 2>/dev/null)" ] && echo "  none - the cut landed between files"

echo "=== library on the card ==="
for D in /mnt/car/2*/; do
  [ -d "$D" ] || continue
  printf "  %s: %s track(s), %s B\n" "$(basename "$D")" \
    "$(find "$D" -name '*.mp3' 2>/dev/null | wc -l)" \
    "$(find "$D" -name '*.mp3' -printf '%s\n' 2>/dev/null | awk '{s+=$1} END{print s+0}')"
done
echo "  recovered-cluster leftovers from the earlier event: $(find /mnt/car -name 'FSCK*.REC' 2>/dev/null | wc -l)"

echo "=== what the firmware last wrote about itself ==="
for S in /mnt/car/BRIDGE/*; do
  [ -f "$S" ] && { echo "  --- $(basename "$S") ---"; sed 's/\r//g' "$S" | head -12 | sed 's/^/    /' | cut -c1-100; }
done
umount /mnt/car 2>/dev/null || umount -l /mnt/car
echo DONE
