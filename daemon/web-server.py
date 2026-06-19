#!/usr/bin/env python3
"""PocketMeter Web Server — serves the UI and proxies requests to the ESP32."""

import http.server
import io
import json
import os
import subprocess
import sys
import urllib.parse
import urllib.request
import urllib.error
from pathlib import Path

try:
    from PIL import Image
    PILLOW_AVAILABLE = True
except ImportError:
    PILLOW_AVAILABLE = False
    log_early = lambda m: print(f"[web-server] {m}", file=sys.stderr, flush=True)
    log_early("WARNING: Pillow not found — /api/petdex/convert will be unavailable. Install with: pip install pillow")

ESP_HOST_OVERRIDE   = "POCKETMETER_ESP_HOST"
DEFAULT_ESP_HOST    = "192.168.1.180"
ESP32_PORT          = int(os.environ.get("ESP32_PORT", "80"))
PETDEX_URL          = "https://petdex.dev/api/pets/search"
PORT                = int(os.environ.get("POCKETMETER_WEB_PORT", "8080"))
ESP_IP_FILE         = Path.home() / ".config" / "pocketmeter" / "esp-ip"
LEGACY_ESP_IP_FILE  = Path.home() / ".config" / "claude-usage-monitor" / "esp-ip"
KEYS_FILE           = Path.home() / ".config" / "pocketmeter" / "api-keys.json"
SPLASH_CONFIGS_FILE = Path.home() / ".config" / "pocketmeter" / "splash-configs.json"
ANIM_DIR            = Path(__file__).parent.parent / "tools" / "claudepix_data"
KIMI_API_BASE_URL   = "https://api.kimi.com/coding/v1"
KIMI_VALIDATE_URL   = f"{KIMI_API_BASE_URL}/models"


def log(msg):
    print(f"[web-server] {msg}", file=sys.stderr, flush=True)


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


def normalize_secret(value):
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


def extract_http_error_message(error):
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


def load_configured_host():
    for path in (ESP_IP_FILE, LEGACY_ESP_IP_FILE):
        try:
            if path.exists():
                host = normalize_host(path.read_text())
                if host:
                    return host, str(path)
        except OSError:
            pass
    return None, None


def resolve_esp_host():
    for env_name in (ESP_HOST_OVERRIDE, "ESP32_HOST"):
        host = normalize_host(os.environ.get(env_name))
        if host:
            return host, f"env:{env_name}"
    host, source = load_configured_host()
    if host:
        return host, source
    return DEFAULT_ESP_HOST, "default"


def current_esp_base():
    host, _source = resolve_esp_host()
    return f"http://{host}:{ESP32_PORT}"


# ── ESP32 proxy (curl-based for robustness) ──────────────────────────────────
def proxy(method, path, body=None, content_type=None):
    url = current_esp_base() + path
    cmd = ["curl", "-s", "-m", "10", "-X", method, url]
    if body:
        cmd += ["-d", body]
        ct = content_type or "application/json"
        cmd += ["-H", f"Content-Type: {ct}"]
    cmd += ["-H", "Accept: application/json"]
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=15)
        if result.returncode != 0:
            return 502, json.dumps({"error": f"ESP32 unreachable: {url}"}), "application/json"
        try:
            json.loads(result.stdout)
            return 200, result.stdout, "application/json"
        except json.JSONDecodeError:
            return 200, result.stdout, "text/plain"
    except Exception as e:
        return 502, json.dumps({"error": str(e)}), "application/json"


# ── Petdex (direct HTTPS from PC) ────────────────────────────────────────────
def fetch_animations():
    """Read claudepix_data JSONs and return full animation data for web canvas rendering."""
    index_path = ANIM_DIR / "_index.json"
    if not index_path.exists():
        return {"animations": []}
    try:
        index = json.loads(index_path.read_text())
        animations = []
        for meta in index:
            stem = meta["filename"].replace(".html", "")
            data_path = ANIM_DIR / f"{stem}.json"
            if not data_path.exists():
                continue
            data = json.loads(data_path.read_text())
            palette = []
            for color in data.get("palette", []):
                if not color or color == "transparent":
                    palette.append("#000000")
                else:
                    palette.append(color)
            # Pad palette to 10 entries
            while len(palette) < 10:
                palette.append("#000000")
            frames = []
            holds = []
            for f in data.get("frames", []):
                flat = []
                for row in f["grid"]:
                    flat.extend(row)
                frames.append(flat)
                holds.append(f.get("hold", 100))
            animations.append({
                "name":     data.get("name", meta.get("name", "")),
                "category": data.get("category", meta.get("category", "")),
                "palette":  palette,
                "frames":   frames,
                "holds":    holds,
            })
        return {"animations": animations}
    except Exception as e:
        log(f"animations error: {e}")
        return {"animations": []}


