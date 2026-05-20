#!/bin/bash
# Claude Usage Tracker Daemon (WiFi)
# Reads Claude Code OAuth token, polls usage via API, sends to ESP32 over HTTP.
# Dependencies: curl

DEVICE_IP_FILE="$HOME/.config/claude-usage-monitor/esp-ip"
POLL_INTERVAL=60
TICK=5

log() {
    echo "[$(date '+%H:%M:%S')] $1"
}

read_token() {
    grep -o '"accessToken":"[^"]*"' "$HOME/.claude/.credentials.json" | cut -d'"' -f4
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

save_ip() {
    mkdir -p "$(dirname "$DEVICE_IP_FILE")"
    echo "$DEVICE_IP" > "$DEVICE_IP_FILE"
}

poll() {
    local token
    token=$(read_token) || { log "Error: could not read token"; return 1; }
    local now
    now=$(date +%s)

    local headers
    headers=$(curl -s -D - -o /dev/null \
        "https://api.anthropic.com/v1/messages" \
        -H "Authorization: Bearer $token" \
        -H "anthropic-version: 2023-06-01" \
        -H "anthropic-beta: oauth-2025-04-20" \
        -H "Content-Type: application/json" \
        -H "User-Agent: claude-code/2.1.5" \
        -d '{"model":"claude-haiku-4-5-20251001","max_tokens":1,"messages":[{"role":"user","content":"hi"}]}' \
        2>/dev/null) || { log "Error: API call failed"; return 1; }

    local s5h_util s5h_reset s7d_util s7d_reset status
    s5h_util=$(echo "$headers" | grep -i "anthropic-ratelimit-unified-5h-utilization" | tr -d '\r' | awk '{print $2}')
    s5h_reset=$(echo "$headers" | grep -i "anthropic-ratelimit-unified-5h-reset" | tr -d '\r' | awk '{print $2}')
    s7d_util=$(echo "$headers" | grep -i "anthropic-ratelimit-unified-7d-utilization" | tr -d '\r' | awk '{print $2}')
    s7d_reset=$(echo "$headers" | grep -i "anthropic-ratelimit-unified-7d-reset" | tr -d '\r' | awk '{print $2}')
    status=$(echo "$headers" | grep -i "anthropic-ratelimit-unified-5h-status" | tr -d '\r' | awk '{print $2}')

    s5h_util=${s5h_util:-0}
    s5h_reset=${s5h_reset:-0}
    s7d_util=${s7d_util:-0}
    s7d_reset=${s7d_reset:-0}
    status=${status:-unknown}

    local payload
    payload=$(awk -v u5="$s5h_util" -v r5="$s5h_reset" -v u7="$s7d_util" -v r7="$s7d_reset" -v st="$status" -v now="$now" \
        'BEGIN {
            sp = sprintf("%.0f", u5 * 100);
            sr = (r5 - now) / 60; sr = sr > 0 ? sprintf("%.0f", sr) : 0;
            wp = sprintf("%.0f", u7 * 100);
            wr = (r7 - now) / 60; wr = wr > 0 ? sprintf("%.0f", wr) : 0;
            printf "{\"s\":%s,\"sr\":%s,\"w\":%s,\"wr\":%s,\"st\":\"%s\",\"ok\":true}", sp, sr, wp, wr, st;
        }')

    log "Sending to $DEVICE_IP: $payload"
    curl -s -X POST "http://${DEVICE_IP}/api/usage" \
        -H "Content-Type: application/json" \
        -d "$payload" >/dev/null 2>&1 || { log "HTTP POST failed"; return 1; }
    return 0
}

log "=== Claude Usage Tracker Daemon (WiFi) ==="
log "Poll interval: ${POLL_INTERVAL}s"

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
            poll && LAST_POLL=$NOW
        fi
        sleep "$TICK"
    done

    log "Device disconnected, retrying..."
    sleep 2
done
