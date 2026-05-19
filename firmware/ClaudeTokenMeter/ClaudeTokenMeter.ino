// ═══════════════════════════════════════════════════════════════════
//  Claude Token Meter — Firmware v2.0
//  Board: Seeed XIAO ESP32-S3
//  Display: SSD1306 128x64 I2C OLED
//
//  Design rules:
//   - No WiFiManager
//   - No auto factory reset / no boot-button reset
//   - No ESP.restart() outside POST /api/reboot
//   - Credentials in NVS (Preferences) + mirrored to /config.json
//   - First-boot open AP "ClaudeTokenMeter" @ 192.168.4.1
//   - On WiFi failure: stay in STA mode, retry every 60s, never restart
// ═══════════════════════════════════════════════════════════════════

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>

// ─── OLED ──────────────────────────────────────────────────────────
#define OLED_WIDTH   128
#define OLED_HEIGHT  64
#define OLED_ADDR    0x3C
Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);

// ─── State ─────────────────────────────────────────────────────────
Preferences prefs;
WebServer   server(80);

struct Config {
  String   ssid;
  String   pass;
  String   apikey;
  uint32_t limit;
  String   devname;
} cfg;

struct Stats {
  uint32_t weeklyTotal  = 0;
  uint32_t dailyTotal   = 0;
  uint32_t sessionTotal = 0;
  uint32_t inputTokens  = 0;
  uint32_t outputTokens = 0;
  uint32_t cacheRead    = 0;
  uint32_t cacheWrite   = 0;
  float    costUSD      = 0.0f;
  String   model        = "—";
  uint32_t lastUpdateMs = 0;
} stats;

enum WifiState { WIFI_AP_MODE, WIFI_CONNECTING, WIFI_CONNECTED, WIFI_RETRYING };
WifiState wifiState     = WIFI_AP_MODE;
uint32_t  lastWifiTryMs = 0;
uint32_t  lastPageMs    = 0;
uint8_t   currentPage   = 0;
const uint32_t PAGE_INTERVAL_MS = 6000;
const uint32_t WIFI_RETRY_MS    = 60000;
const uint32_t WIFI_CONNECT_TIMEOUT_MS = 30000;

// ─── Helpers ───────────────────────────────────────────────────────
String formatK(uint32_t n) {
  char buf[16];
  if (n >= 1000000) snprintf(buf, sizeof(buf), "%.2fM", n / 1000000.0);
  else if (n >= 1000) snprintf(buf, sizeof(buf), "%.1fK", n / 1000.0);
  else snprintf(buf, sizeof(buf), "%lu", (unsigned long)n);
  return String(buf);
}

void writeConfigFile() {
  JsonDocument doc;
  doc["ssid"]    = cfg.ssid;
  doc["pass"]    = cfg.pass;
  doc["apikey"]  = cfg.apikey;
  doc["limit"]   = cfg.limit;
  doc["devname"] = cfg.devname;
  File f = LittleFS.open("/config.json", FILE_WRITE);
  if (f) { serializeJsonPretty(doc, f); f.close(); }
}

void loadConfig() {
  prefs.begin("ctm", true);
  cfg.ssid    = prefs.getString("ssid", "");
  cfg.pass    = prefs.getString("pass", "");
  cfg.apikey  = prefs.getString("apikey", "");
  cfg.limit   = prefs.getULong("limit", 1000000);
  cfg.devname = prefs.getString("devname", "ClaudeTokenMeter");
  prefs.end();
}

void saveConfig() {
  prefs.begin("ctm", false);
  prefs.putString("ssid",    cfg.ssid);
  prefs.putString("pass",    cfg.pass);
  prefs.putString("apikey",  cfg.apikey);
  prefs.putULong ("limit",   cfg.limit);
  prefs.putString("devname", cfg.devname);
  prefs.end();
  writeConfigFile();
}

