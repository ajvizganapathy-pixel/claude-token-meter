/*
 * ╔══════════════════════════════════════════════╗
 * ║         CLAUDE TOKEN METER  v1.1.0           ║
 * ║  Real-time Claude API token usage monitor    ║
 * ╠══════════════════════════════════════════════╣
 * ║  Hardware:                                   ║
 * ║    • Seeed XIAO ESP32-S3                     ║
 * ║    • SSD1306 0.96" OLED (I2C)               ║
 * ║    • MAX98357 I2S 3W Amplifier + Speaker     ║
 * ╠══════════════════════════════════════════════╣
 * ║  Features:                                   ║
 * ║    • Open WiFi AP + Captive Portal setup     ║
 * ║    • 4-page rotating OLED dashboard          ║
 * ║    • Audio alerts at 80%, 90%, 100% limit    ║
 * ║    • REST API for companion script           ║
 * ║    • Mini web dashboard at device IP         ║
 * ║    • mDNS: http://claude-meter.local         ║
 * ║    • Persistent stats (NVS flash)            ║
 * ║    • OTA firmware updates (ArduinoOTA)       ║
 * ║    • Auto daily reset at midnight (NTP)      ║
 * ║    • Auto weekly reset on Monday (NTP)       ║
 * ║    • WiFi factory reset (hold BOOT 5s)       ║
 * ╚══════════════════════════════════════════════╝
 *
 *  WIRING GUIDE
 *  ─────────────────────────────────────────────
 *  OLED SSD1306 (I2C):
 *    VCC  → 3.3V
 *    GND  → GND
 *    SDA  → D4 (GPIO5)
 *    SCL  → D5 (GPIO6)
 *
 *  MAX98357 I2S Amplifier:
 *    VIN  → 3.3V
 *    GND  → GND
 *    BCLK → D8 (GPIO7)
 *    LRC  → D9 (GPIO8)
 *    DIN  → D10 (GPIO9)
 *    SD   → (leave unconnected or tie to 3.3V)
 *    GAIN → GND (9dB) or float (12dB)
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>
#include <driver/i2s.h>
#include <ArduinoOTA.h>
#include <math.h>
#include <time.h>

// ─────────────────────────────────────────────────
//   PIN DEFINITIONS  (XIAO ESP32-S3)
// ─────────────────────────────────────────────────
#define SDA_PIN       5    // D4 — OLED SDA
#define SCL_PIN       6    // D5 — OLED SCL
#define I2S_BCK_PIN   7    // D8 — MAX98357 BCLK
#define I2S_WS_PIN    8    // D9 — MAX98357 LRC/WS
#define I2S_DOUT_PIN  9    // D10 — MAX98357 DIN
#define LED_PIN       21   // Built-in LED (active HIGH on XIAO ESP32-S3)

// ─────────────────────────────────────────────────
//   DISPLAY
// ─────────────────────────────────────────────────
#define SCREEN_W    128
#define SCREEN_H    64
#define OLED_ADDR   0x3C
#define OLED_RESET  -1

Adafruit_SSD1306 oled(SCREEN_W, SCREEN_H, &Wire, OLED_RESET);

// ─────────────────────────────────────────────────
//   FIRMWARE CONSTANTS
// ─────────────────────────────────────────────────
#define FW_VERSION        "1.1.0"
#define AP_NAME           "ClaudeTokenMeter"   // Open AP — no password
#define MDNS_NAME         "claude-meter"       // → http://claude-meter.local
#define NTP_SERVER        "pool.ntp.org"
#define OTA_PASSWORD      "ctmeter2024"        // Change before deploying!
#define BOOT_PIN          0                    // BOOT/RESET button on XIAO ESP32-S3
#define BOOT_RESET_MS     5000                 // Hold BOOT 5s → clear WiFi creds
#define PAGE_INTERVAL_MS  6000   // rotate display pages every 6s
#define SAVE_INTERVAL_MS  60000  // persist stats every 60s
#define RESET_CHECK_MS    60000  // check daily/weekly resets every 60s
#define MAX_PAGES         4
#define I2S_SAMPLE_RATE   22050

// ─────────────────────────────────────────────────
//   GLOBALS
// ─────────────────────────────────────────────────
Preferences  prefs;
WebServer    httpServer(80);

// WiFiManager custom parameters (persisted in NVS)
char cfgApiKey[80]  = "";
char cfgLimit[12]   = "1000000";
char cfgDevName[32] = "ClaudeTokenMeter";

// Token statistics
struct Stats {
  long  weeklyTotal   = 0;
  long  dailyTotal    = 0;
  long  sessionTotal  = 0;
  long  inputTokens   = 0;
  long  outputTokens  = 0;
  long  cacheRead     = 0;
  long  cacheWrite    = 0;
  long  weeklyLimit   = 1000000;
  float costUSD       = 0.0f;
  char  model[64]     = "---";
  char  lastUpdate[20]= "Never";
  bool  hasData       = false;
} stats;

// Alert state
bool alert80  = false;
bool alert90  = false;
bool alert100 = false;

// Display / UI state
uint8_t  curPage       = 0;
uint32_t lastPage      = 0;
uint32_t lastSave      = 0;
uint32_t lastResetChk  = 0;   // auto-reset checker
uint32_t bootPressedAt = 0;   // factory-reset via BOOT hold
bool     blinkOn       = true;
uint32_t lastBlink     = 0;
uint32_t setupAnim     = 0;
bool     otaInProgress = false;

// ════════════════════════════════════════════════
//   I2S AUDIO
// ════════════════════════════════════════════════

void i2sInit() {
  i2s_config_t cfg = {};
  cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate          = I2S_SAMPLE_RATE;
  cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count        = 8;
  cfg.dma_buf_len          = 256;
  cfg.use_apll             = false;
  cfg.tx_desc_auto_clear   = true;

  i2s_pin_config_t pins = {};
  pins.bck_io_num   = I2S_BCK_PIN;
  pins.ws_io_num    = I2S_WS_PIN;
  pins.data_out_num = I2S_DOUT_PIN;
  pins.data_in_num  = I2S_PIN_NO_CHANGE;

  i2s_driver_install(I2S_NUM_0, &cfg, 0, nullptr);
  i2s_set_pin(I2S_NUM_0, &pins);
  i2s_zero_dma_buffer(I2S_NUM_0);
}

// Play a sine-wave tone with fade-in/out envelope
void playTone(uint16_t freq, uint16_t durationMs, uint8_t vol = 80) {
  const int SAMPLES = (I2S_SAMPLE_RATE * durationMs) / 1000;
  const float AMP   = (float)vol / 100.0f * 28000.0f;
  const int FADE    = (I2S_SAMPLE_RATE * 12) / 1000; // 12 ms fade

  int16_t* buf = (int16_t*)malloc(SAMPLES * 2 * sizeof(int16_t));
  if (!buf) return;

  for (int i = 0; i < SAMPLES; i++) {
    float env = 1.0f;
    if (i < FADE)             env = (float)i / FADE;
    if (i > SAMPLES - FADE)   env = (float)(SAMPLES - i) / FADE;
    int16_t s = (int16_t)(AMP * env * sinf(2.0f * M_PI * freq * i / I2S_SAMPLE_RATE));
    buf[i * 2]     = s;   // L
    buf[i * 2 + 1] = s;   // R
  }

  size_t written;
  i2s_write(I2S_NUM_0, buf, SAMPLES * 4, &written, portMAX_DELAY);
  free(buf);
}

// Silence gap helper
void silence(uint16_t ms) {
  const int SAMPLES = (I2S_SAMPLE_RATE * ms) / 1000;
  int16_t* buf = (int16_t*)calloc(SAMPLES * 2, sizeof(int16_t));
  if (!buf) { delay(ms); return; }
  size_t written;
  i2s_write(I2S_NUM_0, buf, SAMPLES * 4, &written, portMAX_DELAY);
  free(buf);
}

/*
 * Sound patterns:
 *  1 = Startup chime      4 = 90% alert (3 rapid beeps)
 *  2 = WiFi connected     5 = 100% alarm (urgent)
 *  3 = 80% warning        6 = Data received (soft click)
 */
