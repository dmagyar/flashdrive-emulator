#!/bin/sh
# Rename-leg proof, run ON THE BENCH.
#
#   sh flatten_refile_proof.sh [log-name]
#
# The dated archive claims that a track it already holds is *filed*, not fetched: when the bytes
# are on the card under another name, the bridge renames them, which on FatFs rewrites a 32-byte
# directory entry instead of transferring 41 MB. That claim is worthless unless it is measured,
# and the only honest measurement is to destroy the folder layout from the other side and watch
# what comes back over the wire.
#
#   pass A  host mounts the card read-write, moves every dated file back to the card root,
#           deletes MUSIC/. The board now holds the same (size, mtime) identities at the wrong
#           paths.                                          <- the state the recorder creates
#   pass B  board's pass: must FILE all of them (rename) and copy NOTHING.
#   pass C  a second pass with nothing changed: must copy nothing and file nothing. Any churn
#           here means the identity test or the date comparison is wrong and every pass would
#           rewrite every directory entry.
#
# Asserted from the console log, then re-asserted against the mounted card - two independent
# witnesses, because a firmware that reports the wrong thing consistently passes a log-only test.
set -eu
LOG=${1:-refile.log}
PORT=$(ls /dev/serial/by-id/*1a86* | head -1)
# Resolve the card by VENDOR, never by "the first vfat". This bench has a 1.9 GB CHIPSBNK vfat
# stick next to the bridge's 14.8 GB TinyUSB volume, so a positional guess would have mounted -
# and then flattened - somebody else's disk. Exactly one match or nothing runs.
DEV=$(lsblk -rpno NAME,VENDOR 2>/dev/null | awk '$2=="TinyUSB"{print $1}')
[ "$(printf '%s\n' "$DEV" | grep -c .)" = "1" ] || { echo "FAIL: want exactly one TinyUSB volume, got [$DEV]"; exit 1; }
echo "card = $DEV"
AUTH=/sys/bus/usb/devices/1-1.5/authorized
MNT=/mnt/car
[ -n "$PORT" ] || { echo "no 1a86 console port"; exit 1; }

exec 9>/tmp/bridge.lock
flock -n 9 || { echo "REFUSING TO RUN: another bench harness holds the port."; exit 1; }

auth() {  # $1 = 0|1 ; retry, believe only the read-back (authorizing during re-enumeration
          # returns EPROTO and a lost write would leave the volume ownerless)
    i=0
    while [ $i -lt 12 ]; do
        echo "$1" > "$AUTH" 2>/dev/null || true
        sleep 2
        [ "$(cat "$AUTH" 2>/dev/null)" = "$1" ] && return 0
        i=$((i+1))
    done
    echo "could not set authorized=$1 (got '$(cat "$AUTH" 2>/dev/null)')"; return 1
}

# Whoever dies, the host ends up holding the volume - a board that keeps it leaves the next
# script staring at "Can't lookup blockdev" and blaming the wrong component.
trap 'auth 1 || true; umount "$MNT" 2>/dev/null || true' EXIT INT TERM

stty -F "$PORT" 115200 raw -echo
: > "/tmp/$LOG"
setsid nohup sh -c "timeout 2400 cat $PORT >> /tmp/$LOG" >/dev/null 2>&1 < /dev/null &
sleep 2
before=$(wc -l < "/tmp/$LOG")

say() { printf '\n=== %s ===\n' "$*"; }

say "A: host flattens the card (dated files -> root, MUSIC/ removed)"
auth 1
for i in $(seq 1 20); do [ -b "$DEV" ] && break; sleep 2; done
[ -b "$DEV" ] || { echo "FAIL: no block device after authorizing"; exit 1; }
sleep 3
mount -o remount,rw "$MNT" 2>/dev/null || mount -o rw "$DEV" "$MNT"
moved=$(cd "$MNT" && find MUSIC -maxdepth 1 -mindepth 2 -type f -name '*.mp3' | wc -l)
if [ "$moved" -lt 1 ]; then echo "FAIL: no dated files on the card to flatten"; exit 1; fi
( cd "$MNT" && find MUSIC -maxdepth 1 -mindepth 2 -type f -name '*.mp3' -exec mv -f {} . \; )
rm -rf "$MNT/MUSIC"
root=$(find "$MNT" -maxdepth 1 -type f -name '*.mp3' | wc -l)
echo "flattened $moved dated file(s); card root now holds $root file(s)"
md5sum "$MNT"/*.mp3 | sort > /tmp/refile_before.md5
sync; umount "$MNT"

say "B: board re-files by identity (rename expected, transfer must be zero)"
auth 0
i=0
while [ $i -lt 200 ]; do
    sleep 6
    tail -n +$((before+1)) "/tmp/$LOG" | tr -d '\r' | grep -aqE 'sync (done|FAILED)' && break
    i=$((i+1))
    [ $((i % 10)) -eq 0 ] && tail -n +$((before+1)) "/tmp/$LOG" | tr -d '\r' | tail -1
done
before=$(wc -l < "/tmp/$LOG")
B=$(tail -n +1 "/tmp/$LOG" | tr -d '\r')
echo "$B" | grep -aE 'filed |sync done|sync FAILED|archive:|prune ' | tail -20
filed=$(echo "$B" | grep -a 'filed by rename' | tail -1 | sed -n 's/.*; \([0-9][0-9]*\) filed by rename.*/\1/p')
copied=$(echo "$B" | grep -aE 'sync (done|FAILED):' | tail -1 | sed -n 's/.*: \([0-9][0-9]*\) copied.*/\1/p')
# The recorder rotates every 30 minutes, so a *new* chunk legitimately appears mid-test and may be
# fetched. What would falsify the identity test is re-fetching a file that was already on the card:
# compare downloaded basenames against the set we flattened, and fail only on that intersection.
# "I (tick) bridge: get //srv/share/07.mp3 (41000000 B)" -> field 5 is the path.
awk '/bridge: get /{p=$5; sub(/.*\//,"",p); print p}' "/tmp/$LOG" | sort -u > /tmp/refile_got.txt
awk '{p=$2; sub(/.*\//,"",p); print p}' /tmp/refile_before.md5 | sort -u > /tmp/refile_had.txt
refetch=$(comm -12 /tmp/refile_got.txt /tmp/refile_had.txt | tr '\n' ' ')
echo "PASS B: filed=$filed copied=${copied:-?} fetched=[ $(wc -l < /tmp/refile_got.txt) ] refetch_of_known=[ $refetch ]"
echo "        (want filed=$moved, and no known file re-fetched)"

say "C: second pass, nothing changed (must neither copy nor file)"
auth 1; sleep 6; auth 0
i=0
while [ $i -lt 200 ]; do
    sleep 6
    tail -n +$((before+1)) "/tmp/$LOG" | tr -d '\r' | grep -aqE 'sync (done|FAILED)' && break
    i=$((i+1))
done
C=$(tail -n +$((before+1)) "/tmp/$LOG" | tr -d '\r')
echo "$C" | grep -aE 'filed |sync done|sync FAILED' | tail -8
c_copied=$(echo "$C" | grep -aE 'sync (done|FAILED):' | tail -1 | sed -n 's/.*: \([0-9][0-9]*\) copied.*/\1/p')
c_filed=$(echo "$C" | grep -a 'filed by rename' | tail -1 | sed -n 's/.*; \([0-9][0-9]*\) filed by rename.*/\1/p')
c_filed=${c_filed:-0}
echo "PASS C: copied=${c_copied:-?} filed=$c_filed (want copied=0 filed=0)"

say "card contents, read back by the host"
auth 1
for i in $(seq 1 20); do [ -b "$DEV" ] && break; sleep 2; done
sleep 3
mount -o ro "$DEV" "$MNT"
find "$MNT" -name '*.mp3' | sed "s|$MNT/||" | sort | head -24
after=$(find "$MNT" -mindepth 2 -type f -name '*.mp3' | wc -l)
md5sum "$MNT"/MUSIC/*/*.mp3 2>/dev/null | awk '{n=$2; sub(/.*\//,"",n); print $1, n}' | sort > /tmp/refile_after.md5
sync; umount "$MNT"

say "verdict"
ok=1
[ "${filed:-0}" -eq "$moved" ] || { echo "FAIL: expected $moved filed, got ${filed:-0}"; ok=0; }
[ -z "$refetch" ] || { echo "FAIL: pass B re-downloaded a track already on the card: $refetch"; ok=0; }
[ "${c_copied:-x}" = "0" ] || { echo "FAIL: pass C re-copied (copied=$c_copied)"; ok=0; }
[ "${c_filed:-1}" = "0" ] || { echo "FAIL: pass C re-filed ($c_filed renames) - every pass would rewrite the FAT"; ok=0; }
[ "$after" -eq "$moved" ] || { echo "FAIL: card holds $after dated files, expected $moved"; ok=0; }
# Same bytes, different path: prove the rename moved content, not copies.
cut -d' ' -f1 /tmp/refile_before.md5 | sort > /tmp/a.sum
cut -d' ' -f1 /tmp/refile_after.md5  | sort > /tmp/b.sum
if diff -q /tmp/a.sum /tmp/b.sum >/dev/null; then
    echo "OK: md5 set identical before/after the filing pass - rename moved no data"
else
    echo "FAIL: md5 set changed across the filing pass"; diff /tmp/a.sum /tmp/b.sum | head; ok=0
fi
[ $ok -eq 1 ] && echo "REFILE_PROOF_OK" || echo "REFILE_PROOF_FAILED"
