#!/bin/bash
# Wait for the current sync pass to end, then read the recovered clusters from the card and say
# whether they look like our own MP3 downloads (the bridge writing when the car cut power) or like
# a head unit's index database.
P=$(ls /dev/serial/by-id/*1a86* | head -1)
stty -F "$P" 115200 raw -echo
: > /tmp/fin.log
timeout 900 cat "$P" > /tmp/fin.log 2>/dev/null &
CAT=$!

i=0
while [ $i -lt 860 ]; do
    grep -aqE 'sync done:|sync yielded|sync FAILED:' /tmp/fin.log && break
    sleep 5
    i=$((i + 5))
done
sleep 8
kill $CAT 2>/dev/null

echo "=== pass verdict ==="
tr -d '\r' < /tmp/fin.log | grep -aE 'sync done|sync yielded|sync FAILED|filed by rename|dates:|owner=' | tail -6

echo "=== forensic: recovered cluster contents ==="
for t in 1 2 3 4 5 6 7 8; do
    [ -b /dev/sdb ] && break
    sleep 5
done
if mount -o ro /dev/sdb /mnt/car; then
    ls -la /mnt/car/FSCK0000.REC 2>/dev/null || echo "(no .REC files left)"
    for f in /mnt/car/FSCK0000.REC /mnt/car/FSCK0001.REC /mnt/car/FSCK0002.REC /mnt/car/FSCK0003.REC; do
        [ -f "$f" ] || continue
        sz=$(stat -c %s "$f")
        printf '%s %s B  first bytes: ' "$(basename "$f")" "$sz"
        head -c 8 "$f" | od -An -tx1 | head -1
        n=$(grep -ac ID3 "$f" 2>/dev/null)
        echo "   ID3 markers: $n"
    done
    echo "mp3=$(find /mnt/car -name '*.mp3' | wc -l)  rec=$(find /mnt/car -name '*.REC' | wc -l)"
    find /mnt/car -maxdepth 1 -type d -name '20*' -printf '%f\n' | sort
    sync
    umount /mnt/car
else
    echo "(volume not handed over to the host yet)"
fi
