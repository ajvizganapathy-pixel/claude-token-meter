## Claude Token Meter v1.1.0

First production firmware release for the Seeed XIAO ESP32-S3 + SSD1306 OLED + MAX98357 I2S amplifier.

### Hardware
- MCU: Seeed XIAO ESP32-S3
- Display: SSD1306 0.96 inch OLED (I2C)
- Audio: MAX98357 I2S 3W Amplifier

### Features
- 4-page rotating OLED dashboard (weekly usage, token breakdown, daily/session stats, system info)
- Audio alerts at 80%, 90%, 100% of weekly token limit
- Open WiFi AP + captive portal for first-boot WiFi setup
- mDNS: http://claude-meter.local
- REST API for companion Python script
- Persistent stats (NVS flash)
- OTA firmware updates (ArduinoOTA)
- Auto daily + weekly reset (NTP-synced)
- Factory reset via BOOT button hold (5s)

### Flash via Web Flasher
1. Open https://ajvizganapathy-pixel.github.io/claude-token-meter/ in Chrome or Edge
2. Put XIAO in bootloader mode: hold BOOT, press RESET, release BOOT
3. Download firmware.bin from this release assets below
4. Click Connect then Flash Firmware in the web flasher

Requires Chrome/Edge 89+ with Web Serial API.

### Build fixes applied for Arduino ESP32 3.x compatibility
- Fixed I2S driver API usage (removed deprecated communication_format field)
- Fixed numeric literal digit separator for C++11 compatibility
- Fixed Serial.printf with F() macro
- Removed deprecated MDNS.update() call
- Converted embedded JS to arrow functions for INO-to-CPP compiler compatibility
