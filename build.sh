#!/usr/bin/env bash
# Build the bridge firmware. idf.py is not on a bare shell's PATH, so source the export.
set -u
source /home/hermes/esp/esp-idf/export.sh >/dev/null 2>&1
cd "$(dirname "$0")"
idf.py set-target esp32s3 >/dev/null 2>&1
idf.py build 2>&1
echo "BUILD_EXIT=$?"
