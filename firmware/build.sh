#!/usr/bin/env bash
# Build (and flash) the glitchESP firmware on Linux or macOS, or in the GitHub workflow.
# The ESP-IDF v5.5.5 environment must be active (". $IDF_PATH/export.sh").
#
# Usage:  ./build.sh                       build, rev1_3 silicon profile
#         ./build.sh /dev/ttyUSB0          build and flash
#         ./build.sh "" rev3_x             another silicon profile
#
# Besides the usual images the build leaves build/<profile>/glitchesp-merged.bin: bootloader,
# partition table and application in one file, to be written at address 0 (a first install;
# it also wipes the settings and presets stored in flash).
set -euo pipefail
cd "$(dirname "$0")"
port="${1:-}"
profile="${2:-rev1_3}"
build="build/$profile"
args=(-B "$build" -D "SDKCONFIG=$PWD/$build/sdkconfig" -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.$profile")

idf.py "${args[@]}" build
(cd "$build" && python -m esptool --chip esp32p4 merge_bin -o glitchesp-merged.bin @flash_args)

if [ -n "$port" ]; then
    idf.py "${args[@]}" -p "$port" flash
fi