void playPattern(uint8_t p) {
  switch (p) {
    case 1:  // Startup: ascending triad
      playTone(523, 100); silence(40);
      playTone(659, 100); silence(40);
      playTone(784, 180);
      break;
    case 2:  // Connected: two-note chime
      playTone(880, 90); silence(40);
      playTone(1047, 140);
      break;
    case 3:  // 80% warning: double beep
      playTone(880, 220); silence(130);
      playTone(880, 220);
      break;
    case 4:  // 90% alert: triple rapid
      for (int i = 0; i < 3; i++) {
        playTone(1046, 150); silence(90);
      }
      break;
    case 5:  // 100% alarm: urgent alternating
      for (int i = 0; i < 6; i++) {
        playTone(1318, 90); silence(60);
        playTone(987,  90); silence(60);
      }
      break;
    case 6:  // Data received: soft click
      playTone(1200, 50, 25);
      break;
  }
}

// ════════════════════════════════════════════════
//   DISPLAY HELPERS
// ════════════════════════════════════════════════

void drawCentered(const char* text, int y, uint8_t sz = 1) {
  oled.setTextSize(sz);
  int16_t x1, y1; uint16_t w, h;
  oled.getTextBounds(text, 0, y, &x1, &y1, &w, &h);
  oled.setCursor((SCREEN_W - w) / 2, y);
  oled.print(text);
}

// Format large numbers: 1,234,567 → 1.23M  /  12,345 → 12.3K
void fmtNum(long n, char* buf) {
  if (n >= 1000000L)  sprintf(buf, "%.2fM", n / 1000000.0f);
  else if (n >= 1000) sprintf(buf, "%.1fK", n / 1000.0f);
  else                sprintf(buf, "%ld", n);
}

// Filled progress bar (outline + fill)
void drawBar(int x, int y, int w, int h, float pct) {
  oled.drawRect(x, y, w, h, WHITE);
  int fill = (int)((w - 2) * constrain(pct, 0.0f, 1.0f));
  if (fill > 0) oled.fillRect(x + 1, y + 1, fill, h - 2, WHITE);
}

// ════════════════════════════════════════════════
//   DISPLAY SCREENS
// ════════════════════════════════════════════════

void drawSplash() {
  oled.clearDisplay();
  oled.setTextColor(WHITE);

  // Large title
  oled.setTextSize(2);
  drawCentered("CLAUDE", 4);

  // Subtitle in box
  oled.drawRoundRect(18, 24, 92, 13, 3, WHITE);
  oled.setTextSize(1);
  oled.setTextColor(WHITE);
  drawCentered("TOKEN  METER", 27);

  oled.drawLine(8, 42, 120, 42, WHITE);

  oled.setTextSize(1);
  char ver[28];
  sprintf(ver, "v%s  XIAO ESP32-S3", FW_VERSION);
  drawCentered(ver, 46);
  drawCentered("Initializing...", 56);

  oled.display();
}

void drawWiFiPortal() {
  oled.clearDisplay();
  oled.setTextColor(WHITE);

  // Inverted title
  oled.fillRect(0, 0, SCREEN_W, 11, WHITE);
  oled.setTextColor(BLACK);
  oled.setTextSize(1);
  drawCentered("SETUP  MODE", 2);
  oled.setTextColor(WHITE);

  oled.setCursor(0, 14);
  oled.print("1. Connect to WiFi:");

  // Highlighted AP name
  oled.setTextColor(BLACK, WHITE);
  oled.setCursor(4, 25);
  oled.print("  ClaudeTokenMeter  ");
  oled.setTextColor(WHITE);

  oled.setCursor(0, 38);
  oled.print("2. Open browser:");
  oled.setCursor(10, 49);
  oled.print("192.168.4.1");
  oled.setCursor(0, 57);
  oled.print("to configure WiFi");

  oled.display();
}

void drawConnecting(int dots) {
  oled.clearDisplay();
  oled.setTextColor(WHITE);
  drawCentered("Connecting to", 18);
  drawCentered("WiFi...", 30);
  char dbuf[5] = { 0 };
  for (int i = 0; i < (dots % 4); i++) dbuf[i] = '.';
  drawCentered(dbuf, 44);
  oled.display();
}

