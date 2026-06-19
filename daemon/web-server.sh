#!/usr/bin/env bash
# PocketMeter Web Server wrapper
# Launches the Python web server that serves the UI locally and proxies API calls to the ESP32.
#
# Environment:
#   ESP32_HOST / POCKETMETER_ESP_HOST - Optional ESP32 IP/hostname override
#   POCKETMETER_WEB_PORT - Local web server port (default: 8080)
#
# Usage:
#   ./web-server.sh              # Run in foreground
#   ./web-server.sh --daemon     # Run in background (detached)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PYTHON="${PYTHON:-python3}"

# Auto-detect Pillow in nix store if not already importable
if ! "$PYTHON" -c "import PIL" 2>/dev/null; then
  PY_VER=$("$PYTHON" -c "import sys; print(f'python{sys.version_info.major}.{sys.version_info.minor}')")
  NIX_PIL_DIR=$(find /nix/store -maxdepth 6 -name "PIL" -type d 2>/dev/null | grep "/${PY_VER}/site-packages" | head -1)
  if [[ -n "$NIX_PIL_DIR" ]]; then
    NIX_SITEPKGS="${NIX_PIL_DIR%/PIL}"
    export PYTHONPATH="${NIX_SITEPKGS}${PYTHONPATH:+:$PYTHONPATH}"
    echo "[web-server] Using Pillow from: $NIX_SITEPKGS" >&2
  fi
fi

# Defaults
export POCKETMETER_WEB_PORT="${POCKETMETER_WEB_PORT:-8080}"

log() {
    echo "[web-server] $(date '+%H:%M:%S') $*" >&2
}

if [[ "${1:-}" == "--daemon" ]]; then
    log "Starting in background on port $POCKETMETER_WEB_PORT (ESP32: ${POCKETMETER_ESP_HOST:-${ESP32_HOST:-auto}})"
    nohup "$PYTHON" "$SCRIPT_DIR/web-server.py" >"$SCRIPT_DIR/web-server.log" 2>&1 &
    PID=$!
    sleep 1
    if kill -0 "$PID" 2>/dev/null; then
        echo "$PID" >"$SCRIPT_DIR/web-server.pid"
        log "Running in background (PID: $PID, log: web-server.log)"
        log "Open http://localhost:$POCKETMETER_WEB_PORT"
    else
        log "Failed to start. Check web-server.log"
        exit 1
    fi
elif [[ "${1:-}" == "--stop" ]]; then
    if [[ -f "$SCRIPT_DIR/web-server.pid" ]]; then
        PID=$(cat "$SCRIPT_DIR/web-server.pid")
        if kill -0 "$PID" 2>/dev/null; then
            kill "$PID"
            rm -f "$SCRIPT_DIR/web-server.pid"
            log "Stopped (PID: $PID)"
        else
            log "Process not running, cleaning up PID file"
            rm -f "$SCRIPT_DIR/web-server.pid"
        fi
    else
        log "No PID file found"
    fi
else
    log "Starting in foreground on port $POCKETMETER_WEB_PORT (ESP32: ${POCKETMETER_ESP_HOST:-${ESP32_HOST:-auto}})"
    log "Open http://localhost:$POCKETMETER_WEB_PORT"
    exec "$PYTHON" "$SCRIPT_DIR/web-server.py"
fi
