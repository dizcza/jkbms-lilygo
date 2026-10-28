#!/bin/bash
# Publishes build/jkbms.bin on the dashboard (ssh host "vps"): boards with OTA on
# download it with their next status upload when it differs from what they run.
#   idf.py build && tools/deploy_firmware.sh
set -e
cd "$(dirname "$0")/.."
WEB_DIR=projects/jkbms-web
ssh vps "mkdir -p $WEB_DIR/firmware"
scp -q build/jkbms.bin "vps:$WEB_DIR/firmware/jkbms.bin.tmp"
# rename, so that a board never downloads a half-copied file
ssh vps "mv $WEB_DIR/firmware/jkbms.bin.tmp $WEB_DIR/firmware/jkbms.bin && cd $WEB_DIR && ./venv/bin/python manage.py firmware"
echo "local ELF: $(sha256sum build/jkbms.elf | cut -d' ' -f1)"