def fetch_petdex(query, limit, cursor):
    params = {"limit": str(limit)}
    if query:
        params["q"] = query
    if cursor:
        params["cursor"] = str(cursor)
    url = PETDEX_URL + "?" + urllib.parse.urlencode(params)
    cmd = ["curl", "-sS", "-L", "--fail", "-m", "15", "-H", "Accept: application/json", url]
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=20)
        if result.returncode != 0:
            log(f"Petdex curl failed: {result.stderr}")
            return {"error": "Petdex search failed", "query": query or "", "pets": [], "total": 0}
        upstream = json.loads(result.stdout)
        pets = []
        for pet in upstream.get("pets", [])[:limit]:
            pets.append({
                "assigned":         True,
                "id":               pet.get("id", ""),
                "slug":             pet.get("slug", ""),
                "displayName":      pet.get("displayName", ""),
                "description":      pet.get("description", ""),
                "spritesheetPath":  pet.get("spritesheetPath", ""),
                "petJsonPath":      pet.get("petJsonPath", ""),
                "dominantColor":    pet.get("dominantColor", ""),
                "previewImagePath": pet.get("spritesheetPath", ""),
                "vibes":            pet.get("vibes", [])[:3],
            })
        return {
            "query":      query or "",
            "total":      upstream.get("total", 0),
            "nextCursor": upstream.get("nextCursor", 0),
            "pets":       pets,
        }
    except Exception as e:
        log(f"Petdex error: {e}")
        return {"error": "Petdex search failed", "query": query or "", "pets": [], "total": 0}


# ── PetDex → claudepix conversion ────────────────────────────────────────────
def _to_rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def convert_petdex_to_claudepix(spritesheet_url, row, frame_count, name, hold_ms=150):
    """Download a PetDex WebP spritesheet and convert one animation row to claudepix format.

    Returns a dict with palette (list of 10 RGB565 ints), frames (flat digit string),
    frame_count, and holds (list of hold times in ms).
    """
    if not PILLOW_AVAILABLE:
        raise RuntimeError("Pillow not installed")

    req = urllib.request.Request(spritesheet_url, headers={"User-Agent": "PocketMeter/1.0"})
    with urllib.request.urlopen(req, timeout=15) as resp:
        data = resp.read()

    sheet = Image.open(io.BytesIO(data)).convert("RGBA")
    CELL_W, CELL_H = 192, 208

    # Extract and resize each frame to 20×20, composited over black
    frames_rgb = []
    for col in range(frame_count):
        crop = sheet.crop((col * CELL_W, row * CELL_H, (col + 1) * CELL_W, (row + 1) * CELL_H))
        small = crop.resize((20, 20), Image.LANCZOS)
        bg = Image.new("RGBA", (20, 20), (0, 0, 0, 255))
        bg.paste(small, mask=small.split()[3])
        frames_rgb.append(bg.convert("RGB"))

    # Build combined image and quantize to 10 colors (shared palette)
    combined = Image.new("RGB", (20 * frame_count, 20))
    for i, f in enumerate(frames_rgb):
        combined.paste(f, (i * 20, 0))
    q_combined = combined.quantize(colors=10)
    pal_flat = q_combined.getpalette()[:30]
    palette_rgb = [(pal_flat[i * 3], pal_flat[i * 3 + 1], pal_flat[i * 3 + 2]) for i in range(10)]
    pal_565 = [_to_rgb565(r, g, b) for r, g, b in palette_rgb]

    # Map each pixel to nearest palette index
    def nearest(r, g, b):
        best_i, best_d = 0, float("inf")
        for i, (pr, pg, pb) in enumerate(palette_rgb):
            d = (r - pr) ** 2 + (g - pg) ** 2 + (b - pb) ** 2
            if d < best_d:
                best_d, best_i = d, i
        return best_i

    frames_str = ""
    for f in frames_rgb:
        for r, g, b in f.getdata():
            frames_str += str(nearest(r, g, b))

    return {
        "name":        name,
        "category":    "petdex",
        "palette":     pal_565,
        "frames":      frames_str,
        "frame_count": frame_count,
        "holds":       [hold_ms] * frame_count,
    }


