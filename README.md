# PocketMeter

PocketMeter is a desk-side usage display for Claude Code built around a [Waveshare ESP32-S3-Touch-AMOLED-2.16](https://www.waveshare.com/esp32-s3-touch-amoled-2.16.htm?&aff_id=149786). The ESP32 firmware connects to WiFi, a small Python daemon running on your computer polls provider usage over HTTP, and the built-in web panel shows device + provider status in the browser.

Today the flow is:

- **ESP32 firmware** on the Waveshare board
- **Python daemon on your PC** posting usage to the device over HTTP
- **Web panel** served directly by the ESP32
- **Claude** works now if `~/.claude/.credentials.json` exists
- **Codex** shows as **Not configured** until you run `codex login`
- **BLE is still used only for HID button shortcuts**, not for usage transport

|              Usage meter              |              Clawd animation screen              |
| :-----------------------------------: | :----------------------------------------------: |
| ![Usage meter](assets/demo.jpeg) | ![Clawd animation screen](assets/demo.gif) |

The Clawd animations come from [claudepix](https://claudepix.vercel.app), [@amaanbuilds](https://x.com/amaanbuilds)'s library of pixel-art Clawd sprites.

## Hardware

- [Waveshare ESP32-S3-Touch-AMOLED-2.16](https://www.waveshare.com/esp32-s3-touch-amoled-2.16.htm?&aff_id=149786)
- ESP32-S3R8, 2.16" 480×480 AMOLED, CST9220 touch, AXP2101 PMU, QMI8658 IMU
- USB-C cable for flashing and serial logs
- Optional 3.7V Li-Po battery

## Project pieces

| Piece | Runs where | What it does |
| --- | --- | --- |
| Firmware | ESP32 | Connects to WiFi, renders the UI, receives usage via `POST /api/usage`, serves the web panel |
| Python daemon | Your computer | Reads Claude/Codex auth state, polls provider usage, pushes JSON to the ESP32 |
| Web panel | ESP32 | Shows WiFi status, latest provider data, and device uptime at `http://<esp-ip>/` |

## Quick start

### 1) Configure WiFi in the firmware

Edit `firmware/src/config.h`:

```cpp
#define WIFI_SSID "TuRedWiFi"
#define WIFI_PASSWORD "TuPassword"
```

### 2) Build and flash the firmware

Linux:

```bash
pio run -d firmware
pio run -d firmware -t upload --upload-port /dev/ttyACM0
```

Or with the helper script:

```bash
./flash.sh /dev/ttyACM0
```

macOS:

```bash
./flash-mac.sh
# o bien
./flash-mac.sh /dev/cu.usbmodem1101
```

### 3) Find the ESP32 IP

Open a serial monitor after boot:

```bash
pio device monitor -p /dev/ttyACM0 -b 115200
```

Look for the WiFi/IP line and save that IP locally for the daemon:

```bash
mkdir -p ~/.config/claude-usage-monitor
printf '192.168.1.169\n' > ~/.config/claude-usage-monitor/esp-ip
```

### 4) Start the Python daemon

From the repo root:

```bash
python3 daemon/clawdmeter-daemon.py
```

What to expect:

- **Claude** should start reporting real usage if you are already signed in via Claude Code.
- **Codex** will appear as **Not configured** until you run:

```bash
codex login
```

Then restart the daemon:

```bash
python3 daemon/clawdmeter-daemon.py
```

### 5) Open the web panel

```text
http://192.168.1.169/
```

Useful API endpoints:

- `GET /api/health`
- `GET /api/status`
- `POST /api/usage`

## How it works now

1. The ESP32 joins your WiFi and starts an HTTP server.
2. The Python daemon reads `~/.claude/.credentials.json` for Claude.
3. If present, it also reads `~/.codex/auth.json` for Codex.
4. It polls provider usage and sends a combined JSON payload to `http://<esp-ip>/api/usage`.
5. The firmware updates the on-device UI and the web panel.
6. The side buttons still act as BLE HID shortcuts for Space and Shift+Tab.

## Screens

The device boots into the splash. The middle button cycles through the usage/network views; on the splash it cycles animations instead.

|              Splash               |              Usage              |                 Network                 |
| :-------------------------------: | :-----------------------------: | :-------------------------------------: |
| ![Splash](screenshots/splash.png) | ![Usage](screenshots/usage.png) | ![Network](screenshots/bluetooth.png) |
|   Splash; touch-toggle anytime    | Session and weekly utilization  |  WiFi state, IP, and connection status  |

## Physical buttons

| Button | GPIO | Function |
| --- | --- | --- |
| Left | GPIO 0 | Hold to send Space |
| Middle (PWR) | AXP2101 PKEY | Cycle screens; on splash, cycle animations |
| Right | GPIO 18 | Send Shift+Tab |

## Manual test

You can send a payload without the daemon:

```bash
curl -X POST http://192.168.1.169/api/usage \
  -H "Content-Type: application/json" \
  -d '{"s":45,"sr":120,"w":28,"wr":7200,"st":"allowed","ok":true}'

curl http://192.168.1.169/api/status
curl http://192.168.1.169/api/health
```

## Notes

- The repo folder can still be named `Clawdmeter`; the product branding is now **PocketMeter**.
- Older BLE data-daemon scripts are still in the repo for reference, but the current main path is **WiFi + HTTP + `daemon/clawdmeter-daemon.py`**.
- `tools/`, fonts, animations, and assets remain as they were unless you want to rework the visual identity further.

## Credits

- Pixel-art Clawd animation by [@amaanbuilds](https://x.com/amaanbuilds), sourced from [claudepix.vercel.app](https://claudepix.vercel.app)
- Lucide icon set ([lucide.dev](https://lucide.dev), MIT)
- Anthropic brand fonts (Tiempos Text, Styrene B)

## Licensing warning

This repo still includes proprietary/copyrighted visual assets and fonts tied to Anthropic branding, including Clawd-related artwork. Review that before publishing or relicensing the project.