// ─── Page 0: Weekly overview ───
void drawPageMain() {
  oled.clearDisplay();
  oled.setTextColor(WHITE);

  // Title bar
  oled.fillRect(0, 0, SCREEN_W, 11, WHITE);
  oled.setTextColor(BLACK);
  oled.setTextSize(1);
  char title[28];
  snprintf(title, sizeof(title), "WEEKLY  %s", stats.lastUpdate);
  drawCentered(title, 2);
  oled.setTextColor(WHITE);

  if (!stats.hasData) {
    drawCentered("Waiting for data", 20);
    drawCentered("Run companion", 33);
    drawCentered("script on your PC", 45);
    // Blinking cursor
    if (blinkOn) oled.fillRect(56, 57, 4, 6, WHITE);
    oled.display();
    return;
  }

  // Large token count
  char numBuf[12];
  fmtNum(stats.weeklyTotal, numBuf);
  oled.setTextSize(2);
  drawCentered(numBuf, 12);
  oled.setTextSize(1);
  drawCentered("tokens used", 31);

  // Progress bar
  float pct = (stats.weeklyLimit > 0) ? (float)stats.weeklyTotal / stats.weeklyLimit : 0;
  drawBar(4, 41, 120, 8, pct);

  // Percentage on left, limit on right
  char pctBuf[8], limBuf[12];
  snprintf(pctBuf, sizeof(pctBuf), "%.1f%%", pct * 100);
  fmtNum(stats.weeklyLimit, limBuf);
  oled.setCursor(0, 53);
  oled.print(pctBuf);

  char limStr[18];
  snprintf(limStr, sizeof(limStr), "/%s lim", limBuf);
  oled.setCursor(SCREEN_W - (int)strlen(limStr) * 6, 53);
  oled.print(limStr);

  // Warning blinking dot when > 80%
  if (pct > 0.8f && blinkOn) {
    oled.fillCircle(123, 5, 4, WHITE);
    oled.setTextColor(BLACK);
    oled.setCursor(120, 2);
    oled.print("!");
    oled.setTextColor(WHITE);
  }

  oled.display();
}

// ─── Page 1: Token breakdown ───
void drawPageBreakdown() {
  oled.clearDisplay();
  oled.setTextColor(WHITE);

  oled.fillRect(0, 0, SCREEN_W, 11, WHITE);
  oled.setTextColor(BLACK);
  oled.setTextSize(1);
  drawCentered("TOKEN  BREAKDOWN", 2);
  oled.setTextColor(WHITE);

  struct Row { const char* label; long val; };
  Row rows[] = {
    { "Input  :", stats.inputTokens  },
    { "Output :", stats.outputTokens },
    { "Cache R:", stats.cacheRead    },
    { "Cache W:", stats.cacheWrite   }
  };

  int y = 14;
  for (auto& r : rows) {
    oled.setCursor(0, y);
    oled.print(r.label);

    char nb[12]; fmtNum(r.val, nb);
    oled.setCursor(SCREEN_W - (int)strlen(nb) * 6, y);
    oled.print(nb);

    // Mini proportional bar
    float p = (stats.weeklyTotal > 0) ? (float)r.val / stats.weeklyTotal : 0;
    int bw = (int)(50 * p);
    if (bw > 0) {
      oled.fillRect(50, y + 1, min(bw, 50), 5, WHITE);
    }
    y += 13;
  }

  oled.display();
}

// ─── Page 2: Daily + session + cost ───
void drawPageSession() {
  oled.clearDisplay();
  oled.setTextColor(WHITE);

  oled.fillRect(0, 0, SCREEN_W, 11, WHITE);
  oled.setTextColor(BLACK);
  oled.setTextSize(1);
  drawCentered("DAILY / SESSION", 2);
  oled.setTextColor(WHITE);

  char nb[14];

  // Daily
  oled.setCursor(0, 14); oled.print("Daily :");
  fmtNum(stats.dailyTotal, nb);
  oled.setCursor(SCREEN_W - (int)strlen(nb) * 6, 14); oled.print(nb);
  float dailyPct = (stats.weeklyLimit > 0) ? (float)stats.dailyTotal / (stats.weeklyLimit / 7.0f) : 0;
  drawBar(0, 23, SCREEN_W, 6, dailyPct);

  // Session
  oled.setCursor(0, 33); oled.print("Session:");
  fmtNum(stats.sessionTotal, nb);
  oled.setCursor(SCREEN_W - (int)strlen(nb) * 6, 33); oled.print(nb);
  float sesPct = (stats.weeklyLimit > 0) ? (float)stats.sessionTotal / (stats.weeklyLimit / 7.0f) : 0;
  drawBar(0, 42, SCREEN_W, 6, sesPct);

  // Cost + model
  oled.setCursor(0, 52);
  char costBuf[24];
  snprintf(costBuf, sizeof(costBuf), "$%.4f  %s", stats.costUSD, stats.model);
  oled.print(costBuf);

  oled.display();
}

// ─── Page 3: Network / system info ───
void drawPageSystem() {
  oled.clearDisplay();
  oled.setTextColor(WHITE);

  oled.fillRect(0, 0, SCREEN_W, 11, WHITE);
  oled.setTextColor(BLACK);
  oled.setTextSize(1);
  drawCentered("SYSTEM  INFO", 2);
  oled.setTextColor(WHITE);

  oled.setCursor(0, 14); oled.print("IP:");
  oled.setCursor(21, 14); oled.print(WiFi.localIP().toString());

  oled.setCursor(0, 25); oled.print("Net:");
  String ssid = WiFi.SSID();
  if (ssid.length() > 12) ssid = ssid.substring(0, 12) + "~";
  oled.setCursor(26, 25); oled.print(ssid);

  oled.setCursor(0, 36);
  char rssiBuf[20];
  snprintf(rssiBuf, sizeof(rssiBuf), "RSSI: %d dBm", WiFi.RSSI());
  oled.print(rssiBuf);

  oled.setCursor(0, 47); oled.print("URL:");
  oled.setCursor(26, 47); oled.print("claude-meter.local");

  oled.setCursor(0, 57);
  char fwBuf[24];
  snprintf(fwBuf, sizeof(fwBuf), "FW v%s  CTM", FW_VERSION);
  oled.print(fwBuf);

  oled.display();
}