# ── Splash configs (per-provider splash assignments) ─────────────────────────
def load_splash_configs():
    try:
        if SPLASH_CONFIGS_FILE.exists():
            return json.loads(SPLASH_CONFIGS_FILE.read_text())
    except Exception:
        pass
    return {}


def save_splash_configs(data):
    SPLASH_CONFIGS_FILE.parent.mkdir(parents=True, exist_ok=True)
    SPLASH_CONFIGS_FILE.write_text(json.dumps(data, indent=2))


# ── API key storage ───────────────────────────────────────────────────────────
def load_keys():
    try:
        if KEYS_FILE.exists():
            return json.loads(KEYS_FILE.read_text())
    except Exception:
        pass
    return {}


def save_key(provider, key):
    key = normalize_secret(key)
    if not key:
        raise ValueError("key is required")
    KEYS_FILE.parent.mkdir(parents=True, exist_ok=True)
    keys = load_keys()
    keys[provider] = key
    KEYS_FILE.write_text(json.dumps(keys, indent=2))


def clear_key(provider):
    provider = str(provider or "").strip()
    if not provider:
        raise ValueError("provider is required")
    keys = load_keys()
    removed = provider in keys
    if removed:
        keys.pop(provider, None)
        KEYS_FILE.parent.mkdir(parents=True, exist_ok=True)
        if keys:
            KEYS_FILE.write_text(json.dumps(keys, indent=2))
        elif KEYS_FILE.exists():
            KEYS_FILE.unlink()
    return removed


def validate_provider_key(provider, key):
    key = normalize_secret(key)
    if not key:
        return False, "key is required"
    if provider != "kimi":
        return True, ""
    req = urllib.request.Request(
        KIMI_VALIDATE_URL,
        headers={"Authorization": f"Bearer {key}", "Accept": "application/json"},
    )
    try:
        with urllib.request.urlopen(req, timeout=10):
            return True, ""
    except urllib.error.HTTPError as e:
        detail = extract_http_error_message(e)
        if e.code in (401, 403):
            return False, f"Kimi rejected this API key: {detail or 'invalid authentication'}"
        return False, f"Kimi API error {e.code}: {detail or 'validation failed'}"
    except Exception as e:
        return False, f"Could not validate key: {e}"


# ── Load static HTML ──────────────────────────────────────────────────────────
INDEX_PATH = os.path.join(os.path.dirname(__file__), "index.html")
try:
    with open(INDEX_PATH) as f:
        INDEX_HTML = f.read()
except Exception as e:
    log(f"Failed to load {INDEX_PATH}: {e}")
    INDEX_HTML = f"<html><body><h1>Error loading index.html</h1><p>{e}</p></body></html>"


