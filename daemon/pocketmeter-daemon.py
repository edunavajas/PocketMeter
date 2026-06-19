#!/usr/bin/env python3
"""
PocketMeter Multi-Provider Daemon
Reads AI provider usage data and sends to ESP32 over WiFi.

Supported providers (CLI/file-based auth):
  - Claude     ~/.claude/.credentials.json
  - Codex      ~/.codex/auth.json
  - Gemini     ~/.gemini/oauth_creds.json
  - Copilot    ~/.config/github-copilot/hosts.json  or  COPILOT_API_TOKEN env
  - Grok       ~/.grok/auth.json
  - Windsurf   ~/.config/Windsurf/.../state.vscdb   (SQLite)
  - Cursor     CURSOR_SESSION_TOKEN env (browser cookie)

API-key providers (env vars):
  - OpenAI     OPENAI_API_KEY
  - DeepSeek   DEEPSEEK_API_KEY
  - Anthropic  ANTHROPIC_API_KEY  (admin org usage, separate from Claude CLI)

Usage:
    python3 daemon/pocketmeter-daemon.py
"""

import ipaddress
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

from claude_oauth import get_access_token as get_claude_access_token

# ── Configuration ───────────────────────────────────────────────────────────
ESP_IP_FILE = Path.home() / ".config" / "pocketmeter" / "esp-ip"
LEGACY_ESP_IP_FILE = Path.home() / ".config" / "claude-usage-monitor" / "esp-ip"
ESP_HOST_OVERRIDE = "POCKETMETER_ESP_HOST"
ESP_DEFAULT_HOSTNAMES = (
    "pocketmeter.local",
    "pocketmeter",
    "clawdmeter.local",
    "clawdmeter",
)
POLL_INTERVAL = 30
TICK = 5
BACKOFF_MAX = 60
HTTP_TIMEOUT = 3
SUBNET_SCAN_TIMEOUT = 0.75
SUBNET_SCAN_WORKERS = 48
KEYS_FILE = Path.home() / ".config" / "pocketmeter" / "api-keys.json"


def _normalize_secret(value):
    if value is None:
        return None
    if not isinstance(value, str):
        value = str(value)
    value = value.strip()
    if not value:
        return None
    if len(value) >= 2 and value[0] == value[-1] and value[0] in ('"', "'"):
        value = value[1:-1].strip()
    if value.lower().startswith("bearer "):
        value = value[7:].strip()
    return value or None


def _load_saved_keys():
    """Load API keys saved via the web UI."""
    try:
        if KEYS_FILE.exists():
            data = json.loads(KEYS_FILE.read_text())
            if isinstance(data, dict):
                return {
                    provider: _normalize_secret(value)
                    for provider, value in data.items()
                    if _normalize_secret(value)
                }
    except Exception:
        pass
    return {}


def _get_api_key(*env_vars, provider_id=None):
    """Get an API key from env vars first, then from the saved keys file."""
    for v in env_vars:
        k = _normalize_secret(os.environ.get(v))
        if k:
            return k
    if provider_id:
        return _load_saved_keys().get(provider_id)
    return None


def _extract_http_error_message(error):
    try:
        payload = json.loads(error.read().decode("utf-8", "replace"))
    except Exception:
        return ""
    if isinstance(payload, dict):
        err = payload.get("error")
        if isinstance(err, dict):
            msg = err.get("message")
            typ = err.get("type")
            if msg and typ:
                return f"{msg} ({typ})"
            if msg:
                return str(msg)
            if typ:
                return str(typ)
    return ""


# ── Console colors ───────────────────────────────────────────────────────────
class Colors:
    GREEN  = '\033[92m'
    YELLOW = '\033[93m'
    RED    = '\033[91m'
    BLUE   = '\033[94m'
    CYAN   = '\033[96m'
    RESET  = '\033[0m'


def log(msg, color=Colors.RESET):
    ts = datetime.now().strftime("%H:%M:%S")
    print(f"{color}[{ts}] {msg}{Colors.RESET}", flush=True)


# ── ESP discovery helpers ────────────────────────────────────────────────────
@dataclass(frozen=True)
class EspEndpoint:
    host: str
    ip: str
    source: str
    hostname: str = ""


def normalize_host(value):
    if not value:
        return None
    value = value.strip()
    if not value:
        return None
    for prefix in ("https://", "http://"):
        if value.startswith(prefix):
            value = value[len(prefix):]
    return value.strip("/") or None


def is_ipv4_address(value):
    try:
        ipaddress.IPv4Address(value)
        return True
    except ipaddress.AddressValueError:
        return False


def load_configured_host():
    for cache_file in (ESP_IP_FILE, LEGACY_ESP_IP_FILE):
        try:
            if cache_file.exists():
                host = normalize_host(cache_file.read_text())
                if host:
                    return host
        except OSError:
            pass
    return None


def save_configured_host(host):
    host = normalize_host(host)
    if not host:
        return
    ESP_IP_FILE.parent.mkdir(parents=True, exist_ok=True)
    ESP_IP_FILE.write_text(f"{host}\n")


def make_url(host, path):
    return f"http://{host}{path}"


def dashboard_url(endpoint):
    host = endpoint.ip if endpoint and endpoint.ip else endpoint.host
    return make_url(host, "/")


