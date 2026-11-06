#!/bin/sh
# Build every arm of the RX_BA_WIN A/B, with the resolved value ASSERTED before an image is kept.
#
#   sh tools/ba_win_build.sh            # -> dist/bridge-base.bin, dist/bridge-ba20.bin,
#                                       #    dist/bridge-ba25buf25.bin
#
# Why all three are built the same way. `idf.py build` honours the existing `sdkconfig`, and
# `sdkconfig.defaults` only fills keys that file does NOT already contain - demonstrated, not
# guessed: an overlay setting RX_BA_WIN=20 next to a saved sdkconfig with RX_BA_WIN=6 produced a
# firmware with 6 in it. So an overlay file is not a lever. `idf.py set-target` is: it deletes
# sdkconfig and regenerates it from sdkconfig.defaults, which is the one path in this project that
# is known to apply a knob. Building the baseline through that same path costs a rebuild and buys
# the control: the three images differ by exactly the lines added below and nothing else.
#
# What each arm is, and the compile-time rule that decides what is even legal
# (esp_wifi/src/wifi_init.c #errors, so an illegal pair is not a slow firmware, it is no firmware):
#
#   RX_BA_WIN <= DYNAMIC_RX_BUFFER_NUM (32 here)
#   RX_BA_WIN <= 2 * STATIC_RX_BUFFER_NUM (10 here -> 20)
#
#   base          BA 6  / static 10   shipping configuration
#   ba20          BA 20 / static 10   the knob alone, costs nothing - the only single-variable arm
#   ba25buf25     BA 25 / static 25   IDF's recommended pairing; ~+24 KiB internal DRAM, and the
#                                     buffer bump alone already measured +2% and was reverted
set -eu
cd "$(dirname "$0")/.."
. /home/hermes/esp/esp-idf/export.sh >/dev/null 2>&1
DEF=sdkconfig.defaults
mkdir -p dist

cp "$DEF" /tmp/sdkconfig.defaults.bak
restore() { cp /tmp/sdkconfig.defaults.bak "$DEF"; }
trap restore EXIT

build_arm() {                          # build_arm NAME WANT_BA WANT_STATIC [extra defaults lines...]
    name=$1; want_ba=$2; want_static=$3; shift 3
    restore
    if [ $# -gt 0 ]; then
        printf '%s\n' "$@" >> "$DEF"
    fi
    echo "== arm $name: configuring from scratch"
    idf.py set-target esp32s3 >/dev/null 2>&1 || { echo "FAIL: set-target"; exit 1; }
    if ! idf.py build > /tmp/ba_build_$name.log 2>&1; then
        echo "FAIL: build for arm $name - see /tmp/ba_build_$name.log"
        grep -E 'error:' /tmp/ba_build_$name.log | head -5
        return 1
    fi
    ba=$(sed -n 's/^CONFIG_ESP_WIFI_RX_BA_WIN=//p' sdkconfig)
    st=$(sed -n 's/^CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=//p' sdkconfig)
    echo "   resolved: RX_BA_WIN=$ba STATIC_RX_BUFFER_NUM=$st"
    # The baseline gets asserted too. An arm whose control is not known to be 6/10 cannot be
    # compared against anything, and "the numbers came out the same" is worthless if both firmware
    # images were the same configuration.
    [ "$ba" = "$want_ba" ] && [ "$st" = "$want_static" ] || {
        echo "FAIL: arm $name resolved to BA=$ba/static=$st, wanted $want_ba/$want_static - refusing to ship it"; return 1; }
    cp build/bridge.bin dist/bridge-$name.bin
    echo "   image: dist/bridge-$name.bin  $(stat -c %s dist/bridge-$name.bin) B  sha256:$(sha256sum dist/bridge-$name.bin | cut -c1-16)"
}

# The arm list. Base injects nothing: RX_BA_WIN then falls to its Kconfig default of 6, which the
# assertion above confirms rather than assumes.
build_arm base       6  10
build_arm ba20       20 10  CONFIG_ESP_WIFI_RX_BA_WIN=20
build_arm ba25buf25  25 25  CONFIG_ESP_WIFI_RX_BA_WIN=25 CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=25

# Leave the tree as the shipping configuration, so a later plain `idf.py build` cannot silently
# ship the last arm that was built.
restore
idf.py set-target esp32s3 >/dev/null 2>&1 || true
echo "tree config restored: $(sed -n 's/^CONFIG_ESP_WIFI_RX_BA_WIN=/RX_BA_WIN=/p' sdkconfig) $(sed -n 's/^CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=/STATIC_RX_BUFFER_NUM=/p' sdkconfig)"

echo
echo "arms ready in dist/ - run one per arm with:"
echo "  sh tools/ba_win_matrix.sh base       dist/bridge-base.bin    2 '/01.mp3 /02.mp3 /03.mp3'"
echo "  sh tools/ba_win_matrix.sh ba20       dist/bridge-ba20.bin    2 '/01.mp3 /02.mp3 /03.mp3'"
echo "  sh tools/ba_win_matrix.sh ba25buf25  dist/bridge-ba25buf25.bin 2 '/01.mp3 /02.mp3 /03.mp3'"
