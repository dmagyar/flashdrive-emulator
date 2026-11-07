#!/bin/bash
# Integrity of a file that was preallocated, cut at 32 MB and resumed across the seam.
# Reserved-but-never-written space is the failure mode to look for: a seek bug leaves a hole of
# zero bytes, which still renames to a plausible-looking track and plays as silence. So: no all-zero
# 64 KiB window anywhere, and ffmpeg must decode the whole thing without complaining.
set -u
DEV=/sys/bus/usb/devices/1-1.1.2
CARD=/dev/$(lsblk -d -no NAME,TRAN,VENDOR 2>/dev/null | awk '$2=="usb" && $3=="TinyUSB"{print $1; exit}')
[ -b "$CARD" ] || { echo "no TinyUSB card visible"; exit 1; }
mkdir -p /mnt/car
mount -o ro,flush "$CARD" /mnt/car || { echo MOUNT_FAILED; exit 1; }

echo "=== leftovers on the card ==="
echo "  .part files:  $(find /mnt/car -name '*.part' 2>/dev/null | wc -l)"
echo "  .part.len:    $(find /mnt/car -name '*.part.len' 2>/dev/null | wc -l)   (want 0 - the sidecar must go when the track is renamed)"
T=$(find /mnt/car -name "153929_10.mp3" 2>/dev/null | head -1)
[ -n "$T" ] || { echo "  the resumed track is not on the card yet"; umount /mnt/car; exit 1; }
echo "  finished track: $(stat -c %s "$T") B"

echo "=== zero-hole scan (a resume that wrote at the wrong offset leaves a zero run) ==="
python3 - "$T" <<'PY'
import sys
f, win, off, holes = open(sys.argv[1], "rb", 65536), 65536, 0, []
while True:
    b = f.read(win)
    if not b:
        break
    if not any(b):
        holes.append(off)
    off += len(b)
print("   %d zero-filled 64 KiB windows%s" % (len(holes), (" at " + str(holes[:6])) if holes else "  <- clean"))
PY

echo "=== full decode (ffmpeg reports any corrupt frame in the file) ==="
if command -v ffmpeg >/dev/null; then
  ffmpeg -v error -i "$T" -f null - 2>/tmp/ff.out; echo "  ffmpeg error lines: $(wc -l < /tmp/ff.out)"
  head -3 /tmp/ff.out | cut -c1-100
  ffprobe -v error -show_entries format=duration,bit_rate -of default=nw=1 "$T" | tr '\n' ' '; echo
else
  echo "  ffmpeg absent on this host"
fi
umount /mnt/car 2>/dev/null || umount -l /mnt/car
echo DONE