// ════════════════════════════════════════════════
//   WEB DASHBOARD  (served at GET /)
// ════════════════════════════════════════════════

const char HTML_DASHBOARD[] PROGMEM = R"HTMLEOF(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Claude Token Meter</title>
<style>
@import url('https://fonts.googleapis.com/css2?family=IBM+Plex+Mono:wght@400;600&family=Space+Grotesk:wght@400;600;700&display=swap');
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:'Space Grotesk',sans-serif;background:#0a0a0f;color:#e8e8f0;min-height:100vh}
.hdr{background:linear-gradient(135deg,#1a0a2e 0%,#2d1b69 50%,#1a0a2e 100%);padding:24px 20px;text-align:center;border-bottom:1px solid #2d1b69}
.hdr h1{font-size:22px;font-weight:700;letter-spacing:.5px}
.hdr h1 span{color:#a78bfa}
.hdr p{font-family:'IBM Plex Mono',monospace;font-size:12px;color:#7c6fad;margin-top:4px}
.wrap{max-width:480px;margin:0 auto;padding:16px}
.card{background:#111118;border:1px solid #222235;border-radius:14px;padding:18px;margin:10px 0}
.card h2{font-family:'IBM Plex Mono',monospace;font-size:11px;color:#6b6b8a;text-transform:uppercase;letter-spacing:1.5px;margin-bottom:14px}
.big{font-size:36px;font-weight:700;color:#a78bfa;font-family:'IBM Plex Mono',monospace;text-align:center;padding:8px 0}
.row{display:flex;justify-content:space-between;align-items:center;padding:7px 0;border-bottom:1px solid #1a1a28}
.row:last-child{border-bottom:none}
.row .lbl{color:#8888a8;font-size:13px}
.row .val{font-family:'IBM Plex Mono',monospace;font-size:14px;color:#c4b5fd}
.prog-wrap{margin:12px 0 6px}
.prog-bg{background:#1a1a28;border-radius:6px;height:10px;overflow:hidden}
.prog-fill{height:100%;border-radius:6px;transition:width .6s ease}
.c-ok{background:linear-gradient(90deg,#065f46,#10b981)}
.c-warn{background:linear-gradient(90deg,#78350f,#f59e0b)}
.c-danger{background:linear-gradient(90deg,#7f1d1d,#ef4444)}
.prog-lbl{display:flex;justify-content:space-between;font-family:'IBM Plex Mono',monospace;font-size:11px;color:#555570;margin-top:4px}
.badge{display:inline-block;padding:3px 10px;border-radius:20px;font-size:11px;font-weight:600;letter-spacing:.5px;font-family:'IBM Plex Mono',monospace}
.b-ok{background:#064e3b;color:#34d399;border:1px solid #065f46}
.b-warn{background:#451a03;color:#fbbf24;border:1px solid #78350f}
.b-danger{background:#450a0a;color:#f87171;border:1px solid #7f1d1d;animation:pulse 1s infinite}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:.6}}
input[type=number]{width:100%;padding:10px;background:#1a1a28;border:1px solid #2d2d48;color:#e8e8f0;border-radius:8px;font-family:'IBM Plex Mono',monospace;font-size:14px;margin-bottom:8px;outline:none}
input[type=number]:focus{border-color:#7c3aed}
.btn{width:100%;padding:11px;background:linear-gradient(135deg,#5b21b6,#7c3aed);color:#fff;border:none;border-radius:8px;font-family:'Space Grotesk',sans-serif;font-size:14px;font-weight:600;cursor:pointer;transition:opacity .2s}
.btn:hover{opacity:.85}
.footer{text-align:center;font-family:'IBM Plex Mono',monospace;font-size:11px;color:#444460;margin:16px 0 8px;padding:0 16px}
.status-dot{display:inline-block;width:6px;height:6px;border-radius:50%;background:#10b981;margin-right:5px;animation:pulse2 2s infinite}
@keyframes pulse2{0%,100%{box-shadow:0 0 0 0 rgba(16,185,129,.4)}50%{box-shadow:0 0 0 4px rgba(16,185,129,0)}}
</style>
</head>
<body>
<div class="hdr">
  <h1>🤖 Claude <span>Token</span> Meter</h1>
  <p id="devname">Connecting...</p>
</div>
<div class="wrap">

  <div class="card">
    <h2>Weekly Usage</h2>
    <div class="big" id="wk-total">—</div>
    <div class="prog-wrap">
      <div class="prog-bg"><div class="prog-fill c-ok" id="wk-bar" style="width:0%"></div></div>
      <div class="prog-lbl"><span id="wk-pct">0%</span><span id="wk-lim">—</span></div>
    </div>
    <div style="text-align:center;margin-top:6px">
      <span class="badge b-ok" id="badge">● NOMINAL</span>
    </div>
  </div>

  <div class="card">
    <h2>Token Breakdown</h2>
    <div class="row"><span class="lbl">Input</span><span class="val" id="tok-in">—</span></div>
    <div class="row"><span class="lbl">Output</span><span class="val" id="tok-out">—</span></div>
    <div class="row"><span class="lbl">Cache Read</span><span class="val" id="tok-cr">—</span></div>
    <div class="row"><span class="lbl">Cache Write</span><span class="val" id="tok-cw">—</span></div>
    <div class="row"><span class="lbl">Est. Cost</span><span class="val" id="cost">$0.0000</span></div>
  </div>

  <div class="card">
    <h2>Session / Daily</h2>
    <div class="row"><span class="lbl">Session</span><span class="val" id="sess">—</span></div>
    <div class="row"><span class="lbl">Daily</span><span class="val" id="daily">—</span></div>
    <div class="row"><span class="lbl">Model</span><span class="val" id="model" style="font-size:12px">—</span></div>
    <div class="row"><span class="lbl">Last Update</span><span class="val" id="upd">—</span></div>
  </div>

  <div class="card">
    <h2>Set Weekly Limit</h2>
    <input type="number" id="inp-limit" placeholder="e.g.  1000000">
    <button class="btn" onclick="setLimit()">Apply Limit</button>
  </div>

  <div class="footer">
    <span class="status-dot"></span>Live • claude-meter.local • FW v<span id="fw">—</span>
  </div>
</div>

<script>
function fmt(n){
  if(n>=1e6)return(n/1e6).toFixed(2)+'M';
  if(n>=1e3)return(n/1e3).toFixed(1)+'K';
  return String(n||0);
}
async function refresh(){
  try{
    const d=await(await fetch('/api/status')).json();
    document.getElementById('devname').textContent=d.device||'Claude Token Meter';
    document.getElementById('wk-total').textContent=fmt(d.weeklyTotal||0);
    document.getElementById('wk-lim').textContent='Limit: '+fmt(d.weeklyLimit||0);
    const pct=Math.min(100,((d.weeklyTotal||0)/(d.weeklyLimit||1))*100);
    const bar=document.getElementById('wk-bar');
    bar.style.width=pct+'%';
    bar.className='prog-fill '+(pct<65?'c-ok':pct<85?'c-warn':'c-danger');
    document.getElementById('wk-pct').textContent=pct.toFixed(1)+'%';
    document.getElementById('tok-in').textContent=fmt(d.inputTokens||0);
    document.getElementById('tok-out').textContent=fmt(d.outputTokens||0);
    document.getElementById('tok-cr').textContent=fmt(d.cacheRead||0);
    document.getElementById('tok-cw').textContent=fmt(d.cacheWrite||0);
    document.getElementById('cost').textContent='$'+(d.costUSD||0).toFixed(4);
    document.getElementById('sess').textContent=fmt(d.sessionTotal||0);
    document.getElementById('daily').textContent=fmt(d.dailyTotal||0);
    document.getElementById('model').textContent=d.model||'—';
    document.getElementById('upd').textContent=d.lastUpdate||'Never';
    document.getElementById('fw').textContent=d.fwVersion||'—';
    const b=document.getElementById('badge');
    if(pct>=100){b.textContent='⚠ LIMIT REACHED';b.className='badge b-danger';}
    else if(pct>=90){b.textContent='▲ CRITICAL';b.className='badge b-danger';}
    else if(pct>=80){b.textContent='⚡ WARNING';b.className='badge b-warn';}
    else{b.textContent='● NOMINAL';b.className='badge b-ok';}
  }catch(e){}
}
async function setLimit(){
  const v=document.getElementById('inp-limit').value;
  if(!v||isNaN(v))return;
  await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({weeklyLimit:parseInt(v)})});
  refresh();
}
refresh();
setInterval(refresh,8000);
</script>
</body>
</html>
)HTMLEOF";

// ════════════════════════════════════════════════
//   HTTP SERVER HANDLERS
// ════════════════════════════════════════════════

void handleRoot() {
  httpServer.send(200, "text/html", HTML_DASHBOARD);
}

void handleApiStatus() {
  DynamicJsonDocument doc(768);
  doc["device"]       = cfgDevName;
  doc["weeklyTotal"]  = stats.weeklyTotal;
  doc["dailyTotal"]   = stats.dailyTotal;
  doc["sessionTotal"] = stats.sessionTotal;
  doc["inputTokens"]  = stats.inputTokens;
  doc["outputTokens"] = stats.outputTokens;
  doc["cacheRead"]    = stats.cacheRead;
  doc["cacheWrite"]   = stats.cacheWrite;
  doc["weeklyLimit"]  = stats.weeklyLimit;
  doc["costUSD"]      = stats.costUSD;
  doc["model"]        = stats.model;
  doc["lastUpdate"]   = stats.lastUpdate;
  doc["hasData"]      = stats.hasData;
  doc["fwVersion"]    = FW_VERSION;
  doc["rssi"]         = WiFi.RSSI();
  doc["ip"]           = WiFi.localIP().toString();
  doc["otaHostname"]  = String(MDNS_NAME) + ".local";

  String out;
  serializeJson(doc, out);
  httpServer.sendHeader("Access-Control-Allow-Origin", "*");
  httpServer.send(200, "application/json", out);
}

void handleApiUpdate() {
  if (httpServer.method() == HTTP_OPTIONS) {
    httpServer.sendHeader("Access-Control-Allow-Origin", "*");
    httpServer.sendHeader("Access-Control-Allow-Methods", "POST, OPTIONS");
    httpServer.sendHeader("Access-Control-Allow-Headers", "Content-Type");
    httpServer.send(204);
    return;
  }
  if (httpServer.method() != HTTP_POST) {
    httpServer.send(405, "text/plain", "Method Not Allowed");
    return;
  }

  DynamicJsonDocument doc(512);
  if (deserializeJson(doc, httpServer.arg("plain")) != DeserializationError::Ok) {
    httpServer.send(400, "text/plain", "Bad JSON");
    return;
  }

  if (doc.containsKey("weeklyTotal"))  stats.weeklyTotal  = doc["weeklyTotal"].as<long>();
  if (doc.containsKey("dailyTotal"))   stats.dailyTotal   = doc["dailyTotal"].as<long>();
  if (doc.containsKey("sessionTotal")) stats.sessionTotal = doc["sessionTotal"].as<long>();
  if (doc.containsKey("inputTokens"))  stats.inputTokens  = doc["inputTokens"].as<long>();
  if (doc.containsKey("outputTokens")) stats.outputTokens = doc["outputTokens"].as<long>();
  if (doc.containsKey("cacheRead"))    stats.cacheRead    = doc["cacheRead"].as<long>();
  if (doc.containsKey("cacheWrite"))   stats.cacheWrite   = doc["cacheWrite"].as<long>();
  if (doc.containsKey("weeklyLimit"))  stats.weeklyLimit  = doc["weeklyLimit"].as<long>();
  if (doc.containsKey("costUSD"))      stats.costUSD      = doc["costUSD"].as<float>();
  if (doc.containsKey("model"))
    strlcpy(stats.model, doc["model"].as<const char*>(), sizeof(stats.model));

  // Increment mode: add to running totals instead of replacing
  if (doc.containsKey("deltaTokens")) {
    long delta = doc["deltaTokens"].as<long>();
    stats.weeklyTotal  += delta;
    stats.dailyTotal   += delta;
    stats.sessionTotal += delta;
    if (doc.containsKey("deltaInput"))  stats.inputTokens  += doc["deltaInput"].as<long>();
    if (doc.containsKey("deltaOutput")) stats.outputTokens += doc["deltaOutput"].as<long>();
    if (doc.containsKey("deltaCost"))   stats.costUSD      += doc["deltaCost"].as<float>();
  }

  // Timestamp
  struct tm ti;
  if (getLocalTime(&ti))
    strftime(stats.lastUpdate, sizeof(stats.lastUpdate), "%H:%M", &ti);
  else
    strlcpy(stats.lastUpdate, "??:??", sizeof(stats.lastUpdate));

  stats.hasData = true;
  playPattern(6);

  httpServer.sendHeader("Access-Control-Allow-Origin", "*");
  httpServer.send(200, "application/json", "{\"status\":\"ok\"}");
}

void handleApiConfig() {
  if (httpServer.method() != HTTP_POST) {
    httpServer.send(405, "text/plain", "Method Not Allowed");
    return;
  }
  DynamicJsonDocument doc(256);
  if (deserializeJson(doc, httpServer.arg("plain")) == DeserializationError::Ok) {
    if (doc.containsKey("weeklyLimit")) {
      stats.weeklyLimit = doc["weeklyLimit"].as<long>();
      prefs.begin("ctmeter", false);
      prefs.putLong("weeklyLimit", stats.weeklyLimit);
      prefs.end();
    }
    if (doc["resetStats"].as<bool>()) {
      stats.weeklyTotal = stats.dailyTotal = stats.sessionTotal = 0;
      stats.inputTokens = stats.outputTokens = stats.cacheRead = stats.cacheWrite = 0;
      stats.costUSD = 0;
      alert80 = alert90 = alert100 = false;
    }
    if (doc["resetAlerts"].as<bool>()) {
      alert80 = alert90 = alert100 = false;
    }
  }
  httpServer.sendHeader("Access-Control-Allow-Origin", "*");
  httpServer.send(200, "application/json", "{\"status\":\"ok\"}");
}

void handleNotFound() {
  httpServer.sendHeader("Location", "/");
  httpServer.send(302, "text/plain", "Redirect");
}

// ════════════════════════════════════════════════
//   ALERT MANAGEMENT
// ════════════════════════════════════════════════

void checkAlerts() {
  if (!stats.hasData || stats.weeklyLimit == 0) return;
  float pct = (float)stats.weeklyTotal / (float)stats.weeklyLimit;

  if (pct >= 1.0f && !alert100) {
    alert100 = true;
    Serial.println("[ALERT] 100% — Weekly limit reached!");
    playPattern(5);
  } else if (pct >= 0.9f && !alert90) {
    alert90 = true;
    Serial.println("[ALERT] 90% — Approaching limit!");
    playPattern(4);
  } else if (pct >= 0.8f && !alert80) {
    alert80 = true;
    Serial.println("[ALERT] 80% — Warning!");
    playPattern(3);
  }

  // Auto-reset when usage drops (weekly reset happened)
  if (pct < 0.75f) alert80  = false;
  if (pct < 0.85f) alert90  = false;
  if (pct < 0.95f) alert100 = false;
}

// ════════════════════════════════════════════════
//   PERSISTENCE
// ════════════════════════════════════════════════

void loadStats() {
  prefs.begin("ctmeter", true);
  stats.weeklyTotal  = prefs.getLong("weeklyTotal",  0);
  stats.dailyTotal   = prefs.getLong("dailyTotal",   0);
  stats.sessionTotal = prefs.getLong("sessionTotal", 0);
  stats.inputTokens  = prefs.getLong("inputTokens",  0);
  stats.outputTokens = prefs.getLong("outputTokens", 0);
  stats.cacheRead    = prefs.getLong("cacheRead",    0);
  stats.cacheWrite   = prefs.getLong("cacheWrite",   0);
  stats.weeklyLimit  = prefs.getLong("weeklyLimit",  1000000);
  stats.costUSD      = prefs.getFloat("costUSD",     0.0f);
  stats.hasData      = prefs.getBool("hasData",      false);
  String m = prefs.getString("model", "---");
  strlcpy(stats.model, m.c_str(), sizeof(stats.model));
  prefs.end();
}

void saveStats() {
  prefs.begin("ctmeter", false);
  prefs.putLong("weeklyTotal",  stats.weeklyTotal);
  prefs.putLong("dailyTotal",   stats.dailyTotal);
  prefs.putLong("sessionTotal", stats.sessionTotal);
  prefs.putLong("inputTokens",  stats.inputTokens);
  prefs.putLong("outputTokens", stats.outputTokens);
  prefs.putLong("cacheRead",    stats.cacheRead);
  prefs.putLong("cacheWrite",   stats.cacheWrite);
  prefs.putLong("weeklyLimit",  stats.weeklyLimit);
  prefs.putFloat("costUSD",     stats.costUSD);
  prefs.putBool("hasData",      stats.hasData);
  prefs.putString("model",      stats.model);
  prefs.end();
}

// ════════════════════════════════════════════════
//   OTA DISPLAY SCREENS
// ════════════════════════════════════════════════

void drawOtaProgress(unsigned int progress, unsigned int total) {
  oled.clearDisplay();
  oled.setTextColor(WHITE);

  oled.fillRect(0, 0, SCREEN_W, 11, WHITE);
  oled.setTextColor(BLACK);
  oled.setTextSize(1);
  drawCentered("OTA  UPDATE", 2);
  oled.setTextColor(WHITE);

  oled.setTextSize(1);
  drawCentered("Updating firmware...", 16);

  float pct = (total > 0) ? (float)progress / total : 0;
  drawBar(4, 30, 120, 10, pct);

  char buf[20];
  snprintf(buf, sizeof(buf), "%d%%  (%d / %d KB)",
           (int)(pct * 100), progress / 1024, total / 1024);
  oled.setTextSize(1);
  drawCentered(buf, 46);
  drawCentered("Do not power off!", 56);

  oled.display();
}

void drawOtaStart() {
  oled.clearDisplay();
  oled.setTextColor(WHITE);
  drawCentered("OTA", 8, 2);
  oled.setTextSize(1);
  drawCentered("Receiving firmware...", 36);
  drawCentered("Do not power off!", 50);
  oled.display();
}

void drawOtaError(const char* msg) {
  oled.clearDisplay();
  oled.setTextColor(WHITE);
  oled.fillRect(0, 0, SCREEN_W, 11, WHITE);
  oled.setTextColor(BLACK);
  drawCentered("OTA  ERROR", 2);
  oled.setTextColor(WHITE);
  oled.setTextSize(1);
  drawCentered(msg, 28);
  drawCentered("Restarting...", 44);
  oled.display();
  delay(2000);
}

// ════════════════════════════════════════════════
//   OTA SETUP
// ════════════════════════════════════════════════

void setupOTA() {
  ArduinoOTA.setHostname(MDNS_NAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);

  ArduinoOTA.onStart([]() {
    String type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
    Serial.printf("[OTA]  Start — updating %s\n", type.c_str());
    otaInProgress = true;
    i2s_zero_dma_buffer(I2S_NUM_0);   // silence audio during OTA
    drawOtaStart();
  });

  ArduinoOTA.onEnd([]() {
    Serial.println(F("[OTA]  Complete — rebooting"));
    oled.clearDisplay();
    oled.setTextColor(WHITE);
    oled.setTextSize(1);
    drawCentered("Update complete!", 22);
    drawCentered("Rebooting...", 38);
    oled.display();
    delay(1000);
    otaInProgress = false;
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("[OTA]  %u%%\r", (progress / (total / 100)));
    drawOtaProgress(progress, total);
  });

  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("[OTA]  Error[%u]: ", error);
    const char* msg = "Unknown error";
    if      (error == OTA_AUTH_ERROR)    msg = "Auth failed";
    else if (error == OTA_BEGIN_ERROR)   msg = "Begin failed";
    else if (error == OTA_CONNECT_ERROR) msg = "Connect failed";
    else if (error == OTA_RECEIVE_ERROR) msg = "Receive error";
    else if (error == OTA_END_ERROR)     msg = "End error";
    Serial.println(msg);
    drawOtaError(msg);
    otaInProgress = false;
  });

  ArduinoOTA.begin();
  Serial.printf("[OTA]  Ready  hostname: %s  password: %s\n",
                MDNS_NAME, OTA_PASSWORD);
}

// ════════════════════════════════════════════════
//   AUTO DAILY / WEEKLY RESET  (NTP-based)
// ════════════════════════════════════════════════

void checkAutoReset() {
  uint32_t now = millis();
  if (now - lastResetChk < RESET_CHECK_MS) return;
  lastResetChk = now;

  time_t epoch = time(nullptr);
  if (epoch < 1_000_000L) return;   // NTP not synced yet

  // Epoch-day and epoch-week counters (UTC)
  long curDay  = (long)(epoch / 86400L);
  long curWeek = (long)(epoch / (86400L * 7L));

  prefs.begin("ctmeter", false);
  long savedDay  = prefs.getLong("savedDay",  0L);
  long savedWeek = prefs.getLong("savedWeek", 0L);

  bool dayChanged  = (savedDay  != 0 && curDay  != savedDay);
  bool weekChanged = (savedWeek != 0 && curWeek != savedWeek);

  if (dayChanged) {
    stats.dailyTotal   = 0;
    stats.sessionTotal = 0;
    prefs.putLong("dailyTotal",   0);
    prefs.putLong("sessionTotal", 0);
    Serial.println(F("[RESET] Daily counters cleared at midnight (UTC)"));
  }

  if (weekChanged) {
    stats.weeklyTotal  = 0;
    stats.inputTokens  = 0;
    stats.outputTokens = 0;
    stats.cacheRead    = 0;
    stats.cacheWrite   = 0;
    stats.costUSD      = 0.0f;
    alert80 = alert90 = alert100 = false;
    prefs.putLong("weeklyTotal",  0);
    prefs.putLong("inputTokens",  0);
    prefs.putLong("outputTokens", 0);
    prefs.putLong("cacheRead",    0);
    prefs.putLong("cacheWrite",   0);
    prefs.putFloat("costUSD",     0.0f);
    Serial.println(F("[RESET] Weekly counters cleared"));
    playPattern(2);   // chime to indicate weekly reset
  }

  prefs.putLong("savedDay",  curDay);
  prefs.putLong("savedWeek", curWeek);
  prefs.end();
}

// ════════════════════════════════════════════════
//   FACTORY RESET  (hold BOOT button 5s)
// ════════════════════════════════════════════════

void checkBootButton() {
  if (digitalRead(BOOT_PIN) == LOW) {              // button pressed
    if (bootPressedAt == 0) bootPressedAt = millis();
    uint32_t held = millis() - bootPressedAt;

    if (held > BOOT_RESET_MS) {
      Serial.println(F("[BTN]  5s hold — clearing WiFi credentials!"));
      oled.clearDisplay();
      oled.setTextColor(WHITE);
      oled.setTextSize(1);
      drawCentered("Factory Reset", 16);
      drawCentered("WiFi cleared!", 30);
      drawCentered("Restarting...", 44);
      oled.display();
      playPattern(5);   // loud alert
      delay(1500);
      WiFiManager wm;
      wm.resetSettings();
      ESP.restart();
    }
  } else {
    bootPressedAt = 0;                              // released — reset timer
  }
}

// ════════════════════════════════════════════════
//   SETUP
// ════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println(F("\n\n╔═══════════════════════════════╗"));
  Serial.println(F(  "║   Claude Token Meter  v1.0.0  ║"));
  Serial.println(F(  "║   XIAO ESP32-S3 Firmware      ║"));
  Serial.println(F(  "╚═══════════════════════════════╝\n"));

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  pinMode(BOOT_PIN, INPUT_PULLUP);   // BOOT button for factory reset

  // ── OLED init ────────────────────────────────
  Wire.begin(SDA_PIN, SCL_PIN);
  if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println(F("[OLED] INIT FAILED — check wiring & address!"));
    while (true) delay(500);
  }
  oled.setTextColor(WHITE);
  drawSplash();
  Serial.println(F("[OLED] OK"));

  // ── I2S init ─────────────────────────────────
  i2sInit();
  delay(150);
  playPattern(1);   // Startup chime
  Serial.println(F("[I2S]  OK"));

  // ── Load saved stats & config ─────────────────
  loadStats();
  prefs.begin("ctmeter", true);
  strlcpy(cfgApiKey,  prefs.getString("apiKey",  "").c_str(), sizeof(cfgApiKey));
  strlcpy(cfgDevName, prefs.getString("devName", "ClaudeTokenMeter").c_str(), sizeof(cfgDevName));
  sprintf(cfgLimit, "%ld", prefs.getLong("weeklyLimit", 1000000));
  prefs.end();

  // ── WiFiManager ───────────────────────────────
  drawWiFiPortal();

  WiFiManager wm;
  wm.setTitle("Claude Token Meter Setup");
  wm.setConfigPortalTimeout(300);  // 5 min before giving up
  wm.setConnectTimeout(30);
  wm.setDarkMode(true);

  WiFiManagerParameter p_apikey ("apikey",  "Anthropic API Key (optional)",  cfgApiKey,  79);
  WiFiManagerParameter p_limit  ("limit",   "Weekly Token Limit",             cfgLimit,   11);
  WiFiManagerParameter p_devname("devname", "Device Name",                    cfgDevName, 31);
  wm.addParameter(&p_apikey);
  wm.addParameter(&p_limit);
  wm.addParameter(&p_devname);

  // Save callback
  wm.setSaveParamsCallback([&]() {
    strlcpy(cfgApiKey,  p_apikey.getValue(),  sizeof(cfgApiKey));
    strlcpy(cfgDevName, p_devname.getValue(), sizeof(cfgDevName));
    strlcpy(cfgLimit,   p_limit.getValue(),   sizeof(cfgLimit));
    stats.weeklyLimit = atol(cfgLimit);
    prefs.begin("ctmeter", false);
    prefs.putString("apiKey",    cfgApiKey);
    prefs.putString("devName",   cfgDevName);
    prefs.putLong("weeklyLimit", stats.weeklyLimit);
    prefs.end();
    Serial.println(F("[WiFi] Params saved"));
  });

  // AP mode callback (shown while portal is open)
  wm.setAPCallback([](WiFiManager* wm) {
    Serial.printf("[WiFi] AP started: %s  IP: 192.168.4.1\n", AP_NAME);
  });

  // ── Open AP — NO password ─────────────────────
  Serial.println(F("[WiFi] Starting... (open AP if no saved credentials)"));
  if (!wm.autoConnect(AP_NAME)) {   // Empty password = open network
    Serial.println(F("[WiFi] Failed / timeout → restarting"));
    ESP.restart();
  }

  // Refresh params after portal
  strlcpy(cfgApiKey,  p_apikey.getValue(),  sizeof(cfgApiKey));
  strlcpy(cfgDevName, p_devname.getValue(), sizeof(cfgDevName));
  strlcpy(cfgLimit,   p_limit.getValue(),   sizeof(cfgLimit));
  if (strlen(cfgLimit) > 0) stats.weeklyLimit = atol(cfgLimit);

  Serial.printf(F("[WiFi] Connected  IP: %s  RSSI: %d dBm\n"),
                WiFi.localIP().toString().c_str(), WiFi.RSSI());
  playPattern(2);   // Connected chime

  // ── mDNS ─────────────────────────────────────
  if (MDNS.begin(MDNS_NAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("[mDNS] http://%s.local\n", MDNS_NAME);
  }

  // ── OTA ──────────────────────────────────────
  setupOTA();

  // ── NTP ──────────────────────────────────────
  configTime(0, 0, NTP_SERVER);
  Serial.println(F("[NTP]  Syncing..."));

  // ── HTTP server ───────────────────────────────
  httpServer.on("/",            HTTP_GET,              handleRoot);
  httpServer.on("/api/status",  HTTP_GET,              handleApiStatus);
  httpServer.on("/api/update",  HTTP_POST,             handleApiUpdate);
  httpServer.on("/api/update",  HTTP_OPTIONS,          handleApiUpdate);
  httpServer.on("/api/config",  HTTP_POST,             handleApiConfig);
  httpServer.onNotFound(handleNotFound);
  httpServer.begin();
  Serial.println(F("[HTTP] Server started on port 80"));
  Serial.printf("[READY] %s  →  http://%s.local  /  http://%s\n",
               cfgDevName, MDNS_NAME, WiFi.localIP().toString().c_str());
}

// ════════════════════════════════════════════════
//   LOOP
// ════════════════════════════════════════════════

void loop() {
  // ── OTA must be first — highest priority ─────
  ArduinoOTA.handle();
  if (otaInProgress) return;   // freeze normal UI while flashing

  httpServer.handleClient();
  MDNS.update();

  // ── Factory reset via BOOT hold ───────────────
  checkBootButton();

  uint32_t now = millis();

  // ── Blink LED + blinkOn flag every 600ms ─────
  if (now - lastBlink > 600) {
    blinkOn = !blinkOn;
    lastBlink = now;
    digitalWrite(LED_PIN, blinkOn ? HIGH : LOW);
  }

  // ── Page rotation ────────────────────────────
  if (now - lastPage > PAGE_INTERVAL_MS) {
    curPage = (curPage + 1) % MAX_PAGES;
    lastPage = now;
  }

  // ── Draw current page ─────────────────────────
  switch (curPage) {
    case 0: drawPageMain();      break;
    case 1: drawPageBreakdown(); break;
    case 2: drawPageSession();   break;
    case 3: drawPageSystem();    break;
  }

  // ── Alert checks ─────────────────────────────
  checkAlerts();

  // ── Auto daily/weekly reset (NTP) ────────────
  checkAutoReset();

  // ── Periodic stats save ───────────────────────
  if (now - lastSave > SAVE_INTERVAL_MS) {
    saveStats();
    lastSave = now;
    Serial.println(F("[NVS]  Stats saved"));
  }

  delay(60);  // ~16 fps, keeps CPU load low
}
