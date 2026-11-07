#!/bin/sh
# Build negprobe against the vendored libsmb2 and run the dialect matrix against the NAS.
# Credentials stay in /root/.smbcreds (600) — passed via env, never argv, never printed.
SRC=/root/libsmb2-diag/libsmb2
cd /root/libsmb2-diag || exit 1
gcc -O2 -I "$SRC/include" -o negprobe negprobe.c "$SRC/build/lib/libsmb2.so" \
    -Wl,-rpath,"$SRC/build/lib" || { echo "BUILD FAILED"; exit 1; }
SMBPASS=$(sed -n 's/^password *= *//p' /root/.smbcreds)
export SMBPASS
for v in 2.0.2 2.1 3.0 3.0.2 3.1.1 any any2 any3; do
    out=$(timeout 40 ./negprobe 10.119.0.4 CAR-MEDIA "$v" car-media-bridge 2>&1)
    printf '[%-6s] %s\n' "$v" "$(echo "$out" | grep -E 'CONNECT|entries|opendir' | tr '\n' ' ')"
done