def resolve_local_ipv4_addresses():
    addresses = set()
    for target in ("8.8.8.8", "1.1.1.1"):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            sock.connect((target, 80))
            addresses.add(sock.getsockname()[0])
        except OSError:
            pass
        finally:
            sock.close()
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET, socket.SOCK_STREAM):
            addr = info[4][0]
            if addr:
                addresses.add(addr)
    except socket.gaierror:
        pass
    filtered = []
    for addr in sorted(addresses):
        if addr.startswith("127.") or addr.startswith("169.254."):
            continue
        if is_ipv4_address(addr):
            filtered.append(addr)
    return filtered


def discovery_subnets(seed_host=None):
    subnets = []
    seen = set()

    def add_subnet_from_ip(ip):
        try:
            network = ipaddress.ip_network(f"{ip}/24", strict=False)
        except ValueError:
            return
        key = str(network)
        if key not in seen:
            seen.add(key)
            subnets.append(network)

    seed_host = normalize_host(seed_host)
    if seed_host and is_ipv4_address(seed_host):
        add_subnet_from_ip(seed_host)
    for addr in resolve_local_ipv4_addresses():
        add_subnet_from_ip(addr)
    return subnets


def validate_esp_host(host, source, timeout=HTTP_TIMEOUT):
    host = normalize_host(host)
    if not host:
        return None
    try:
        with urllib.request.urlopen(make_url(host, "/api/status"), timeout=timeout) as response:
            payload = json.loads(response.read().decode("utf-8"))
    except Exception:
        return None
    if not isinstance(payload, dict):
        return None
    ip = payload.get("ip")
    wifi_connected = payload.get("wifi_connected")
    if not is_ipv4_address(ip) or not isinstance(wifi_connected, bool):
        return None
    hostname = payload.get("hostname") or ""
    return EspEndpoint(host=host, ip=ip, source=source, hostname=hostname)


def discovery_candidates():
    candidates = []
    seen = set()

    def add(host, source):
        host = normalize_host(host)
        if host and host not in seen:
            seen.add(host)
            candidates.append((host, source))

    add(os.environ.get(ESP_HOST_OVERRIDE), f"env:{ESP_HOST_OVERRIDE}")
    add(load_configured_host(), "cache")
    for hostname in ESP_DEFAULT_HOSTNAMES:
        add(hostname, "hostname")
    return candidates


def refresh_cached_ip(cached_host, endpoint):
    cached_host = normalize_host(cached_host)
    if not endpoint or not is_ipv4_address(endpoint.ip):
        return
    if cached_host and not is_ipv4_address(cached_host):
        return
    if cached_host != endpoint.ip:
        save_configured_host(endpoint.ip)


def scan_subnet_for_esp(network):
    log(f"Scanning subnet {network} for PocketMeter...", Colors.CYAN)
    hosts = [str(ip) for ip in network.hosts()]
    executor = ThreadPoolExecutor(max_workers=SUBNET_SCAN_WORKERS)
    try:
        futures = {executor.submit(probe, ip): ip for ip in hosts}
        for future in as_completed(futures):
            endpoint = future.result()
            if endpoint:
                executor.shutdown(wait=False, cancel_futures=True)
                return endpoint
    finally:
        executor.shutdown(wait=False, cancel_futures=True)
    return None


def probe(ip):
    return validate_esp_host(ip, "subnet-scan", timeout=SUBNET_SCAN_TIMEOUT)


def discover_esp_endpoint():
    cached_host = load_configured_host()
    for host, source in discovery_candidates():
        endpoint = validate_esp_host(host, source)
        if endpoint:
            refresh_cached_ip(cached_host, endpoint)
            return endpoint
    for network in discovery_subnets(cached_host):
        endpoint = scan_subnet_for_esp(network)
        if endpoint:
            refresh_cached_ip(cached_host, endpoint)
            return endpoint
    return None


