#!/bin/sh
# Compare the recorder's own media dir with what the SMB share shows, to see whether the
# mirror into CAR-MEDIA preserves mtime. Run on the bench.
smbclient //10.119.0.4/CAR-MEDIA -A /root/.smbcreds -c 'ls' 2>/dev/null | grep -i '\.mp3'
