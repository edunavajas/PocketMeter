#!/usr/bin/env python3
"""
PocketMeter Multi-Provider Daemon
Reads AI provider usage data and sends to ESP32 over WiFi.

Supported providers:
- Claude (Anthropic): Reads ~/.claude/.credentials.json
- Codex (OpenAI): Reads ~/.codex/auth.json

Usage:
    python3 daemon/clawdmeter-daemon.py
"""

import json
import os
import sys
import time
import urllib.request
import urllib.error
from pathlib import Path
from datetime import datetime
import random

# Configuration
ESP_IP_FILE = Path.home() / ".config" / "claude-usage-monitor" / "esp-ip"
POLL_INTERVAL = 30  # seconds (reduced for demo)
TICK = 5  # seconds
BACKOFF_MAX = 60  # seconds

class Colors:
    GREEN = '\033[92m'
    YELLOW = '\033[93m'
    RED = '\033[91m'
    BLUE = '\033[94m'
    CYAN = '\033[96m'
    RESET = '\033[0m'

def log(msg, color=Colors.RESET):
    timestamp = datetime.now().strftime("%H:%M:%S")
    print(f"{color}[{timestamp}] {msg}{Colors.RESET}", flush=True)

def load_esp_ip():
    """Load ESP32 IP from config file."""
    if ESP_IP_FILE.exists():
        ip = ESP_IP_FILE.read_text().strip()
        if ip:
            return ip
    return None

def send_to_esp(data):
    """Send usage data to ESP32 via HTTP POST."""
    ip = load_esp_ip()
    if not ip:
        log("No ESP IP configured", Colors.RED)
        return False
    
    url = f"http://{ip}/api/usage"
    headers = {
        "Content-Type": "application/json"
    }
    
    try:
        req = urllib.request.Request(
            url,
            data=json.dumps(data).encode('utf-8'),
            headers=headers,
            method='POST'
        )
        with urllib.request.urlopen(req, timeout=5) as response:
            return response.status == 200
    except Exception as e:
        log(f"HTTP POST failed: {e}", Colors.RED)
        return False

def check_esp_health():
    """Check if ESP32 is responding."""
    ip = load_esp_ip()
    if not ip:
        return False
    
    try:
        urllib.request.urlopen(f"http://{ip}/api/health", timeout=3)
        return True
    except:
        return False

