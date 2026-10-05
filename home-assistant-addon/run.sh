#!/usr/bin/env bash

set -euo pipefail

SOURCE="/opt/mammotion-rc/web-server"
APP="/data/mammotion-rc"
OPTIONS="/data/options.json"

echo "=========================================="
echo " Mammotion Remote - Home Assistant"
echo "=========================================="

mkdir -p "$APP"

echo "[1/4] Updating mammotion-rc files..."
cp -a "$SOURCE/." "$APP/"

cd "$APP"

echo "[2/4] Checking HTTPS certificate..."

if [ ! -f cert.pem ] || [ ! -f key.pem ]; then
    echo "Generating HTTPS certificate..."
    python gen_cert.py \
        --cert cert.pem \
        --key key.pem \
        --host localhost
fi

echo "[3/4] Checking LoRa bridge..."

LORA_SERIAL=""
if [ -f "$OPTIONS" ]; then
    LORA_SERIAL="$(python -c 'import json,sys; print(json.load(open(sys.argv[1])).get("lora_serial") or "")' "$OPTIONS")"
fi

if [ -n "$LORA_SERIAL" ]; then
    # The bridge reopens the port itself if the board is unplugged; this loop
    # only restarts it if the process exits.  LoRa mowers in mowers.toml use
    # hc33_host = "127.0.0.1", link = "lora".
    echo "Starting LoRa bridge on $LORA_SERIAL (127.0.0.1:9876)"
    (
        while true; do
            python lora_bridge.py --serial "$LORA_SERIAL" || true
            echo "LoRa bridge exited — restarting in 5 s"
            sleep 5
        done
    ) &
else
    echo "No lora_serial configured — LoRa bridge disabled"
fi

echo "[4/4] Starting server..."
echo "Web interface: https://<home-assistant-host>:8443/"

exec python -m uvicorn app:app \
    --host 0.0.0.0 \
    --port 8443 \
    --ssl-keyfile key.pem \
    --ssl-certfile cert.pem \
    --timeout-graceful-shutdown 2
