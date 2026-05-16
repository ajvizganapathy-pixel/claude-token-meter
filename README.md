# 🤖 Claude Token Meter

Real-time Claude API token usage monitor — XIAO ESP32-S3 desk display with OLED, audio alerts, OTA updates, and a one-click browser web flasher.

![License](https://img.shields.io/badge/license-MIT-purple)
![Platform](https://img.shields.io/badge/board-XIAO_ESP32--S3-blue)
![Framework](https://img.shields.io/badge/framework-Arduino-teal)
![Version](https://img.shields.io/badge/firmware-v1.1.0-purple)
![Build](https://github.com/ajvizganapathy-pixel/claude-token-meter/actions/workflows/build.yml/badge.svg)

## Overview

**Claude Token Meter** turns a `$10` XIAO ESP32-S3 + OLED into a live dashboard
for Anthropic Claude API consumption. A companion Python script reads Claude
usage data and POSTs it over WiFi to the device every 30 seconds. The OLED
rotates through four pages of stats; audio alerts fire as you approach the
weekly token cap. Firmware is flashed straight from your browser — no drivers,
no Arduino IDE — and can update wirelessly over OTA after first boot.

---

## Features

- **4-page rotating OLED dashboard** — weekly overview, token breakdown, daily/session stats, system info
- **Audio alerts** via MAX98357 I2S amplifier — 80%, 90%, 100% of weekly limit
- **Open WiFi AP** on first boot → captive portal to configure WiFi + settings
- **mDNS** — access at `http://claude-meter.local`
- **Mini web dashboard** served directly from the ESP32
- **REST API** — companion script POSTs token data every 30 seconds
- **Persistent stats** saved to NVS flash
- **OTA firmware updates** — update wirelessly without USB (ArduinoOTA)
- **Auto daily reset** at midnight UTC (NTP-synced)
- **Auto weekly reset** every 7 days (NTP-synced)
- **Factory reset** — hold BOOT button 5 seconds to clear WiFi credentials
- **Browser-based web flasher** — no drivers needed (Chrome/Edge only)
- **GitHub Actions** CI — auto-builds firmware and attaches to releases

---

## Hardware

| Component | Part |
|---|---|
| MCU | Seeed XIAO ESP32-S3 |
| Display | SSD1306 0.96" OLED (128×64, I2C) |
| Audio | MAX98357 I2S 3W Mono Amplifier |
| Speaker | 4Ω or 8Ω, 1–3W |

---

## Wiring

### OLED SSD1306
| OLED | XIAO ESP32-S3 | GPIO |
|------|--------------|------|
| VCC  | 3V3          | —    |
| GND  | GND          | —    |
| SDA  | D4           | GPIO5 |
| SCL  | D5           | GPIO6 |

### MAX98357 Amplifier
| MAX98357 | XIAO ESP32-S3 | GPIO |
|----------|--------------|------|
| VIN      | 3V3          | —    |
| GND      | GND          | —    |
| BCLK     | D8           | GPIO7 |
| LRC      | D9           | GPIO8 |
| DIN      | D10          | GPIO9 |
| SD       | Leave unconnected | — |
| GAIN     | GND (9 dB) or float (12 dB) | — |

---

## Quick Start

### 1 — Build firmware (VS Code + PlatformIO)

```bash
# Open folder in VS Code, then in terminal:
pio run -e xiao_esp32s3
```

Output: `.pio/build/xiao_esp32s3/firmware.bin`

Copy it to `flasher/firmware.bin`.

### 2 — Flash with the Web Flasher

The Web Flasher (`flasher/index.html`) is a self-contained page powered by
[esptool-js](https://github.com/espressif/esptool-js). It works in any
Chromium-based browser with WebSerial (Chrome ≥ 89, Edge ≥ 89, Opera, Brave).
No drivers, no CLI, no Python.

1. Plug the XIAO ESP32-S3 into USB.
2. Put XIAO in **bootloader mode**: hold **BOOT** → press **RESET** → release **BOOT**.
3. Open `flasher/index.html` locally **or** the GitHub Pages URL (see below).
4. Click **Connect** → pick the serial port.
5. Select your `firmware.bin` (download from the GitHub Releases page).
6. Click **Flash Firmware**. Wait ~30 s.
7. Press **RESET** once when flashing completes.

> **Browser support**: Firefox and Safari do not implement WebSerial and are
> not supported. Use Chrome or Edge.

### 3 — Configure WiFi

After flashing, the device creates an open AP: **`ClaudeTokenMeter`**

Connect your phone to it. A captive portal opens automatically (or go to `192.168.4.1`).

Enter:
- Your home WiFi SSID + password
- Optional: Anthropic API key
- Weekly token limit (default: 1,000,000)

### 4 — Run the companion script

```bash
cd companion
pip install -r requirements.txt

# Auto-discover device and start monitoring
python claude_monitor.py --discover

# Or specify IP directly
python claude_monitor.py --ip 192.168.1.42

# Set a custom weekly limit
python claude_monitor.py --ip claude-meter.local --limit 500000

# Test with simulated data
python claude_monitor.py --ip claude-meter.local --simulate
```

---

## REST API

The ESP32 exposes a small HTTP API at its IP (or `claude-meter.local`):

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/` | GET | Web dashboard (HTML) |
| `/api/status` | GET | Current stats as JSON |
| `/api/update` | POST | Push new token counts |
| `/api/config` | POST | Update settings (limit, reset) |

### POST /api/update

```json
{
  "weeklyTotal":  247832,
  "dailyTotal":   18456,
  "sessionTotal": 4210,
  "inputTokens":  180000,
  "outputTokens": 60000,
  "cacheRead":    7000,
  "cacheWrite":   832,
  "weeklyLimit":  1000000,
  "costUSD":      2.4512,
  "model":        "claude-sonnet-4-20250514"
}
```

Or use **increment mode** (adds to running totals):

```json
{
  "deltaTokens": 1234,
  "deltaInput":  1000,
  "deltaOutput": 234,
  "deltaCost":   0.0123,
  "model":       "claude-sonnet-4-20250514"
}
```

### POST /api/config

```json
{
  "weeklyLimit": 500000,
  "resetAlerts": true,
  "resetStats":  false
}
```

---

## OLED Display Pages

The display rotates through 4 pages every 6 seconds:

| Page | Content |
|------|---------|
| 0 | Weekly tokens used + progress bar + % of limit |
| 1 | Token breakdown: input / output / cache read / cache write |
| 2 | Daily total + session total + estimated cost |
| 3 | IP address, WiFi SSID, RSSI, mDNS URL, FW version |

---

## Audio Alert Patterns

| Trigger | Pattern |
|---------|---------|
| Startup | Ascending triad chime |
| WiFi connected | Two-note chime |
| 80% of limit | 2× 880 Hz beep |
| 90% of limit | 3× rapid 1046 Hz beep |
| 100% of limit | 6× alternating 1318/987 Hz alarm |
| Data received | Single soft click (subtle) |

---

## Libraries Required

Install via PlatformIO (auto) or Arduino Library Manager:

- `tzapu/WiFiManager` ≥ 2.0.17
- `adafruit/Adafruit SSD1306` ≥ 2.5.10
- `adafruit/Adafruit GFX Library` ≥ 1.11.9
- `bblanchon/ArduinoJson` ≥ 7.1.0

---

## OTA Firmware Updates

Once on WiFi, update firmware **wirelessly** — no USB needed:

**PlatformIO:**
```ini
; in platformio.ini:
upload_protocol = espota
upload_port     = claude-meter.local
upload_flags    = --auth=ctmeter2024
```
Then: `pio run -t upload`

**Arduino IDE:** Tools → Port → `claude-meter (claude-meter.local)` → Upload → password `ctmeter2024`

> ⚠️ Change `#define OTA_PASSWORD "ctmeter2024"` before deploying.

---

## Factory Reset

Hold **BOOT** button **5 seconds** → clears WiFi credentials, reboots to AP mode.  
Stats are preserved. To clear stats too: `POST /api/config` `{"resetStats": true}`

---

## Project Structure

```
ClaudeTokenMeter/
├── firmware/
│   └── ClaudeTokenMeter/
│       └── ClaudeTokenMeter.ino   ← Main firmware (v1.1.0)
├── companion/
│   ├── claude_monitor.py          ← Python companion script
│   ├── requirements.txt
│   ├── setup.sh                   ← Linux/macOS one-click setup
│   └── setup.bat                  ← Windows one-click setup
├── flasher/
│   ├── index.html                 ← Web flasher (open in Chrome)
│   └── firmware.bin               ← Auto-copied here after build
├── scripts/
│   └── copy_firmware.py           ← PlatformIO post-build script
├── docs/
│   └── wiring.md                  ← Wiring diagram + OTA guide
├── .github/workflows/
│   └── build.yml                  ← CI build + GitHub release
├── .gitignore
├── platformio.ini
└── README.md
```

---

## Screenshots

> _Add real screenshots / photos to `docs/screenshots/` and replace the placeholders below._

| | |
|---|---|
| ![Web Flasher](docs/screenshots/web-flasher.png) | ![OLED weekly view](docs/screenshots/oled-weekly.png) |
| **Web Flasher** — Chrome/Edge, drag a `.bin`, click Flash. | **OLED weekly view** — live token usage + progress bar. |
| ![Captive portal](docs/screenshots/captive-portal.png) | ![Built hardware](docs/screenshots/hardware.jpg) |
| **First-boot captive portal** — WiFiManager setup. | **Assembled unit** — XIAO + SSD1306 + MAX98357. |

---

## License

MIT — see [LICENSE](LICENSE).

## Credits

- [esptool-js](https://github.com/espressif/esptool-js) — browser-side ESP32 flasher (Espressif Systems).
- [WiFiManager](https://github.com/tzapu/WiFiManager) by **tzapu** — captive-portal WiFi onboarding.
- [Adafruit SSD1306](https://github.com/adafruit/Adafruit_SSD1306) + [GFX](https://github.com/adafruit/Adafruit-GFX-Library) — display libraries by **Adafruit**.
- [ArduinoJson](https://arduinojson.org/) by **Benoît Blanchon** — JSON parser/serializer.
- [Seeed Studio](https://www.seeedstudio.com/) — XIAO ESP32-S3 hardware.
- Anthropic **Claude** — for being worth metering. 🟣

Built with ☕ and a $10 ESP32 by the Claude Token Meter contributors.
