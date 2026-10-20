#!/bin/bash
# Starts the JK BMS dashboard. Launched at boot from crontab:
#   @reboot screen -dmS jkbms bash /home/vps/projects/jkbms-web/start.sh
# Secrets (JKBMS_API_TOKEN, JKBMS_WEB_USER, JKBMS_WEB_PASSWORD, SECRET_KEY) are read from .env.
cd "$(dirname "$0")"
set -a
source .env
set +a
exec ./venv/bin/gunicorn --workers 1 --threads 4 --bind 0.0.0.0:8099 --pid gunicorn.pid app:app
