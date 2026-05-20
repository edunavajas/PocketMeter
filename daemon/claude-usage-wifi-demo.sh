#!/bin/bash
# Claude Usage Tracker Daemon (WiFi) - DEMO VERSION
# Simulates usage data for testing the display
# Dependencies: curl

DEVICE_IP_FILE="$HOME/.config/claude-usage-monitor/esp-ip"
POLL_INTERVAL=10
TICK=5

log() {
    echo "[$(date '+%H:%M:%S')] $1"
}

# Load saved IP
load_ip() {
    if [ -f "$DEVICE_IP_FILE" ]; then
        DEVICE_IP=$(head -1 "$DEVICE_IP_FILE" | tr -d '\r\n ')
        if [ -n "$DEVICE_IP" ]; then
            return 0
        fi
    fi
    return 1
}

# Simulate usage data with variations
generate_data() {
    # Simulate session usage that varies between 10% and 80%
    session=$((10 + RANDOM % 70))
    # Weekly usage varies slower
    weekly=$((20 + RANDOM % 50))
    # Status based on session usage
    if [ $session -lt 50 ]; then
        status="allowed"
    elif [ $session -lt 80 ]; then
        status="approaching"
    else
        status="limited"
    fi
    
    echo "{\"s\":$session,\"sr\":$((120 - session)),\"w\":$weekly,\"wr\":$((7200 - weekly * 60)),\"st\":\"$status\",\"ok\":true}"
}

log "=== Claude Usage Tracker Daemon (WiFi DEMO) ==="
log "Poll interval: ${POLL_INTERVAL}s"
log "Sending simulated data to test the display"

BACKOFF=1

while true; do
    # Load IP
    if ! load_ip; then
        log "Error: No device IP configured. Set it in $DEVICE_IP_FILE"
        log "Example: echo '192.168.1.100' > $DEVICE_IP_FILE"
        sleep 10
        continue
    fi

    # Quick health check
    if ! curl -s -o /dev/null --connect-timeout 3 "http://${DEVICE_IP}/api/health" 2>/dev/null; then
        log "Device at $DEVICE_IP not responding, retrying in ${BACKOFF}s..."
        sleep "$BACKOFF"
        BACKOFF=$((BACKOFF < 60 ? BACKOFF * 2 : 60))
        continue
    fi

    BACKOFF=1

    # Poll loop
    LAST_POLL=0
    while curl -s -o /dev/null --connect-timeout 3 "http://${DEVICE_IP}/api/health" 2>/dev/null; do
        NOW=$(date +%s)
        if (( NOW - LAST_POLL >= POLL_INTERVAL )); then
            payload=$(generate_data)
            log "Sending to $DEVICE_IP: $payload"
            curl -s -X POST "http://${DEVICE_IP}/api/usage" \
                -H "Content-Type: application/json" \
                -d "$payload" >/dev/null 2>&1 || { log "HTTP POST failed"; }
            LAST_POLL=$NOW
        fi
        sleep "$TICK"
    done

    log "Device disconnected, retrying..."
    sleep 2
done
