#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PYTHON="${PYTHON:-python3}"

DAEMON_PID_FILE="$SCRIPT_DIR/pocketmeter-daemon.pid"
DAEMON_LOG_FILE="$SCRIPT_DIR/pocketmeter-daemon.log"

usage() {
  cat <<'EOF'
Usage: ./pocketmeter-stack.sh [--daemon|--stop|--status]

  ./pocketmeter-stack.sh         Run web UI + daemon in the foreground
  ./pocketmeter-stack.sh --daemon  Run web UI + daemon in the background
  ./pocketmeter-stack.sh --stop    Stop both background processes
  ./pocketmeter-stack.sh --status  Show background process status
EOF
}

log() {
  echo "[stack] $(date '+%H:%M:%S') $*" >&2
}

daemon_running() {
  if [[ -f "$DAEMON_PID_FILE" ]]; then
    local pid
    pid="$(<"$DAEMON_PID_FILE")"
    kill -0 "$pid" 2>/dev/null
  else
    return 1
  fi
}

start_daemon_background() {
  if daemon_running; then
    log "Daemon already running (PID: $(<"$DAEMON_PID_FILE"))"
    return
  fi
  log "Starting provider daemon in background"
  nohup "$PYTHON" "$SCRIPT_DIR/pocketmeter-daemon.py" >"$DAEMON_LOG_FILE" 2>&1 &
  local pid=$!
  sleep 1
  if kill -0 "$pid" 2>/dev/null; then
    echo "$pid" >"$DAEMON_PID_FILE"
    log "Daemon running in background (PID: $pid, log: $(basename "$DAEMON_LOG_FILE"))"
  else
    log "Daemon failed to start. Check $(basename "$DAEMON_LOG_FILE")"
    exit 1
  fi
}

stop_daemon_background() {
  if daemon_running; then
    local pid
    pid="$(<"$DAEMON_PID_FILE")"
    kill "$pid"
    rm -f "$DAEMON_PID_FILE"
    log "Stopped daemon (PID: $pid)"
  else
    rm -f "$DAEMON_PID_FILE"
    log "Daemon is not running"
  fi
}

show_status() {
  if [[ -f "$SCRIPT_DIR/web-server.pid" ]] && kill -0 "$(<"$SCRIPT_DIR/web-server.pid")" 2>/dev/null; then
    log "Web server: running (PID: $(<"$SCRIPT_DIR/web-server.pid"))"
  else
    log "Web server: stopped"
  fi

  if daemon_running; then
    log "Daemon: running (PID: $(<"$DAEMON_PID_FILE"))"
  else
    log "Daemon: stopped"
  fi
}

run_foreground() {
  log "Starting web UI in background for foreground stack run"
  bash "$SCRIPT_DIR/web-server.sh" &
  local web_pid=$!

  cleanup() {
    kill "$web_pid" 2>/dev/null || true
  }
  trap cleanup EXIT INT TERM

  log "Starting provider daemon in foreground"
  "$PYTHON" "$SCRIPT_DIR/pocketmeter-daemon.py"
}

case "${1:-}" in
  "")
    run_foreground
    ;;
  --daemon)
    bash "$SCRIPT_DIR/web-server.sh" --daemon
    start_daemon_background
    ;;
  --stop)
    bash "$SCRIPT_DIR/web-server.sh" --stop
    stop_daemon_background
    ;;
  --status)
    show_status
    ;;
  -h|--help)
    usage
    ;;
  *)
    usage >&2
    exit 1
    ;;
esac
