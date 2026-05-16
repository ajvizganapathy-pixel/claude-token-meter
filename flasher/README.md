# flasher/

This directory contains the web flasher for Claude Token Meter.

## Files

- `index.html` — Open this in Chrome or Edge to flash your XIAO ESP32-S3
- `firmware.bin` — Place your compiled firmware here

## How to get firmware.bin

### Option A — Build with PlatformIO (recommended)
```bash
# From project root:
pio run -e xiao_esp32s3
cp .pio/build/xiao_esp32s3/firmware.bin flasher/firmware.bin
```

### Option B — Download from GitHub Releases
Download the latest `firmware.bin` from the Releases page and place it here.

### Option C — GitHub Actions
Push a tag (e.g. `v1.0.0`) — GitHub Actions builds and attaches firmware.bin
to the release automatically.

## Flash instructions

1. Open `index.html` in **Google Chrome** or **Microsoft Edge** (version 89+)
2. Enter bootloader mode on XIAO ESP32-S3:
   - Hold **BOOT** button
   - Press and release **RESET** button
   - Release **BOOT** button
3. Select `firmware.bin` in the flasher
4. Click **Connect** → select your serial port
5. Click **Flash Firmware**
6. Wait for completion (~30 seconds)

The device will auto-reboot and create the `ClaudeTokenMeter` WiFi AP.
