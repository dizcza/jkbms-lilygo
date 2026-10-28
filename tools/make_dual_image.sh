#!/bin/bash
# Builds the normal firmware (build/) and the collect-first one (build-collect/,
# CONFIG_UPLOAD_COLLECT_FIRST) and merges both into build/jkbms-dual.bin, to be
# written over USB at 0x0: the normal firmware in ota_0 boots by default, the
# collect-first one waits in ota_1. Holding the right button for 5 s switches.
# Writing it erases the saved settings (NVS): the BMS is picked again.
#   . $IDF_PATH/export.sh && tools/make_dual_image.sh
set -e
cd "$(dirname "$0")/.."
idf.py build
idf.py -B build-collect -D SDKCONFIG=build-collect/sdkconfig \
       -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.collect" build
# app offsets from the partition table
offsets=$(python "$IDF_PATH/components/partition_table/gen_esp32part.py" build/partition_table/partition-table.bin \
          | awk -F, '$1=="ota_0"||$1=="ota_1" {print $4}')
ota0=$(echo "$offsets" | sed -n 1p)
ota1=$(echo "$offsets" | sed -n 2p)
esptool.py --chip esp32 merge_bin -o build/jkbms-dual.bin --flash_mode dio --flash_freq 80m --flash_size 16MB \
    0x1000 build/bootloader/bootloader.bin \
    0x8000 build/partition_table/partition-table.bin \
    0xf000 build/ota_data_initial.bin \
    "$ota0" build/jkbms.bin \
    "$ota1" build-collect/jkbms.bin
echo "build/jkbms-dual.bin: normal firmware at $ota0 (boots), collect-first at $ota1; write it at 0x0"