// ─── OLED pages ────────────────────────────────────────────────────
void drawProgressBar(int x, int y, int w, int h, float pct) {
  display.drawRect(x, y, w, h, SSD1306_WHITE);
  int fill = (int)((w - 2) * (pct / 100.0f));
  if (fill < 0) fill = 0;
  if (fill > w - 2) fill = w - 2;
  display.fillRect(x + 1, y + 1, fill, h - 2, SSD1306_WHITE);
}

void drawPageMain() {
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("WEEKLY TOKENS");

  display.setTextSize(2);
  display.setCursor(0, 12);
  display.print(formatK(stats.weeklyTotal));

  float pct = cfg.limit > 0 ? (100.0f * stats.weeklyTotal / cfg.limit) : 0.0f;
  if (pct > 100) pct = 100;
  drawProgressBar(0, 36, 128, 10, pct);

  display.setTextSize(1);
  display.setCursor(0, 52);
  char buf[32];
  snprintf(buf, sizeof(buf), "%.1f%%  of %s", pct, formatK(cfg.limit).c_str());
  display.print(buf);
}

void drawPageModel() {
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("MODEL");
  display.setCursor(0, 12);
  String m = stats.model;
  if (m.length() > 21) m = m.substring(0, 21);
  display.print(m);

  display.setCursor(0, 32);
  char buf[32];
  snprintf(buf, sizeof(buf), "COST  $%.4f", stats.costUSD);
  display.print(buf);

  display.setCursor(0, 48);
  display.print("TODAY  ");
  display.print(formatK(stats.dailyTotal));
  display.print(" tok");
}

void drawPageSession() {
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("SESSION  ");
  display.print(formatK(stats.sessionTotal));
  display.setCursor(0, 16);
  display.print("DAILY    ");
  display.print(formatK(stats.dailyTotal));
  display.setCursor(0, 32);
  display.print("INPUT    ");
  display.print(formatK(stats.inputTokens));
  display.setCursor(0, 48);
  display.print("OUTPUT   ");
  display.print(formatK(stats.outputTokens));
}

void drawPageSystem() {
  display.setTextSize(1);
  display.setCursor(0, 0);
  if (wifiState == WIFI_AP_MODE) {
    display.print("AP: 192.168.4.1");
    display.setCursor(0, 16);
    display.print("SSID: ClaudeTokenMeter");
    display.setCursor(0, 32);
    display.print("Open browser to setup");
  } else {
    display.print("IP: ");
    display.print(WiFi.localIP().toString());
    display.setCursor(0, 16);
    display.print("WiFi: ");
    String s = cfg.ssid;
    if (s.length() > 16) s = s.substring(0, 16);
    display.print(s);
    display.setCursor(0, 32);
    if (wifiState == WIFI_CONNECTED) {
      display.print("RSSI: ");
      display.print(WiFi.RSSI());
      display.print(" dBm");
    } else if (wifiState == WIFI_RETRYING) {
      display.print("WiFi: Retrying...");
    } else {
      display.print("WiFi: Connecting...");
    }
  }
  display.setCursor(0, 52);
  display.print("CTM v2.0  ");
  display.print(wifiState == WIFI_CONNECTED ? "OK" : "--");
}

void renderOLED() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  if (wifiState == WIFI_AP_MODE) {
    drawPageSystem();
  } else {
    switch (currentPage) {
      case 0: drawPageMain();    break;
      case 1: drawPageModel();   break;
      case 2: drawPageSession(); break;
      case 3: drawPageSystem();  break;
    }
  }
  display.display();
}

// ─── HTTP handlers ─────────────────────────────────────────────────
String htmlEscape(const String& s) {
  String o; o.reserve(s.length());
  for (size_t i = 0; i < s.length(); ++i) {
    char c = s[i];
    if      (c == '&') o += "&amp;";
    else if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '"') o += "&quot;";
    else o += c;
  }
  return o;
}

