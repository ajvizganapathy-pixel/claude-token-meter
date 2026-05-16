# Hardware & Wiring Reference

## Bill of Materials

| # | Part | Notes |
|---|------|-------|
| 1 | [Seeed XIAO ESP32-S3](https://www.seeedstudio.com/XIAO-ESP32S3-p-5627.html) | Main MCU |
| 2 | SSD1306 0.96" OLED Module (I2C, 128×64) | White or blue variant |
| 3 | MAX98357A I2S 3W Mono Amplifier breakout | Adafruit #3006 or clone |
| 4 | Small speaker 4Ω or 8Ω, 1–3W | Any 40mm–57mm diameter |
| 5 | Breadboard or custom PCB | |
| 6 | Dupont wires / 26 AWG | |

---

## Wiring Diagram

```
XIAO ESP32-S3                SSD1306 OLED (I2C)
─────────────               ─────────────────────
   3V3  ────────────────────  VCC
   GND  ────────────────────  GND
  D4/SDA (GPIO5) ──────────  SDA
  D5/SCL (GPIO6) ──────────  SCL


XIAO ESP32-S3                MAX98357 I2S Amplifier
─────────────               ───────────────────────
   3V3  ────────────────────  VIN   (3.3 – 5.5V OK)
   GND  ────────────────────  GND
  D8   (GPIO7) ─────────────  BCLK  (Bit Clock)
  D9   (GPIO8) ─────────────  LRC   (Word Select / L-R Clock)
  D10  (GPIO9) ─────────────  DIN   (Data In)
               (no connect)   SD    (always on when floating)
   GND  ────────────────────  GAIN  (9 dB; float = 12 dB)

MAX98357 Amplifier            Speaker
──────────────────           ─────────
  + (OUT+) ────────────────  + terminal
  - (OUT−) ────────────────  - terminal
```

---

## XIAO ESP32-S3 Pinout Reference

```
                USB-C
         ┌────────────────┐
    5V   │ 5V         GND │ GND
   GND   │ GND         5V │ 5V
  GPIO1  │ D0          A0 │ GPIO1
  GPIO2  │ D1          A1 │ GPIO2
  GPIO3  │ D2          A2 │ GPIO3
  GPIO4  │ D3          A3 │ GPIO4
  GPIO5  │ D4/SDA ← OLED SDA
  GPIO6  │ D5/SCL ← OLED SCL
  GPIO43 │ D6/TX       RX │ D7/GPIO44
  GPIO7  │ D8  ← MAX98357 BCLK
  GPIO8  │ D9  ← MAX98357 LRC
  GPIO9  │ D10 ← MAX98357 DIN
         └────────────────┘
              BOOT  RESET
```

---

## Notes

### OLED I2C Address
The default address is `0x3C`. Some modules use `0x3D` — if the display
doesn't initialise, change `OLED_ADDR` in the firmware from `0x3C` to `0x3D`.

### MAX98357 SD (Shutdown) Pin
Leave `SD` **unconnected** (floating) for always-on operation. Connecting to
`GND` puts the amplifier in shutdown mode (silent). Connecting to 3.3V gives
the same always-on result.

### MAX98357 GAIN Pin
| GAIN connection | Output gain |
|---|---|
| GND | 9 dB |
| Floating | 12 dB (default) |
| 100kΩ to VDD | 15 dB |
| VDD | 6 dB |

For a desk environment, 9 dB (GND) or 12 dB (floating) is usually plenty.

### Speaker Impedance
The MAX98357 works with **4Ω or 8Ω** speakers. For louder alerts, use a 
4Ω speaker. Keep speaker wires short (< 10 cm) to avoid RF interference
with the ESP32 WiFi antenna.

### Power Supply
The XIAO ESP32-S3 USB-C port provides 5V. The 3V3 pin is a regulated 3.3V
output capable of ~500 mA — sufficient for the OLED + amplifier combination.
If you power the MAX98357 from 5V instead of 3.3V it will produce more
output volume, but 3.3V is fine for desk use.

---

## OTA Firmware Updates

Once flashed and connected to WiFi, you can update firmware **without USB**:

### Using Arduino IDE
1. Open Arduino IDE → Tools → Port
2. Select the network port `claude-meter (claude-meter.local)`
3. Sketch → Upload
4. Enter password: `ctmeter2024` when prompted

### Using PlatformIO
```ini
; Add to platformio.ini:
upload_protocol = espota
upload_port     = claude-meter.local
upload_flags    = --auth=ctmeter2024
```
Then: `pio run -t upload`

### Using esptool OTA script
```bash
python -m espota -i claude-meter.local -p 3232 \
       --auth ctmeter2024 -f flasher/firmware.bin
```

> ⚠️ **Change the OTA password** in the firmware before production use:
> edit `#define OTA_PASSWORD "ctmeter2024"` in `ClaudeTokenMeter.ino`

---

## Factory Reset

Hold the **BOOT** button on the XIAO for **5 seconds** while powered.

The display shows "Factory Reset / WiFi cleared!" and the device reboots
into AP mode. All WiFi credentials are erased; token stats are kept.

To also reset all stats:
```bash
curl -X POST http://claude-meter.local/api/config \
     -H "Content-Type: application/json" \
     -d '{"resetStats": true}'
```
