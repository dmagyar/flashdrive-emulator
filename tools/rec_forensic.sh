#!/bin/bash
# Read the recovered clusters and decide who wrote them: a ~40 MB MP3 payload means the BRIDGE was
# cut mid-download by the car's power loss; a small structured blob means the HEAD UNIT wrote to the
# card. The answer decides whether write-protecting the LUN is the whole fix or only half of it.
echo "=== LUN state after the handover ==="
for i in 1 2 3 4 5 6 7 8 9 10; do [ -b /dev/sdb ] && break; sleep 3; done
lsblk -o NAME,RO,SIZE,FSTYPE /dev/sdb 2>/dev/null | tail -1
dmesg | grep -aiE 'sdb.*write protect' | tail -1

if ! mount -o ro /dev/sdb /mnt/car; then
    echo "MOUNT FAILED - volume not available to the host yet"
    exit 1
fi

echo "=== recovered clusters ==="
ls -la /mnt/car/FSCK0000.REC /mnt/car/FSCK0001.REC /mnt/car/FSCK0002.REC /mnt/car/FSCK0003.REC 2>/dev/null

if command -v ffprobe >/dev/null 2>&1; then
    HAVE_FF=1
else
    HAVE_FF=0
fi

for f in /mnt/car/FSCK0000.REC /mnt/car/FSCK0001.REC /mnt/car/FSCK0002.REC /mnt/car/FSCK0003.REC; do
    [ -f "$f" ] || continue
    b=$(basename "$f")
    sz=$(stat -c %s "$f")
    printf '%s (%s B)\n' "$b" "$sz"
    echo "   first 8 bytes: $(head -c 8 "$f" | od -An -tx1)"
    if [ "$HAVE_FF" = 1 ]; then
        ffprobe -v error -show_entries format=format_name,duration,bit_rate "$f" 2>&1 | tr '\n' ' ' | sed 's/^/   ffprobe: /'
        echo
    fi
done

echo "=== do the clusters match the START of a track already on the card? ==="
cd /mnt/car || exit 1
for f in FSCK0000.REC FSCK0001.REC FSCK0002.REC FSCK0003.REC; do
    [ -f "$f" ] || continue
    h=$(head -c 262144 "$f" | md5sum | cut -c1-12)
    match=""
    for m in $(find . -name '*.mp3'); do
        hm=$(head -c 262144 "$m" | md5sum | cut -c1-12)
        [ "$h" = "$hm" ] && match="$m" && break
    done
    echo "   $f first-256 KiB $h -> ${match:-no track starts with these bytes}"
done

echo "=== card shape ==="
echo "mp3=$(find /mnt/car -name '*.mp3' | wc -l)  rec=$(find /mnt/car -name '*.REC' | wc -l)  part=$(find /mnt/car -name '*.part' | wc -l)"
find /mnt/car -maxdepth 1 -type d -name '20*' -printf '%f\n' | sort
df -h /mnt/car | awk 'NR==2{print "used="$3" free="$4}'
sync
umount /mnt/car && echo unmounted_ok