# ── HTTP handler ──────────────────────────────────────────────────────────────
class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        log(f"{self.client_address[0]} — {fmt % args}")

    def _json(self, data, status=200):
        b = json.dumps(data) if not isinstance(data, str) else data
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(b.encode())

    def _html(self, body, status=200):
        self.send_response(status)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.end_headers()
        self.wfile.write(body.encode())

    def _proxy_response(self, status, body, ct):
        self.send_response(status)
        self.send_header("Content-Type", ct)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body.encode() if isinstance(body, str) else body)

    def do_GET(self):
        p  = urllib.parse.urlparse(self.path)
        path = p.path
        q  = urllib.parse.parse_qs(p.query)

        if path == "/":
            self._html(INDEX_HTML)
        elif path == "/api/status":
            status, body, ct = proxy("GET", "/api/status")
            self._proxy_response(status, body, ct)
        elif path == "/api/health":
            self._json({"ok": True})
        elif path == "/api/petdex/search":
            result = fetch_petdex(
                q.get("q", [""])[0],
                int(q.get("limit", ["6"])[0]),
                int(q.get("cursor", ["0"])[0]),
            )
            self._json(result)
        elif path == "/api/auth/keys":
            # Return configured providers (without exposing actual keys)
            keys = load_keys()
            self._json({"configured": list(keys.keys())})
        elif path == "/api/animations":
            self._json(fetch_animations())
        elif path == "/api/splash":
            status, body, ct = proxy("GET", "/api/splash")
            self._proxy_response(status, body, ct)
        elif path == "/api/splash-configs":
            self._json(load_splash_configs())
        else:
            self._json({"error": "Not found"}, 404)

    def do_POST(self):
        p    = urllib.parse.urlparse(self.path)
        path = p.path
        cl   = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(cl).decode() if cl else ""

        if path == "/api/config":
            status, resp, ct = proxy(
                "POST", "/api/config", body=body,
                content_type=self.headers.get("Content-Type", "application/json"),
            )
            self._proxy_response(status, resp, ct)

        elif path == "/api/usage":
            status, resp, ct = proxy(
                "POST", "/api/usage", body=body,
                content_type=self.headers.get("Content-Type", "application/json"),
            )
            self._proxy_response(status, resp, ct)

        elif path == "/api/splash":
            status, resp, ct = proxy(
                "POST", "/api/splash", body=body,
                content_type=self.headers.get("Content-Type", "application/json"),
            )
            self._proxy_response(status, resp, ct)

        elif path == "/api/petdex/convert":
            try:
                data       = json.loads(body)
                url        = data.get("url", "")
                row        = int(data.get("row", 0))
                frames     = int(data.get("frames", 6))
                name       = data.get("name", "custom")
                hold_ms    = int(data.get("hold_ms", 150))
                if not url:
                    self._json({"error": "url is required"}, 400)
                    return
                result = convert_petdex_to_claudepix(url, row, frames, name, hold_ms)
                self._json(result)
            except RuntimeError as e:
                self._json({"error": str(e)}, 503)
            except Exception as e:
                log(f"petdex/convert error: {e}")
                self._json({"error": str(e)}, 500)

        elif path == "/api/anim/custom":
            status, resp, ct = proxy(
                "POST", "/api/anim/custom", body=body,
                content_type=self.headers.get("Content-Type", "application/json"),
            )
            self._proxy_response(status, resp, ct)

        elif path == "/api/splash-configs":
            try:
                data = json.loads(body)
                save_splash_configs(data)
                self._json({"ok": True})
            except Exception as e:
                self._json({"error": str(e)}, 500)

        elif path == "/api/auth/apikey":
            try:
                data     = json.loads(body)
                provider = str(data.get("provider", "")).strip()
                key      = normalize_secret(data.get("key", ""))
                if not provider or not key:
                    self._json({"error": "provider and key are required"}, 400)
                    return
                ok, message = validate_provider_key(provider, key)
                if not ok:
                    self._json({"error": message}, 400)
                    return
                save_key(provider, key)
                log(f"Saved API key for {provider}")
                self._json({"ok": True, "provider": provider})
            except Exception as e:
                self._json({"error": str(e)}, 500)

        else:
            self._json({"error": "Not found"}, 404)

    def do_OPTIONS(self):
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()

    def do_DELETE(self):
        p = urllib.parse.urlparse(self.path)
        path = p.path
        q = urllib.parse.parse_qs(p.query)

        if path == "/api/auth/apikey":
            try:
                provider = str(q.get("provider", [""])[0]).strip()
                if not provider:
                    self._json({"error": "provider is required"}, 400)
                    return
                removed = clear_key(provider)
                log(f"Cleared API key for {provider}: {'removed' if removed else 'not present'}")
                self._json({"ok": True, "provider": provider, "removed": removed})
            except Exception as e:
                self._json({"error": str(e)}, 500)
        else:
            self._json({"error": "Not found"}, 404)


if __name__ == "__main__":
    esp_host, esp_source = resolve_esp_host()
    log(f"ESP32:  http://{esp_host}:{ESP32_PORT} ({esp_source})")
    log(f"Petdex: {PETDEX_URL}")
    log(f"Keys:   {KEYS_FILE}")
    log(f"Port:   {PORT}")
    server = http.server.HTTPServer(("0.0.0.0", PORT), Handler)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        log("Shutdown")
        server.shutdown()
