# Release Notes — v1.1.0

**Release date:** 2026-05-16
**Target board:** Seeed XIAO ESP32-S3

## Highlights

- First production release of **Claude Token Meter** — a real-time OLED desk display for Claude API token usage.
- Browser-based **Web Flasher** (Chrome/Edge, WebSerial) — no drivers, no CLI.
- **ArduinoOTA** wireless firmware updates over WiFi.
- **WiFiManager** captive-portal first-boot setup at SSID `ClaudeTokenMeter`.
- **mDNS** hostname `claude-meter.local` and built-in web dashboard.
- **REST API** (`/api/status`, `/api/update`, `/api/config`) with absolute and increment modes.
- **MAX98357 I2S audio alerts** at 80 / 90 / 100 % of the weekly token limit.
- **NVS-persistent stats**, NTP-synced daily and weekly resets.
- **GitHub Actions CI** builds firmware and attaches the `.bin` to tagged releases.

## Included artifacts (after CI build)

| File | Purpose |
|---|---|
| `firmware.bin` | Merged image — flash at offset `0x0` via the Web Flasher |
| `bootloader.bin` | Optional — separate bootloader for low-level flashing (offset `0x0`) |
| `partitions.bin` | Optional — partition table (offset `0x8000`) |
| `manifest.json` | esp-web-tools manifest (optional alternative flasher) |

> The Web Flasher in `flasher/index.html` uses **esptool-js** and only needs the merged `firmware.bin`.

## Known limitations

- Web Flasher requires a Chromium-based browser with WebSerial (Chrome ≥ 89, Edge ≥ 89). Firefox / Safari are not supported.
- OTA password is hard-coded to `ctmeter2024` — **change it before deploying to production**.
- The captive-portal AP is open (unsecured) during first-boot WiFi setup.

## Upgrade

OTA from any prior 1.x build:

```
pio run -t upload -e xiao_esp32s3 --upload-port claude-meter.local
```

Or open `flasher/index.html` and flash over USB in bootloader mode.