String portalHTML() {
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_FAILED) { WiFi.scanNetworks(true); n = 0; }

  String opts;
  for (int i = 0; i < n; ++i) {
    opts += "<option value=\"" + htmlEscape(WiFi.SSID(i)) + "\">";
  }

  String h = F("<!doctype html><html><head><meta name=viewport "
               "content='width=device-width,initial-scale=1'>"
               "<title>Claude Token Meter Setup</title>"
               "<style>body{font-family:system-ui,sans-serif;max-width:420px;"
               "margin:20px auto;padding:0 16px;background:#111;color:#eee}"
               "h1{font-size:1.2em}label{display:block;margin:12px 0 4px}"
               "input{width:100%;padding:10px;border-radius:6px;border:1px solid #444;"
               "background:#222;color:#eee;box-sizing:border-box;font-size:1em}"
               "button{margin-top:18px;padding:12px;width:100%;background:#5a4fcf;"
               "color:#fff;border:0;border-radius:6px;font-size:1em;cursor:pointer}"
               ".hint{font-size:.8em;color:#888;margin-top:4px}</style></head><body>"
               "<h1>Claude Token Meter</h1>"
               "<p>Configure WiFi and Anthropic API key.</p>"
               "<form method=POST action='/save'>");
  h += "<label>WiFi SSID</label>"
       "<input name=ssid list=nets required value=\"" + htmlEscape(cfg.ssid) + "\">"
       "<datalist id=nets>" + opts + "</datalist>";
  h += "<label>WiFi Password</label><input name=pass type=password value=\"" + htmlEscape(cfg.pass) + "\">";
  h += "<label>Anthropic API Key <span class=hint>(optional)</span></label>"
       "<input name=apikey value=\"" + htmlEscape(cfg.apikey) + "\">";
  h += "<label>Weekly Token Limit</label><input name=limit type=number value=" + String(cfg.limit) + ">";
  h += "<label>Device Name</label><input name=devname value=\"" + htmlEscape(cfg.devname) + "\">";
  h += F("<button type=submit>Save &amp; Connect</button></form></body></html>");
  return h;
}

String statusHTML() {
  float pct = cfg.limit > 0 ? (100.0f * stats.weeklyTotal / cfg.limit) : 0.0f;
  String h = F("<!doctype html><html><head><meta http-equiv=refresh content=5>"
               "<meta name=viewport content='width=device-width,initial-scale=1'>"
               "<title>Claude Token Meter</title>"
               "<style>body{font-family:system-ui,sans-serif;max-width:520px;"
               "margin:20px auto;padding:0 16px;background:#111;color:#eee}"
               ".row{display:flex;justify-content:space-between;padding:8px 0;"
               "border-bottom:1px solid #333}.bar{height:14px;background:#222;"
               "border-radius:4px;overflow:hidden;margin:12px 0}"
               ".fill{height:100%;background:#5a4fcf}</style></head><body>"
               "<h1>Claude Token Meter</h1>");
  h += "<div class=row><span>Weekly</span><b>" + formatK(stats.weeklyTotal) + " / " + formatK(cfg.limit) + "</b></div>";
  h += "<div class=bar><div class=fill style='width:" + String(pct, 1) + "%'></div></div>";
  h += "<div class=row><span>Model</span><b>" + htmlEscape(stats.model) + "</b></div>";
  h += "<div class=row><span>Cost (USD)</span><b>$" + String(stats.costUSD, 4) + "</b></div>";
  h += "<div class=row><span>Daily</span><b>" + formatK(stats.dailyTotal) + "</b></div>";
  h += "<div class=row><span>Session</span><b>" + formatK(stats.sessionTotal) + "</b></div>";
  h += "<div class=row><span>Input</span><b>" + formatK(stats.inputTokens) + "</b></div>";
  h += "<div class=row><span>Output</span><b>" + formatK(stats.outputTokens) + "</b></div>";
  h += "<div class=row><span>WiFi</span><b>" + htmlEscape(cfg.ssid) + " (" + WiFi.localIP().toString() + ")</b></div>";
  h += "<p><a style='color:#5a4fcf' href='/api/status'>JSON status</a> &middot; "
       "<a style='color:#5a4fcf' href='/config.json'>config.json</a></p>";
  h += F("</body></html>");
  return h;
}

