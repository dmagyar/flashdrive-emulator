#!/bin/sh
# Runs readstall_probe on this machine against the NAS with the same vendored libsmb2 the
# firmware uses, so the only variable left is the platform. Usage: sh run_readstall.sh
set -e
SRC=/root/libsmb2-diag/libsmb2
cd /root/libsmb2-diag
export LD_LIBRARY_PATH=/root/libsmb2-diag/libsmb2/build/lib

gcc -O1 -g -I"$SRC/include" readstall_probe.c -o readstall \
    -L"$SRC/build/lib" -lsmb2 2>&1 | tail -5

SMBPASS=$(sed -n 's/^password *= *//p' /root/.smbcreds)
SMBUSER=$(sed -n 's/^username *= *//p' /root/.smbcreds)
export SMBPASS SMBUSER
[ -n "$SMBPASS" ] || { echo "no password parsed out of /root/.smbcreds" >&2; exit 1; }

echo "===== 8 KiB chunks (what the firmware now uses) ====="
./readstall 2222.mp3 8192 200 2 2>&1 | tail -30
echo
echo "===== 64 KiB chunks (what hung on the ESP) ====="
./readstall 2222.mp3 65536 40 2 2>&1 | tail -15