class ClaudeProvider:
    """Claude Code usage provider."""
    
    def __init__(self):
        self.name = "claude"
        self.credentials_file = Path.home() / ".claude" / ".credentials.json"
        self.available = False
        self.last_error = None
        self.use_simulated = False
        
    def check_available(self):
        """Check if Claude credentials exist."""
        if not self.credentials_file.exists():
            self.last_error = "No ~/.claude/.credentials.json"
            return False
        
        try:
            data = json.loads(self.credentials_file.read_text())
            if 'claudeAiOauth' not in data:
                self.last_error = "No claudeAiOauth in credentials"
                return False
            oauth = data['claudeAiOauth']
            if 'accessToken' not in oauth:
                self.last_error = "No accessToken in claudeAiOauth"
                return False
            self.available = True
            return True
        except Exception as e:
            self.last_error = f"Error reading credentials: {e}"
            return False
    
    def fetch_usage(self):
        """Fetch usage from Claude API."""
        if self.use_simulated:
            return self._simulate_data()
        
        try:
            creds = json.loads(self.credentials_file.read_text())
            oauth = creds.get('claudeAiOauth', {})
            token = oauth.get('accessToken', '')
            
            # Make API call to get usage headers
            req = urllib.request.Request(
                "https://api.anthropic.com/v1/messages",
                data=json.dumps({
                    "model": "claude-haiku-4-5-20251001",
                    "max_tokens": 1,
                    "messages": [{"role": "user", "content": "hi"}]
                }).encode('utf-8'),
                headers={
                    "Authorization": f"Bearer {token}",
                    "anthropic-version": "2023-06-01",
                    "anthropic-beta": "oauth-2025-04-20",
                    "Content-Type": "application/json",
                    "User-Agent": "claude-code/2.1.5"
                },
                method='POST'
            )
            
            with urllib.request.urlopen(req, timeout=10) as response:
                headers = dict(response.headers)
                
                # Parse rate limit headers
                s5h_util = headers.get('anthropic-ratelimit-unified-5h-utilization', '0')
                s5h_reset = headers.get('anthropic-ratelimit-unified-5h-reset', '0')
                s7d_util = headers.get('anthropic-ratelimit-unified-7d-utilization', '0')
                s7d_reset = headers.get('anthropic-ratelimit-unified-7d-reset', '0')
                status = headers.get('anthropic-ratelimit-unified-5h-status', 'unknown')
                
                now = int(time.time())
                
                # Calculate percentages and reset times
                session_pct = float(s5h_util) * 100
                session_reset = max(0, int((float(s5h_reset) - now) / 60))
                weekly_pct = float(s7d_util) * 100
                weekly_reset = max(0, int((float(s7d_reset) - now) / 60))
                
                return {
                    "provider": "claude",
                    "session_pct": round(session_pct, 1),
                    "session_reset": session_reset,
                    "weekly_pct": round(weekly_pct, 1),
                    "weekly_reset": weekly_reset,
                    "status": status,
                    "plan_type": oauth.get('subscriptionType', 'unknown'),
                    "ok": True
                }
                
        except urllib.error.HTTPError as e:
            if e.code == 401:
                self.last_error = "Token invalid or expired (401)"
                log("Claude API returned 401, switching to simulated data", Colors.YELLOW)
                self.use_simulated = True
                return self._simulate_data()
            else:
                self.last_error = f"API error {e.code}"
                return self._simulate_data()
        except Exception as e:
            self.last_error = f"Fetch error: {e}"
            return self._simulate_data()
    
    def _simulate_data(self):
        """Generate simulated data when API fails."""
        session = random.randint(10, 80)
        weekly = random.randint(20, 60)
        
        if session < 50:
            status = "allowed"
        elif session < 80:
            status = "approaching"
        else:
            status = "limited"
        
        return {
            "provider": "claude",
            "session_pct": session,
            "session_reset": 120 - session,
            "weekly_pct": weekly,
            "weekly_reset": 7200 - weekly * 60,
            "status": status,
            "plan_type": "pro",
            "simulated": True,
            "ok": True
        }

class CodexProvider:
    """Codex (OpenAI) usage provider."""
    
    def __init__(self):
        self.name = "codex"
        self.auth_file = Path.home() / ".codex" / "auth.json"
        self.available = False
        self.last_error = None
        
    def check_available(self):
        """Check if Codex auth exists."""
        if not self.auth_file.exists():
            self.last_error = "No ~/.codex/auth.json. Run 'codex login' first."
            return False
        
        try:
            data = json.loads(self.auth_file.read_text())
            if 'tokens' not in data or 'access_token' not in data.get('tokens', {}):
                self.last_error = "No access_token in auth.json"
                return False
            self.available = True
            return True
        except Exception as e:
            self.last_error = f"Error reading auth: {e}"
            return False
    
    def fetch_usage(self):
        """Fetch usage from Codex API."""
        try:
            auth = json.loads(self.auth_file.read_text())
            tokens = auth.get('tokens', {})
            access_token = tokens.get('access_token')
            account_id = tokens.get('account_id')
            
            if not access_token:
                self.last_error = "No access token"
                return None
            
            # Call Codex usage API
            req = urllib.request.Request(
                "https://chatgpt.com/backend-api/wham/usage",
                headers={
                    "Authorization": f"Bearer {access_token}",
                    "ChatGPT-Account-Id": account_id or "",
                    "User-Agent": "codex-cli",
                    "Accept": "application/json"
                }
            )
            
            with urllib.request.urlopen(req, timeout=10) as response:
                data = json.loads(response.read().decode('utf-8'))

                rate_limit = data.get('rate_limit', {})
                primary = rate_limit.get('primary_window', {})
                secondary = rate_limit.get('secondary_window', {})
                credits = data.get('credits', {})

                session_reset = int(primary.get('reset_after_seconds', 0) / 60)
                weekly_reset = int(secondary.get('reset_after_seconds', 0) / 60)

                # balance comes as string from the API
                try:
                    balance = float(credits.get('balance', 0))
                except (ValueError, TypeError):
                    balance = 0.0

                return {
                    "provider": "codex",
                    "plan_type": data.get('plan_type', 'unknown'),
                    "session_pct": primary.get('used_percent', 0),
                    "session_reset": session_reset,
                    "weekly_pct": secondary.get('used_percent', 0),
                    "weekly_reset": weekly_reset,
                    "credits_balance": balance,
                    "has_credits": credits.get('has_credits', False),
                    "ok": True
                }
                
        except urllib.error.HTTPError as e:
            if e.code == 401:
                self.last_error = "Token expired"
            else:
                self.last_error = f"API error {e.code}"
            return None
        except Exception as e:
            self.last_error = f"Fetch error: {e}"
            return None