void handleRoot() {
  if (wifiState == WIFI_AP_MODE) server.send(200, "text/html", portalHTML());
  else                           server.send(200, "text/html", statusHTML());
}

void handleSave() {
  if (server.hasArg("ssid"))    cfg.ssid    = server.arg("ssid");
  if (server.hasArg("pass"))    cfg.pass    = server.arg("pass");
  if (server.hasArg("apikey"))  cfg.apikey  = server.arg("apikey");
  if (server.hasArg("limit"))   cfg.limit   = strtoul(server.arg("limit").c_str(), nullptr, 10);
  if (server.hasArg("devname")) cfg.devname = server.arg("devname");
  if (cfg.limit == 0) cfg.limit = 1000000;
  saveConfig();

  server.send(200, "text/html",
    F("<!doctype html><meta http-equiv=refresh content='3;url=/'>"
      "<body style='font-family:system-ui;background:#111;color:#eee;padding:20px'>"
      "<h2>Saved.</h2><p>Device will now connect to WiFi.</p></body>"));

  delay(2000);
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
  wifiState     = WIFI_CONNECTING;
  lastWifiTryMs = millis();
}

void handleApiStatus() {
  JsonDocument doc;
  doc["weeklyTotal"]  = stats.weeklyTotal;
  doc["dailyTotal"]   = stats.dailyTotal;
  doc["sessionTotal"] = stats.sessionTotal;
  doc["inputTokens"]  = stats.inputTokens;
  doc["outputTokens"] = stats.outputTokens;
  doc["cacheRead"]    = stats.cacheRead;
  doc["cacheWrite"]   = stats.cacheWrite;
  doc["costUSD"]      = stats.costUSD;
  doc["model"]        = stats.model;
  doc["weeklyLimit"]  = cfg.limit;
  doc["devname"]      = cfg.devname;
  doc["ssid"]         = cfg.ssid;
  doc["ip"]           = WiFi.localIP().toString();
  doc["rssi"]         = WiFi.RSSI();
  doc["wifiState"]    = (int)wifiState;
  doc["uptimeSec"]    = millis() / 1000;
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleApiUpdate() {
  if (!server.hasArg("plain")) { server.send(400, "text/plain", "no body"); return; }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err) { server.send(400, "text/plain", err.c_str()); return; }

  if (doc["weeklyTotal"].is<uint32_t>())  stats.weeklyTotal  = doc["weeklyTotal"];
  if (doc["dailyTotal"].is<uint32_t>())   stats.dailyTotal   = doc["dailyTotal"];
  if (doc["sessionTotal"].is<uint32_t>()) stats.sessionTotal = doc["sessionTotal"];
  if (doc["inputTokens"].is<uint32_t>())  stats.inputTokens  = doc["inputTokens"];
  if (doc["outputTokens"].is<uint32_t>()) stats.outputTokens = doc["outputTokens"];
  if (doc["cacheRead"].is<uint32_t>())    stats.cacheRead    = doc["cacheRead"];
  if (doc["cacheWrite"].is<uint32_t>())   stats.cacheWrite   = doc["cacheWrite"];
  if (doc["costUSD"].is<float>())         stats.costUSD      = doc["costUSD"];
  if (doc["model"].is<const char*>())     stats.model        = String((const char*)doc["model"]);
  stats.lastUpdateMs = millis();

  server.send(200, "application/json", "{\"ok\":true}");
}

