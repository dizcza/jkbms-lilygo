#!/bin/bash
# Restarts the dashboard, e.g. after changing .env or deploying new code.
cd "$(dirname "$0")"
if [ -f gunicorn.pid ]; then
    kill "$(cat gunicorn.pid)"
    while [ -f gunicorn.pid ]; do sleep 0.5; done
fi
screen -dmS jkbms bash "$PWD/start.sh"
