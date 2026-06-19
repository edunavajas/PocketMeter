# PocketMeter — Web UI Setup

> **Changed from README**: The web panel is no longer served by the ESP32.  
> It now runs on your PC at `http://localhost:8080`.

## Quick start

```bash
cd daemon
./web-server.sh          # foreground
./web-server.sh --daemon # background (logs → daemon/web-server.log)
```

Open **http://localhost:8080** in your browser.

## Architecture

```
PC (localhost:8080)
├── web-server.py     Python HTTP server — serves UI, proxies ESP32, stores API keys
├── index.html        Single-page dashboard (live polling, no page blink)
└── /api/petdex/search → HTTPS direct to petdex.crafter.run

       │ HTTP proxy
       ▼

ESP32 (192.168.1.180 / pocketmeter.local)
├── /api/status   → WiFi, providers, config, pets
├── /api/config   → POST — mascot toggles, active display, pet assignments
└── /api/usage    → POST — daemon posts provider usage data here
```

## Environment variables for web-server.py

| Variable | Default | Description |
|---|---|---|
| `ESP32_HOST` | `192.168.1.180` | ESP32 IP or hostname |
| `ESP32_PORT` | `80` | ESP32 HTTP port |
| `POCKETMETER_WEB_PORT` | `8080` | Local port for the web server |

## Running the daemon

```bash
python3 daemon/pocketmeter-daemon.py
```

The daemon discovers the ESP32 automatically (mDNS → subnet scan → cached IP file).

## Providers (10 total)

### OAuth providers — configure via CLI

The web UI shows the exact command to run. After running it, restart the daemon.

| Provider | Command |
|---|---|
| Claude | `claude login` |
| Codex | `codex login` |
| Gemini | `gemini` (first run triggers browser OAuth) |
| Copilot | `gh auth login --scopes copilot` |
| Grok | `grok` |

### API key providers — configure via web UI or env var

Open **http://localhost:8080**, find the provider card, paste the key, click **Save**.  
Keys are stored in `~/.config/pocketmeter/api-keys.json` and read on the next daemon poll.

Alternatively, set the environment variable before starting the daemon:

| Provider | Env var | Notes |
|---|---|---|
| OpenAI | `OPENAI_API_KEY` | Admin key needed for org cost API |
| DeepSeek | `DEEPSEEK_API_KEY` | Shows balance |
| Kimi (Moonshot) | `KIMI_API_KEY` | platform.moonshot.cn |
| Cursor | `CURSOR_SESSION_TOKEN` | WorkosCursorSessionToken cookie from cursor.com |

### Windsurf

Install [Windsurf](https://windsurf.com). The daemon reads plan data from its local SQLite database automatically — no configuration needed.

## systemd auto-start

```bash
cp daemon/pocketmeter-web.service ~/.config/systemd/user/
# Edit ExecStart path if needed
systemctl --user enable --now pocketmeter-web
```

## What changed from the old setup

| Before | Now |
|---|---|
| Web panel served by ESP32 | Web panel served by PC at localhost:8080 |
| ESP32 needed HTTPS for Petdex | PC makes HTTPS calls directly |
| Auto-refresh every 30s (page blink) | Smooth background polling, DOM updates only |
| Only Claude + Codex in UI | 10 providers: + Gemini, Copilot, Grok, OpenAI, DeepSeek, Windsurf, Cursor, Kimi |
| API keys only via env vars | API keys saveable via web UI |
| Pet assignment broken for non-Claude providers | Pet assignment works for all providers |