def send_to_esp(data, endpoint):
    if not endpoint:
        log("No ESP endpoint available", Colors.RED)
        return False
    url = make_url(endpoint.host, "/api/usage")
    try:
        req = urllib.request.Request(
            url,
            data=json.dumps(data).encode("utf-8"),
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        with urllib.request.urlopen(req, timeout=5) as response:
            return response.status == 200
    except Exception as e:
        log(f"HTTP POST to {endpoint.host} failed: {e}", Colors.RED)
        return False


def check_esp_health(endpoint):
    if not endpoint:
        return False
    try:
        urllib.request.urlopen(make_url(endpoint.host, "/api/health"), timeout=HTTP_TIMEOUT)
        return True
    except Exception:
        return False


# ── Provider base helpers ────────────────────────────────────────────────────
def _iso_to_reset_mins(iso_str):
    """Parse ISO 8601 datetime → minutes from now (0 if past)."""
    try:
        dt = datetime.fromisoformat(iso_str.replace("Z", "+00:00"))
        now = datetime.now(timezone.utc)
        return max(0, int((dt - now).total_seconds() / 60))
    except Exception:
        return 0


def _status_from_pct(pct):
    if pct >= 100:
        return "limited"
    if pct >= 80:
        return "approaching"
    return "allowed"


# ── Provider: Claude ─────────────────────────────────────────────────────────
class ClaudeProvider:
    name = "claude"
    _API_URL = "https://api.anthropic.com/v1/messages"

    def __init__(self):
        self.credentials_file = Path.home() / ".claude" / ".credentials.json"
        self.available = False
        self.last_error = None

    def check_available(self):
        if not self.credentials_file.exists():
            self.last_error = "No ~/.claude/.credentials.json"
            return False
        try:
            data = json.loads(self.credentials_file.read_text())
            oauth = data.get("claudeAiOauth", {})
            if not (oauth.get("accessToken") or oauth.get("refreshToken")):
                self.last_error = "No accessToken or refreshToken in claudeAiOauth"
                return False
            self.available = True
            return True
        except Exception as e:
            self.last_error = f"Error reading credentials: {e}"
            return False

    def _request_usage(self, token):
        req = urllib.request.Request(
            self._API_URL,
            data=json.dumps({
                "model": "claude-haiku-4-5-20251001",
                "max_tokens": 1,
                "messages": [{"role": "user", "content": "hi"}],
            }).encode("utf-8"),
            headers={
                "Authorization": f"Bearer {token}",
                "anthropic-version": "2023-06-01",
                "anthropic-beta": "oauth-2025-04-20",
                "Content-Type": "application/json",
                "User-Agent": "claude-code/2.1.5",
            },
            method="POST",
        )
        return urllib.request.urlopen(req, timeout=10)

    def fetch_usage(self):
        try:
            token, oauth = get_claude_access_token(self.credentials_file)
            did_retry = False
            while True:
                try:
                    response_cm = self._request_usage(token)
                    break
                except urllib.error.HTTPError as e:
                    if e.code == 401 and not did_retry:
                        did_retry = True
                        token, oauth = get_claude_access_token(self.credentials_file, force_refresh=True)
                        continue
                    raise
            with response_cm as response:
                h = dict(response.headers)
                s5h_util   = float(h.get("anthropic-ratelimit-unified-5h-utilization", "0"))
                s5h_reset  = float(h.get("anthropic-ratelimit-unified-5h-reset",       "0"))
                s7d_util   = float(h.get("anthropic-ratelimit-unified-7d-utilization", "0"))
                s7d_reset  = float(h.get("anthropic-ratelimit-unified-7d-reset",       "0"))
                status     = h.get("anthropic-ratelimit-unified-5h-status", "unknown")
                now        = int(time.time())
                session_pct   = s5h_util * 100
                session_reset = max(0, int((s5h_reset - now) / 60))
                weekly_pct    = s7d_util * 100
                weekly_reset  = max(0, int((s7d_reset - now) / 60))
                return {
                    "provider":      "claude",
                    "session_pct":   round(session_pct, 1),
                    "session_reset": session_reset,
                    "weekly_pct":    round(weekly_pct, 1),
                    "weekly_reset":  weekly_reset,
                    "status":        status,
                    "plan_type":     oauth.get("subscriptionType", "unknown"),
                    "ok":            True,
                }
        except urllib.error.HTTPError as e:
            self.last_error = f"API error {e.code}"
            if e.code == 401:
                log("Claude API 401 → simulated data", Colors.YELLOW)
                return self._simulate()
            return self._simulate()
        except Exception as e:
            self.last_error = f"Fetch error: {e}"
            return self._simulate()

    def _simulate(self):
        import random
        s = random.randint(10, 80)
        w = random.randint(20, 60)
        return {
            "provider":      "claude",
            "session_pct":   s,
            "session_reset": 120 - s,
            "weekly_pct":    w,
            "weekly_reset":  7200 - w * 60,
            "status":        _status_from_pct(s),
            "plan_type":     "pro",
            "simulated":     True,
            "ok":            True,
        }


# ── Provider: Codex ──────────────────────────────────────────────────────────
class CodexProvider:
    name = "codex"
    _AUTH_READ_RETRIES = 4
    _AUTH_READ_DELAY = 0.2
    _TRANSIENT_GRACE_SECONDS = 180

    def __init__(self):
        self.auth_file = Path.home() / ".codex" / "auth.json"
        self.available = False
        self.last_error = None
        self._last_ok_result = None
        self._last_ok_at = 0

    def _load_auth(self):
        last_error = None
        for attempt in range(self._AUTH_READ_RETRIES):
            try:
                return json.loads(self.auth_file.read_text())
            except Exception as e:
                last_error = e
                if attempt + 1 < self._AUTH_READ_RETRIES:
                    time.sleep(self._AUTH_READ_DELAY)
        if last_error:
            raise last_error
        return {}

    def _cached_result(self, reason):
        if not self._last_ok_result:
            return None
        age = time.time() - self._last_ok_at
        if age > self._TRANSIENT_GRACE_SECONDS:
            return None
        cached = dict(self._last_ok_result)
        cached["stale"] = True
        cached["stale_reason"] = reason
        cached["stale_age_seconds"] = int(age)
        return cached

    def check_available(self):
        if not self.auth_file.exists():
            self.last_error = "No ~/.codex/auth.json. Run 'codex login' first."
            return False
        try:
            data = self._load_auth()
            tokens = data.get("tokens", {}) if isinstance(data.get("tokens"), dict) else {}
            if not (tokens.get("access_token") or tokens.get("refresh_token")):
                self.last_error = "No Codex token in auth.json"
                return False
            self.available = True
            return True
        except Exception as e:
            self.last_error = f"Error reading auth: {e}"
            return False

    def fetch_usage(self):
        try:
            auth         = self._load_auth()
            tokens       = auth.get("tokens", {}) if isinstance(auth.get("tokens"), dict) else {}
            access_token = tokens.get("access_token")
            account_id   = tokens.get("account_id")
            if not access_token:
                self.last_error = "Codex login refresh in progress"
                return self._cached_result(self.last_error)
            req = urllib.request.Request(
                "https://chatgpt.com/backend-api/wham/usage",
                headers={
                    "Authorization":      f"Bearer {access_token}",
                    "ChatGPT-Account-Id": account_id or "",
                    "User-Agent":         "codex-cli",
                    "Accept":             "application/json",
                },
            )
            with urllib.request.urlopen(req, timeout=10) as response:
                data        = json.loads(response.read().decode("utf-8"))
            rate_limit  = data.get("rate_limit", {})
            primary     = rate_limit.get("primary_window", {})
            secondary   = rate_limit.get("secondary_window", {})
            credits     = data.get("credits", {})
            try:
                balance = float(credits.get("balance", 0))
            except (ValueError, TypeError):
                balance = 0.0
            result = {
                "provider":       "codex",
                "plan_type":      data.get("plan_type", "unknown"),
                "session_pct":    primary.get("used_percent", 0),
                "session_reset":  int(primary.get("reset_after_seconds", 0) / 60),
                "weekly_pct":     secondary.get("used_percent", 0),
                "weekly_reset":   int(secondary.get("reset_after_seconds", 0) / 60),
                "credits_balance": balance,
                "has_credits":    credits.get("has_credits", False),
                "ok":             True,
            }
            self._last_ok_result = dict(result)
            self._last_ok_at = time.time()
            return result
        except urllib.error.HTTPError as e:
            self.last_error = "Token expired" if e.code == 401 else f"API error {e.code}"
            return self._cached_result(self.last_error)
        except Exception as e:
            self.last_error = f"Fetch error: {e}"
            return self._cached_result(self.last_error)


# ── Provider: Gemini ─────────────────────────────────────────────────────────
class GeminiProvider:
    name = "gemini"
    _QUOTA_URL    = "https://cloudcode-pa.googleapis.com/v1internal:retrieveUserQuota"
    _REFRESH_URL  = "https://oauth2.googleapis.com/token"

    def __init__(self):
        self.creds_file    = Path.home() / ".gemini" / "oauth_creds.json"
        self.settings_file = Path.home() / ".gemini" / "settings.json"
        self.available     = False
        self.last_error    = None
        self._oauth_creds  = None          # {client_id, client_secret}

    def check_available(self):
        if not self.creds_file.exists():
            self.last_error = "No ~/.gemini/oauth_creds.json. Run 'gemini' first."
            return False
        try:
            data = json.loads(self.creds_file.read_text())
            if not (data.get("access_token") or data.get("refresh_token")):
                self.last_error = "No token in credentials file"
                return False
            self.available = True
            return True
        except Exception as e:
            self.last_error = f"Error reading credentials: {e}"
            return False

    # Extract OAuth client credentials from locally installed Gemini CLI
    def _load_oauth_creds(self):
        if self._oauth_creds:
            return self._oauth_creds
        try:
            result = subprocess.run(["which", "gemini"], capture_output=True, text=True)
            if result.returncode != 0:
                return None
            gemini_bin  = Path(result.stdout.strip()).resolve()
            pkg_root    = gemini_bin.parent
            # Walk up to find a directory that looks like an npm package root
            for _ in range(6):
                pkg_json = pkg_root / "package.json"
                if pkg_json.exists():
                    try:
                        pkg = json.loads(pkg_json.read_text())
                        if "gemini" in pkg.get("name", ""):
                            break
                    except Exception:
                        pass
                pkg_root = pkg_root.parent
            # Try known relative paths for oauth2.js inside the package
            candidates = [
                pkg_root / "dist/src/code_assist/oauth2.js",
                pkg_root / "node_modules/@google/gemini-cli-core/dist/src/code_assist/oauth2.js",
                pkg_root / "node_modules/@google/gemini-cli/node_modules/@google/gemini-cli-core/dist/src/code_assist/oauth2.js",
            ]
            for path in candidates:
                if path.exists():
                    content = path.read_text(errors="replace")
                    cid  = re.search(r'CLIENT_ID\s*=\s*["\']([^"\']{20,})["\']', content)
                    csec = re.search(r'CLIENT_SECRET\s*=\s*["\']([^"\']{10,})["\']', content)
                    if cid and csec:
                        self._oauth_creds = {"client_id": cid.group(1), "client_secret": csec.group(1)}
                        return self._oauth_creds
        except Exception:
            pass
        return None

    def _refresh_token(self, refresh_token):
        oauth = self._load_oauth_creds()
        if not oauth:
            return None
        data = urllib.parse.urlencode({
            "client_id":     oauth["client_id"],
            "client_secret": oauth["client_secret"],
            "refresh_token": refresh_token,
            "grant_type":    "refresh_token",
        }).encode("utf-8")
        try:
            req = urllib.request.Request(self._REFRESH_URL, data=data, method="POST")
            with urllib.request.urlopen(req, timeout=10) as r:
                return json.loads(r.read().decode("utf-8"))
        except Exception:
            return None

    def _get_access_token(self):
        creds = json.loads(self.creds_file.read_text())
        access_token  = creds.get("access_token", "")
        refresh_token = creds.get("refresh_token", "")
        expiry = creds.get("expiry_date", 0)
        # expiry_date can be epoch-seconds or epoch-milliseconds
        now_ms = time.time() * 1000
        expired = expiry > 0 and (expiry < (time.time() if expiry < 1e12 else now_ms))
        if not access_token or expired:
            if not refresh_token:
                return None
            refreshed = self._refresh_token(refresh_token)
            if refreshed and refreshed.get("access_token"):
                creds["access_token"] = refreshed["access_token"]
                if "expires_in" in refreshed:
                    creds["expiry_date"] = int(time.time() + refreshed["expires_in"])
                self.creds_file.write_text(json.dumps(creds, indent=2))
                return refreshed["access_token"]
            return None
        return access_token

    def fetch_usage(self):
        try:
            token = self._get_access_token()
            if not token:
                self.last_error = "No valid access token (run 'gemini' to re-auth)"
                return None
            req = urllib.request.Request(
                self._QUOTA_URL,
                headers={"Authorization": f"Bearer {token}", "Content-Type": "application/json"},
            )
            with urllib.request.urlopen(req, timeout=10) as response:
                data = json.loads(response.read().decode("utf-8"))
            quotas = data.get("modelQuotas", [])
            session_pct = session_reset = weekly_pct = weekly_reset = 0
            for i, q in enumerate(quotas):
                model_id  = q.get("modelId", "").lower()
                pct_used  = 100 - q.get("percentLeft", 100)
                reset_mins = _iso_to_reset_mins(q.get("resetTime", ""))
                if i == 0 or "pro" in model_id:
                    session_pct   = pct_used
                    session_reset = reset_mins
                elif "flash" in model_id and "lite" not in model_id:
                    weekly_pct    = pct_used
                    weekly_reset  = reset_mins
            return {
                "provider":      "gemini",
                "session_pct":   round(session_pct, 1),
                "session_reset": session_reset,
                "weekly_pct":    round(weekly_pct, 1),
                "weekly_reset":  weekly_reset,
                "status":        _status_from_pct(session_pct),
                "plan_type":     "gemini",
                "ok":            True,
            }
        except urllib.error.HTTPError as e:
            if e.code == 401:
                # Force token refresh next attempt
                try:
                    creds = json.loads(self.creds_file.read_text())
                    creds["access_token"] = ""
                    self.creds_file.write_text(json.dumps(creds, indent=2))
                except Exception:
                    pass
                self.last_error = "Token expired (401) — will refresh next poll"
            else:
                self.last_error = f"API error {e.code}"
            return None
        except Exception as e:
            self.last_error = f"Fetch error: {e}"
            return None


# ── Provider: Copilot ────────────────────────────────────────────────────────
class CopilotProvider:
    name = "copilot"
    _API_URL = "https://api.github.com/copilot_internal/user"

    def __init__(self):
        self.hosts_file = Path.home() / ".config" / "github-copilot" / "hosts.json"
        self.available  = False
        self.last_error = None

    def check_available(self):
        if os.environ.get("COPILOT_API_TOKEN") or os.environ.get("GITHUB_TOKEN"):
            self.available = True
            return True
        if self.hosts_file.exists():
            try:
                data = json.loads(self.hosts_file.read_text())
                if data.get("github.com", {}).get("oauth_token"):
                    self.available = True
                    return True
            except Exception:
                pass
        self.last_error = "No GitHub Copilot token. Set COPILOT_API_TOKEN or log in."
        return False

    def _token(self):
        t = os.environ.get("COPILOT_API_TOKEN") or os.environ.get("GITHUB_TOKEN")
        if t:
            return t
        if self.hosts_file.exists():
            try:
                data = json.loads(self.hosts_file.read_text())
                return data.get("github.com", {}).get("oauth_token")
            except Exception:
                pass
        return None

    def fetch_usage(self):
        try:
            token = self._token()
            if not token:
                self.last_error = "No token"
                return None
            req = urllib.request.Request(
                self._API_URL,
                headers={
                    "Authorization":    f"token {token}",
                    "X-Github-Api-Version": "2025-04-01",
                    "Accept":           "application/json",
                    "User-Agent":       "PocketMeter/1.0",
                },
            )
            with urllib.request.urlopen(req, timeout=10) as r:
                data = json.loads(r.read().decode("utf-8"))
            snapshots = data.get("quotaSnapshots", {})
            premium   = snapshots.get("premiumInteractions", {})
            chat      = snapshots.get("chat", {})
            used_pct  = float(premium.get("usedPercent", 0))
            chat_pct  = float(chat.get("usedPercent", 0))
            plan      = data.get("copilotPlan", "copilot").lower()
            return {
                "provider":      "copilot",
                "session_pct":   round(used_pct, 1),
                "session_reset": 0,
                "weekly_pct":    round(chat_pct, 1),
                "weekly_reset":  0,
                "status":        _status_from_pct(used_pct),
                "plan_type":     plan,
                "ok":            True,
            }
        except urllib.error.HTTPError as e:
            self.last_error = f"API error {e.code}"
            return None
        except Exception as e:
            self.last_error = f"Fetch error: {e}"
            return None


# ── Provider: Grok ───────────────────────────────────────────────────────────
class GrokProvider:
    name = "grok"
    # Preferred scope keys in priority order
    _SCOPE_ORDER = ["https://auth.x.ai::*", "https://accounts.x.ai/sign-in"]

    def __init__(self):
        grok_home       = os.environ.get("GROK_HOME", str(Path.home() / ".grok"))
        self.auth_file  = Path(grok_home) / "auth.json"
        self.available  = False
        self.last_error = None

    def check_available(self):
        if not self.auth_file.exists():
            self.last_error = "No ~/.grok/auth.json. Run 'grok' to authenticate."
            return False
        try:
            data = json.loads(self.auth_file.read_text())
            if any(isinstance(v, dict) and v.get("key") for v in data.values()):
                self.available = True
                return True
            self.last_error = "No valid key in auth.json"
            return False
        except Exception as e:
            self.last_error = f"Error reading auth: {e}"
            return False

    def _entry(self):
        data = json.loads(self.auth_file.read_text())
        for scope in self._SCOPE_ORDER:
            if scope in data and isinstance(data[scope], dict) and data[scope].get("key"):
                return data[scope]
        for v in data.values():
            if isinstance(v, dict) and v.get("key"):
                return v
        return {}

    def fetch_usage(self):
        try:
            entry = self._entry()
            if not entry:
                self.last_error = "No valid entry"
                return None
            # Grok doesn't expose a public REST usage endpoint yet;
            # return connected status with auth metadata.
            return {
                "provider":      "grok",
                "session_pct":   0.0,
                "session_reset": 0,
                "weekly_pct":    0.0,
                "weekly_reset":  0,
                "status":        "allowed",
                "plan_type":     entry.get("auth_mode", "grok"),
                "ok":            True,
            }
        except Exception as e:
            self.last_error = f"Fetch error: {e}"
            return None


# ── Provider: OpenAI (API key) ───────────────────────────────────────────────
class OpenAIProvider:
    name = "openai"

    def __init__(self):
        self.available  = False
        self.last_error = None

    def check_available(self):
        if _get_api_key("OPENAI_API_KEY", provider_id="openai"):
            self.available = True
            return True
        self.last_error = "No OPENAI_API_KEY — set env var or save via web UI"
        return False

    def fetch_usage(self):
        try:
            key = _get_api_key("OPENAI_API_KEY", provider_id="openai")
            if not key:
                return None
            # Fetch organization costs for last 24 h
            now   = int(time.time())
            start = now - 86400
            url   = f"https://api.openai.com/v1/organization/costs?start_time={start}&end_time={now}&limit=7"
            req   = urllib.request.Request(
                url,
                headers={"Authorization": f"Bearer {key}", "Accept": "application/json"},
            )
            with urllib.request.urlopen(req, timeout=10) as r:
                data = json.loads(r.read().decode("utf-8"))
            total_cost = sum(
                float(item.get("amount", {}).get("value", 0))
                for item in data.get("data", [])
            )
            return {
                "provider":       "openai",
                "session_pct":    0.0,
                "session_reset":  0,
                "weekly_pct":     0.0,
                "weekly_reset":   0,
                "status":         "allowed",
                "plan_type":      "api",
                "credits_balance": round(total_cost, 4),
                "has_credits":    True,
                "ok":             True,
            }
        except urllib.error.HTTPError as e:
            code = e.code
            if code in (401, 403):
                self.last_error = "API key invalid or no admin access"
            else:
                self.last_error = f"API error {code}"
            return None
        except Exception as e:
            self.last_error = f"Fetch error: {e}"
            return None


# ── Provider: DeepSeek (API key) ─────────────────────────────────────────────
class DeepSeekProvider:
    name = "deepseek"
    _API_URL = "https://api.deepseek.com/user/balance"

    def __init__(self):
        self.available  = False
        self.last_error = None

    def check_available(self):
        if _get_api_key("DEEPSEEK_API_KEY", "DEEPSEEK_KEY", provider_id="deepseek"):
            self.available = True
            return True
        self.last_error = "No DEEPSEEK_API_KEY — set env var or save via web UI"
        return False

    def fetch_usage(self):
        try:
            key = _get_api_key("DEEPSEEK_API_KEY", "DEEPSEEK_KEY", provider_id="deepseek")
            if not key:
                return None
            req = urllib.request.Request(
                self._API_URL,
                headers={"Authorization": f"Bearer {key}", "Accept": "application/json"},
            )
            with urllib.request.urlopen(req, timeout=10) as r:
                data = json.loads(r.read().decode("utf-8"))
            total_balance = 0.0
            currency      = "USD"
            for info in data.get("balance_infos", []):
                c = info.get("currency", "")
                if c in ("USD", "CNY"):
                    try:
                        total_balance = float(info.get("total_balance", 0))
                        currency = c
                        break
                    except (ValueError, TypeError):
                        pass
            return {
                "provider":       "deepseek",
                "session_pct":    0.0,
                "session_reset":  0,
                "weekly_pct":     0.0,
                "weekly_reset":   0,
                "status":         "allowed" if data.get("is_available", True) else "limited",
                "plan_type":      f"api/{currency.lower()}",
                "credits_balance": round(total_balance, 4),
                "has_credits":    True,
                "ok":             True,
            }
        except urllib.error.HTTPError as e:
            self.last_error = f"API error {e.code}"
            return None
        except Exception as e:
            self.last_error = f"Fetch error: {e}"
            return None


# ── Provider: Windsurf (SQLite) ───────────────────────────────────────────────
class WindsurfProvider:
    name = "windsurf"
    _DB_KEY = "windsurf.settings.cachedPlanInfo"

    def __init__(self):
        self.db_path    = self._find_db()
        self.available  = False
        self.last_error = None

    @staticmethod
    def _find_db():
        candidates = [
            # Linux / XDG
            Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config"))
                / "Windsurf" / "User" / "globalStorage" / "state.vscdb",
            Path.home() / ".config" / "Windsurf" / "User" / "globalStorage" / "state.vscdb",
            # macOS
            Path.home() / "Library" / "Application Support" / "Windsurf"
                / "User" / "globalStorage" / "state.vscdb",
        ]
        for p in candidates:
            if p.exists():
                return p
        return None

    def check_available(self):
        if self.db_path and self.db_path.exists():
            self.available = True
            return True
        self.last_error = "Windsurf not installed or database not found"
        return False

    def fetch_usage(self):
        try:
            import sqlite3
            with tempfile.NamedTemporaryFile(suffix=".db", delete=False) as tmp:
                shutil.copy2(self.db_path, tmp.name)
                tmp_path = tmp.name
            try:
                conn   = sqlite3.connect(tmp_path)
                cursor = conn.cursor()
                cursor.execute(
                    "SELECT value FROM ItemTable WHERE key = ? LIMIT 1",
                    (self._DB_KEY,),
                )
                row = cursor.fetchone()
                conn.close()
            finally:
                Path(tmp_path).unlink(missing_ok=True)
            if not row:
                self.last_error = "No plan info in Windsurf database"
                return None
            plan_info = json.loads(row[0])
            quota     = plan_info.get("quotaUsage", {})
            now       = int(time.time())
            daily_rem  = float(quota.get("dailyRemainingPercent", 100))
            weekly_rem = float(quota.get("weeklyRemainingPercent", 100))
            daily_reset_at  = int(quota.get("dailyResetAtUnix", 0))
            weekly_reset_at = int(quota.get("weeklyResetAtUnix", 0))
            session_pct   = 100 - daily_rem
            weekly_pct    = 100 - weekly_rem
            session_reset = max(0, (daily_reset_at  - now) // 60)
            weekly_reset  = max(0, (weekly_reset_at - now) // 60)
            plan_name     = plan_info.get("planName", "windsurf").lower()
            return {
                "provider":      "windsurf",
                "session_pct":   round(session_pct, 1),
                "session_reset": session_reset,
                "weekly_pct":    round(weekly_pct, 1),
                "weekly_reset":  weekly_reset,
                "status":        _status_from_pct(session_pct),
                "plan_type":     plan_name,
                "ok":            True,
            }
        except ImportError:
            self.last_error = "sqlite3 module not available"
            return None
        except Exception as e:
            self.last_error = f"Fetch error: {e}"
            return None


# ── Provider: Cursor ──────────────────────────────────────────────────────────
class CursorProvider:
    """
    Cursor uses browser session cookies. On Linux, extraction requires access to
    the browser's encrypted cookie store. The simplest cross-platform approach
    is to pass the session token via an environment variable.

    Set CURSOR_SESSION_TOKEN to the value of your WorkosCursorSessionToken
    cookie from cursor.com.
    """
    name = "cursor"
    _USAGE_URL = "https://www.cursor.com/api/usage"

    def __init__(self):
        self.available  = False
        self.last_error = None

    def check_available(self):
        if _get_api_key("CURSOR_SESSION_TOKEN", provider_id="cursor"):
            self.available = True
            return True
        self.last_error = (
            "No CURSOR_SESSION_TOKEN — set env var or save via web UI "
            "(WorkosCursorSessionToken cookie from cursor.com)."
        )
        return False

    def fetch_usage(self):
        try:
            token = _get_api_key("CURSOR_SESSION_TOKEN", provider_id="cursor")
            if not token:
                return None
            req = urllib.request.Request(
                self._USAGE_URL,
                headers={
                    "Cookie":     f"WorkosCursorSessionToken={token}",
                    "User-Agent": "Mozilla/5.0",
                    "Accept":     "application/json",
                },
            )
            with urllib.request.urlopen(req, timeout=10) as r:
                data = json.loads(r.read().decode("utf-8"))
            ind = data.get("individualUsage", {})
            # gpt-4 / named-model usage percent
            gpt4    = ind.get("gpt-4", {})
            used    = float(gpt4.get("numRequests", 0))
            limit   = float(gpt4.get("maxRequestUsage", 0)) or 1.0
            pct     = min(100.0, used / limit * 100)
            return {
                "provider":      "cursor",
                "session_pct":   round(pct, 1),
                "session_reset": 0,
                "weekly_pct":    0.0,
                "weekly_reset":  0,
                "status":        _status_from_pct(pct),
                "plan_type":     data.get("membershipType", "cursor"),
                "ok":            True,
            }
        except urllib.error.HTTPError as e:
            self.last_error = f"API error {e.code}"
            return None
        except Exception as e:
            self.last_error = f"Fetch error: {e}"
            return None


# ── Provider: Kimi Coding API (API key) ───────────────────────────────────────
class KimiProvider:
    name = "kimi"
    _API_BASE_URL = "https://api.kimi.com/coding/v1"
    _MODELS_URL = f"{_API_BASE_URL}/models"
    _METRICS_NOTE = "No usage metrics via Kimi Coding API"

    def __init__(self):
        self.available  = False
        self.last_error = None

    def check_available(self):
        if _get_api_key("KIMI_API_KEY", "MOONSHOT_API_KEY", provider_id="kimi"):
            self.available = True
            return True
        self.last_error = "No KIMI_API_KEY — set env var or save via web UI"
        return False

    def fetch_usage(self):
        try:
            key = _get_api_key("KIMI_API_KEY", "MOONSHOT_API_KEY", provider_id="kimi")
            if not key:
                return None
            req = urllib.request.Request(
                self._MODELS_URL,
                headers={"Authorization": f"Bearer {key}", "Accept": "application/json"},
            )
            with urllib.request.urlopen(req, timeout=10) as r:
                json.loads(r.read().decode("utf-8"))
            return {
                "provider":        "kimi",
                "session_pct":     0.0,
                "session_reset":   0,
                "weekly_pct":      0.0,
                "weekly_reset":    0,
                "status":          "allowed",
                "plan_type":       "api",
                "has_credits":     False,
                "metrics_available": False,
                "note":            self._METRICS_NOTE,
                "ok":              True,
            }
        except urllib.error.HTTPError as e:
            detail = _extract_http_error_message(e)
            if e.code in (401, 403):
                self.last_error = "Invalid API key"
                if detail:
                    self.last_error += f" ({detail})"
            else:
                self.last_error = f"API error {e.code}"
                if detail:
                    self.last_error += f" ({detail})"
            return None
        except Exception as e:
            self.last_error = f"Fetch error: {e}"
            return None


# ── Merge all providers into one payload ─────────────────────────────────────
def merge_provider_data(providers):
    data = {"providers": [], "timestamp": int(time.time()), "ok": False}
    has_data = False

    for provider in providers:
        if not provider.available:
            provider.check_available()

        if provider.available:
            result = provider.fetch_usage()
            if result:
                result["configured"] = True
                data["providers"].append(result)
                has_data = True
                sim   = result.get("simulated", False)
                color = Colors.YELLOW if sim else Colors.GREEN
                if result.get("metrics_available", True):
                    log(
                        f"{provider.name}: session={result.get('session_pct','N/A')}%  "
                        f"weekly={result.get('weekly_pct','N/A')}%"
                        f"{'  (SIMULATED)' if sim else ''}",
                        color,
                    )
                else:
                    note = result.get("note") or "Authenticated, metrics unavailable"
                    log(f"{provider.name}: {note}", color)
            else:
                log(f"{provider.name}: {provider.last_error}", Colors.YELLOW)
                data["providers"].append({
                    "provider":   provider.name,
                    "error":      provider.last_error or "fetch failed",
                    "configured": True,
                    "ok":         False,
                })
        else:
            log(f"{provider.name}: Not configured — {provider.last_error}", Colors.YELLOW)
            data["providers"].append({
                "provider":   provider.name,
                "error":      provider.last_error or "not configured",
                "configured": False,
                "ok":         False,
            })

    data["ok"] = has_data
    return data


# ── Main ──────────────────────────────────────────────────────────────────────
def main():
    log("=== PocketMeter Multi-Provider Daemon ===", Colors.BLUE)
    log("Providers: Claude, Codex, Gemini, Copilot, Grok, OpenAI, DeepSeek, Windsurf, Cursor, Kimi",
        Colors.BLUE)
    log(f"Poll interval: {POLL_INTERVAL}s  |  Keys file: {KEYS_FILE}", Colors.BLUE)
    log("Press Ctrl+C to stop", Colors.CYAN)
    log("", Colors.RESET)
    log("API-key providers: set env vars OR save via web UI at localhost:8080", Colors.CYAN)
    log("  OPENAI_API_KEY, DEEPSEEK_API_KEY, KIMI_API_KEY, CURSOR_SESSION_TOKEN", Colors.CYAN)
    log("", Colors.RESET)

    providers = [
        ClaudeProvider(),
        CodexProvider(),
        GeminiProvider(),
        CopilotProvider(),
        GrokProvider(),
        OpenAIProvider(),
        DeepSeekProvider(),
        WindsurfProvider(),
        CursorProvider(),
        KimiProvider(),
    ]

    for p in providers:
        p.check_available()

    backoff   = 1
    last_poll = 0
    endpoint  = None

    while True:
        if endpoint is None:
            endpoint = discover_esp_endpoint()
            if endpoint:
                loc   = endpoint.ip if endpoint.ip == endpoint.host else f"{endpoint.host} ({endpoint.ip})"
                extra = f", hostname={endpoint.hostname}" if endpoint.hostname else ""
                log(f"ESP discovered via {endpoint.source}: {loc}{extra}", Colors.GREEN)
                log(f"Dashboard: {dashboard_url(endpoint)}", Colors.GREEN)

        if not check_esp_health(endpoint):
            if endpoint:
                log(f"ESP at {endpoint.host} stopped responding; re-discovering", Colors.YELLOW)
            else:
                log(f"ESP not found, retrying in {backoff}s...", Colors.YELLOW)
            endpoint = None
            time.sleep(backoff)
            backoff  = min(backoff * 2, BACKOFF_MAX)
            continue

        backoff = 1

        now = int(time.time())
        if now - last_poll >= POLL_INTERVAL:
            log("Fetching provider data...", Colors.BLUE)
            data = merge_provider_data(providers)
            if send_to_esp(data, endpoint):
                ok_count = sum(1 for p in data["providers"] if p.get("ok"))
                log(f"✓ Data sent ({ok_count}/{len(providers)} providers active)", Colors.GREEN)
                last_poll = now
            else:
                log("✗ Failed to send data", Colors.RED)
                endpoint = None

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