void handleApiConfig() {
  if (server.method() == HTTP_POST) {
    if (!server.hasArg("plain")) { server.send(400, "text/plain", "no body"); return; }
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, server.arg("plain"));
    if (err) { server.send(400, "text/plain", err.c_str()); return; }
    if (doc["weeklyLimit"].is<uint32_t>()) cfg.limit   = doc["weeklyLimit"];
    if (doc["devname"].is<const char*>())  cfg.devname = String((const char*)doc["devname"]);
    saveConfig();
  }
  JsonDocument doc;
  doc["ssid"]        = cfg.ssid;
  doc["apikey"]      = cfg.apikey;
  doc["weeklyLimit"] = cfg.limit;
  doc["devname"]     = cfg.devname;
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleConfigJson() {
  if (!LittleFS.exists("/config.json")) {
    server.send(404, "application/json", "{\"error\":\"no config\"}");
    return;
  }
  File f = LittleFS.open("/config.json", FILE_READ);
  server.streamFile(f, "application/json");
  f.close();
}

void handleReboot() {
  server.send(200, "text/plain", "rebooting");
  delay(500);
  ESP.restart();  // sole legitimate restart, per user request
}

void setupRoutes() {
  server.on("/",            HTTP_GET,  handleRoot);
  server.on("/save",        HTTP_POST, handleSave);
  server.on("/api/status",  HTTP_GET,  handleApiStatus);
  server.on("/api/update",  HTTP_POST, handleApiUpdate);
  server.on("/api/config",  HTTP_GET,  handleApiConfig);
  server.on("/api/config",  HTTP_POST, handleApiConfig);
  server.on("/config.json", HTTP_GET,  handleConfigJson);
  server.on("/api/reboot",  HTTP_POST, handleReboot);
  server.onNotFound([]() {
    if (wifiState == WIFI_AP_MODE) server.send(200, "text/html", portalHTML());
    else                           server.send(404, "text/plain", "not found");
  });
}

// ─── WiFi management ───────────────────────────────────────────────
void startAP() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP("ClaudeTokenMeter");
  WiFi.scanNetworks(true);
  wifiState = WIFI_AP_MODE;
}

void startSTA() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
  wifiState     = WIFI_CONNECTING;
  lastWifiTryMs = millis();
}

void updateWifi() {
  if (wifiState == WIFI_AP_MODE) return;

  if (WiFi.status() == WL_CONNECTED) {
    if (wifiState != WIFI_CONNECTED) wifiState = WIFI_CONNECTED;
    return;
  }

  uint32_t now = millis();
  if (wifiState == WIFI_CONNECTING) {
    if (now - lastWifiTryMs > WIFI_CONNECT_TIMEOUT_MS) {
      wifiState     = WIFI_RETRYING;
      lastWifiTryMs = now;
    }
  } else if (wifiState == WIFI_CONNECTED) {
    wifiState     = WIFI_RETRYING;
    lastWifiTryMs = now;
    WiFi.disconnect();
    WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
  } else if (wifiState == WIFI_RETRYING) {
    if (now - lastWifiTryMs > WIFI_RETRY_MS) {
      WiFi.disconnect();
      WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
      lastWifiTryMs = now;
    }
  }
}

// ─── Setup / loop ──────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[CTM] boot");

  Wire.begin();
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("[CTM] OLED init failed (continuing)");
  } else {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(0, 24);
    display.print("Claude Token Meter");
    display.setCursor(0, 40);
    display.print("Booting...");
    display.display();
  }

  if (!LittleFS.begin(true)) {
    Serial.println("[CTM] LittleFS mount failed");
  }

  loadConfig();
  Serial.printf("[CTM] ssid='%s' limit=%lu\n", cfg.ssid.c_str(), (unsigned long)cfg.limit);

  if (cfg.ssid.length() == 0) {
    Serial.println("[CTM] no creds, AP mode");
    startAP();
  } else {
    Serial.println("[CTM] creds found, STA mode");
    startSTA();
  }

  setupRoutes();
  server.begin();
  Serial.println("[CTM] HTTP server up on :80");
}

void loop() {
  server.handleClient();
  updateWifi();

  uint32_t now = millis();
  if (now - lastPageMs > PAGE_INTERVAL_MS) {
    lastPageMs  = now;
    currentPage = (currentPage + 1) % 4;
    renderOLED();
  }
  static uint32_t lastTick = 0;
  if (now - lastTick > 1000) { lastTick = now; renderOLED(); }

  delay(5);
}