def merge_provider_data(providers):
    """Merge data from all providers into a single payload."""
    data = {
        "providers": [],
        "timestamp": int(time.time()),
        "ok": False
    }
    
    has_data = False
    for provider in providers:
        # Re-check availability every poll so credentials added after startup are picked up
        if not provider.available:
            provider.check_available()

        if provider.available:
            result = provider.fetch_usage()
            if result:
                data["providers"].append(result)
                has_data = True
                simulated = result.get('simulated', False)
                status_color = Colors.YELLOW if simulated else Colors.GREEN
                log(f"{provider.name}: session={result.get('session_pct', 'N/A')}%, "
                    f"weekly={result.get('weekly_pct', 'N/A')}% "
                    f"({'SIMULATED' if simulated else 'REAL'})", status_color)
            else:
                log(f"{provider.name}: {provider.last_error}", Colors.YELLOW)
                data["providers"].append({
                    "provider": provider.name,
                    "error": provider.last_error,
                    "ok": False
                })
        else:
            log(f"{provider.name}: Not configured - {provider.last_error}", Colors.YELLOW)
            data["providers"].append({
                "provider": provider.name,
                "error": provider.last_error,
                "ok": False
            })
    
    data["ok"] = has_data
    return data

def main():
    log("=== PocketMeter Multi-Provider Daemon ===", Colors.BLUE)
    log("Providers: Claude, Codex", Colors.BLUE)
    log(f"Poll interval: {POLL_INTERVAL}s", Colors.BLUE)
    log("Press Ctrl+C to stop", Colors.CYAN)
    
    # Initialize providers
    providers = [ClaudeProvider(), CodexProvider()]
    
    # Check availability
    for provider in providers:
        provider.check_available()
    
    backoff = 1
    last_poll = 0
    
    while True:
        # Check ESP connectivity
        if not check_esp_health():
            log(f"ESP not responding, retrying in {backoff}s...", Colors.YELLOW)
            time.sleep(backoff)
            backoff = min(backoff * 2, BACKOFF_MAX)
            continue
        
        backoff = 1
        
        # Poll if interval passed
        now = int(time.time())
        if now - last_poll >= POLL_INTERVAL:
            log("Fetching provider data...", Colors.BLUE)
            data = merge_provider_data(providers)
            
            if send_to_esp(data):
                log("✓ Data sent to ESP32", Colors.GREEN)
                last_poll = now
            else:
                log("✗ Failed to send data", Colors.RED)
        
        time.sleep(TICK)

if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        log("\nDaemon stopped", Colors.YELLOW)
        sys.exit(0)
    except Exception as e:
        log(f"Fatal error: {e}", Colors.RED)
        sys.exit(1)
