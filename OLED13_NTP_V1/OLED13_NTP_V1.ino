/*
 * NTP Clock on OLED XFP1116-07A-Y (SH1106/CH1116 128x64)
 * Wemos D1 Mini (ESP8266)
 *
 * OLED:  SDA=D5 (GPIO14), SCL=D6 (GPIO12), addr 0x3C
 * LED:   D4 (GPIO2) built-in, active LOW
 * OTA:   Hostname "OLED13"
 * Board: esp8266:esp8266:d1_mini
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <ArduinoOTA.h>
#include <EEPROM.h>
#include <Wire.h>
#include <SH1106Wire.h>
#include <time.h>

// ─── Pins ───────────────────────────────────────────────────────────
#define OLED_SDA  D5   // GPIO14
#define OLED_SCL  D6   // GPIO12
#define LED_PIN   D4   // GPIO2 built-in LED (active LOW)

// ─── EEPROM ─────────────────────────────────────────────────────────
#define EEPROM_SIZE  256
#define EEPROM_MAGIC 0xE7

struct DevConfig {
  uint8_t magic;
  char ssid[33];
  char pass[65];
  char tz[40];
};

static DevConfig cfg;

// ─── NTP Data ───────────────────────────────────────────────────────
static uint8_t ntpH, ntpM, ntpS, ntpDay, ntpMon, ntpWday;
static uint16_t ntpYear;
static bool ntpSynced = false;

static const char * const DAYS[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
static const char * const MONS[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                     "Jul","Aug","Sep","Oct","Nov","Dec"};

// ─── Objects ────────────────────────────────────────────────────────
static SH1106Wire oled(0x3C, OLED_SDA, OLED_SCL);
static ESP8266WebServer server(80);
static DNSServer dnsServer;
static bool apMode = false;
static uint32_t lastOled = 0;
static bool ledState = false;

// ─── EEPROM ─────────────────────────────────────────────────────────
static void loadCfg() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(0, cfg);
  if (cfg.magic != EEPROM_MAGIC) {
    memset(&cfg, 0, sizeof(cfg));
    cfg.magic = EEPROM_MAGIC;
    strlcpy(cfg.tz, "ICT-7", sizeof(cfg.tz));
  }
  EEPROM.end();
}

static void saveCfg() {
  cfg.magic = EEPROM_MAGIC;
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.put(0, cfg);
  EEPROM.commit();
  EEPROM.end();
}

// ─── NTP ────────────────────────────────────────────────────────────
static void ntpUpdate() {
  time_t now = time(nullptr);
  if (now < 100000) return;
  struct tm *t = localtime(&now);
  ntpH = t->tm_hour; ntpM = t->tm_min; ntpS = t->tm_sec;
  ntpDay = t->tm_mday; ntpMon = t->tm_mon + 1;
  ntpYear = t->tm_year + 1900; ntpWday = t->tm_wday;
  ntpSynced = true;
}

// ─── OLED ───────────────────────────────────────────────────────────
static void oledUpdate() {
  if (millis() - lastOled < 500) return;
  lastOled = millis();

  oled.clear();

  if (!ntpSynced) {
    oled.setFont(ArialMT_Plain_16);
    oled.setTextAlignment(TEXT_ALIGN_CENTER);
    if (apMode) {
      oled.drawString(64, 10, "WiFi Setup");
      oled.setFont(ArialMT_Plain_10);
      oled.drawString(64, 36, "AP: OLED13_Setup");
    } else {
      oled.drawString(64, 10, "Syncing NTP...");
      oled.setFont(ArialMT_Plain_10);
      oled.drawString(64, 36, WiFi.localIP().toString().c_str());
    }
    oled.display();
    return;
  }

  // Time (large)
  char buf[20];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d", ntpH, ntpM, ntpS);
  oled.setFont(ArialMT_Plain_24);
  oled.setTextAlignment(TEXT_ALIGN_CENTER);
  oled.drawString(64, 5, buf);

  // Date
  snprintf(buf, sizeof(buf), "%s %d %s %d",
    DAYS[ntpWday], ntpDay, MONS[ntpMon - 1], ntpYear);
  oled.setFont(ArialMT_Plain_16);
  oled.drawString(64, 40, buf);

  oled.display();

  // Flash LED on each OLED update
  ledState = !ledState;
  digitalWrite(LED_PIN, ledState ? LOW : HIGH);
}

// ─── Dashboard HTML (PROGMEM) ───────────────────────────────────────
static const char PAGE_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>NTP Clock</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:'Segoe UI',system-ui,sans-serif;background:linear-gradient(135deg,#0f0c29,#302b63,#24243e);
color:#e0e0e0;min-height:100vh;display:flex;flex-direction:column;align-items:center;justify-content:center;padding:16px}
h1{font-size:1.3em;margin-bottom:16px;
background:linear-gradient(90deg,#f093fb,#f5576c);-webkit-background-clip:text;-webkit-text-fill-color:transparent}
.card{background:rgba(255,255,255,0.06);backdrop-filter:blur(12px);border:1px solid rgba(255,255,255,0.1);
border-radius:20px;padding:30px 40px;text-align:center;min-width:300px}
.time{font-size:4em;font-weight:700;letter-spacing:3px;
background:linear-gradient(90deg,#00d2ff,#3a7bd5,#f093fb);-webkit-background-clip:text;-webkit-text-fill-color:transparent}
.sec{font-size:1.5em;color:#f093fb;vertical-align:super}
.date{font-size:1.2em;color:#bbb;margin-top:8px;letter-spacing:2px}
.day{color:#f5576c;font-weight:700}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-top:16px}
.item{background:rgba(255,255,255,0.05);border-radius:10px;padding:8px}
.lbl{font-size:0.6em;color:#777;text-transform:uppercase;letter-spacing:1px}
.val{font-size:0.95em;font-weight:600;margin-top:2px}
.c1{color:#1dd1a1}.c2{color:#48dbfb}.c3{color:#feca57}.c4{color:#f093fb}
.nav{margin-top:14px}
.nav a{color:#48dbfb;text-decoration:none;margin:0 12px;font-size:0.85em}
</style></head><body>
<h1>NTP Clock - OLED13</h1>
<div class="card">
<div class="time" id="t">--:--</div>
<div class="date"><span class="day" id="dy">---</span> <span id="dt">-----</span></div>
<div class="grid">
<div class="item"><div class="lbl">IP</div><div class="val c2" id="ip">--</div></div>
<div class="item"><div class="lbl">Timezone</div><div class="val c4" id="tz">--</div></div>
<div class="item"><div class="lbl">Uptime</div><div class="val c1" id="up">--</div></div>
<div class="item"><div class="lbl">Heap</div><div class="val c3" id="hp">--</div></div>
</div></div>
<div class="nav"><a href="/">Dashboard</a><a href="/settings">Settings</a></div>
<script>
function u(){fetch('/api/data').then(r=>r.json()).then(d=>{
if(d.time){var p=d.time.split(':');
document.getElementById('t').innerHTML=p[0]+':'+p[1]+'<span class="sec">:'+p[2]+'</span>';}
if(d.date)document.getElementById('dt').textContent=d.date;
if(d.day)document.getElementById('dy').textContent=d.day;
if(d.ip)document.getElementById('ip').textContent=d.ip;
if(d.tz)document.getElementById('tz').textContent=d.tz;
document.getElementById('hp').textContent=d.heap||'--';
if(d.up!==undefined){var s=d.up,h=Math.floor(s/3600),m=Math.floor((s%3600)/60);
document.getElementById('up').textContent=h+'h '+m+'m';}
}).catch(e=>{});}
u();setInterval(u,1000);
</script></body></html>)rawliteral";

// ─── Settings HTML (PROGMEM) ────────────────────────────────────────
static const char SETTINGS_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Settings</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:'Segoe UI',system-ui,sans-serif;background:linear-gradient(135deg,#0f0c29,#302b63,#24243e);
color:#e0e0e0;min-height:100vh;padding:16px;display:flex;flex-direction:column;align-items:center}
h1{font-size:1.4em;margin-bottom:12px;
background:linear-gradient(90deg,#f093fb,#f5576c);-webkit-background-clip:text;-webkit-text-fill-color:transparent}
form{background:rgba(255,255,255,0.08);backdrop-filter:blur(10px);border:1px solid rgba(255,255,255,0.12);
border-radius:12px;padding:20px;width:100%;max-width:360px}
label{display:block;font-size:0.8em;color:#aaa;margin:10px 0 3px;text-transform:uppercase;letter-spacing:1px}
input{width:100%;padding:8px 10px;border-radius:6px;border:1px solid rgba(255,255,255,0.2);
background:rgba(0,0,0,0.3);color:#fff;font-size:0.9em}
input:focus{outline:none;border-color:#f093fb}
button{margin-top:16px;width:100%;padding:10px;border:none;border-radius:8px;
background:linear-gradient(90deg,#f093fb,#f5576c);color:#fff;font-size:1em;font-weight:600;cursor:pointer}
.nav{margin:14px 0}.nav a{color:#48dbfb;text-decoration:none;margin:0 12px;font-size:0.85em}
.msg{text-align:center;color:#1dd1a1;margin-top:10px;font-size:0.85em}
.hint{font-size:0.7em;color:#666;margin-top:2px}
</style></head><body>
<h1>Settings</h1>
<div class="nav"><a href="/">Dashboard</a><a href="/settings">Settings</a></div>
<form method="POST" action="/settings">
<label>WiFi SSID</label><input name="ssid" value="{{SSID}}" maxlength="32">
<label>WiFi Password</label><input name="pass" type="password" value="{{PASS}}" maxlength="64">
<label>Timezone (POSIX)</label><input name="tz" value="{{TZ}}" maxlength="39">
<div class="hint">ICT-7 (Bangkok) | EST5EDT (US East) | GMT0BST (UK) | JST-9 (Japan)</div>
<button type="submit">Save &amp; Reboot</button>
</form>
<div class="msg" id="msg"></div>
<script>if(location.search==='?saved=1')document.getElementById('msg').textContent='Saved! Rebooting...';</script>
</body></html>)rawliteral";

// ─── Captive Portal HTML (PROGMEM) ─────────────────────────────────
static const char PORTAL_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Setup</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:'Segoe UI',sans-serif;background:#1a1a2e;color:#e0e0e0;
display:flex;flex-direction:column;align-items:center;justify-content:center;min-height:100vh;padding:20px}
h1{color:#f093fb;margin-bottom:20px}
form{background:rgba(255,255,255,0.08);border-radius:12px;padding:24px;width:100%;max-width:340px}
label{display:block;font-size:0.8em;color:#aaa;margin:10px 0 3px}
input{width:100%;padding:8px 10px;border-radius:6px;border:1px solid rgba(255,255,255,0.2);
background:rgba(0,0,0,0.3);color:#fff;font-size:0.9em}
button{margin-top:16px;width:100%;padding:10px;border:none;border-radius:8px;
background:linear-gradient(90deg,#f093fb,#f5576c);color:#fff;font-size:1em;font-weight:600;cursor:pointer}
</style></head><body>
<h1>OLED13 Setup</h1>
<form method="POST" action="/save">
<label>WiFi SSID</label><input name="ssid" maxlength="32" required>
<label>WiFi Password</label><input name="pass" type="password" maxlength="64">
<button type="submit">Connect</button>
</form></body></html>)rawliteral";

// ─── Handlers ───────────────────────────────────────────────────────
static void handleRoot() { server.send_P(200, "text/html", PAGE_HTML); }

static void handleAPI() {
  char buf[200];
  int n = 0;
  n += snprintf(buf + n, sizeof(buf) - n, "{\"heap\":%u,\"up\":%lu",
    ESP.getFreeHeap(), (unsigned long)(millis() / 1000));
  if (!apMode)
    n += snprintf(buf + n, sizeof(buf) - n, ",\"ip\":\"%s\"",
      WiFi.localIP().toString().c_str());
  n += snprintf(buf + n, sizeof(buf) - n, ",\"tz\":\"%s\"", cfg.tz);
  if (ntpSynced) {
    char tb[9], db[16];
    snprintf(tb, sizeof(tb), "%02d:%02d:%02d", ntpH, ntpM, ntpS);
    snprintf(db, sizeof(db), "%02d %s %04d", ntpDay, MONS[ntpMon-1], ntpYear);
    n += snprintf(buf + n, sizeof(buf) - n,
      ",\"time\":\"%s\",\"date\":\"%s\",\"day\":\"%s\"", tb, db, DAYS[ntpWday]);
  }
  n += snprintf(buf + n, sizeof(buf) - n, "}");
  server.send(200, "application/json", buf);
}

static void handleSettings() {
  size_t len = strlen_P(SETTINGS_HTML);
  char *p = (char *)malloc(len + 200);
  if (!p) { server.send(500, "text/plain", "OOM"); return; }
  memcpy_P(p, SETTINGS_HTML, len + 1);

  auto rep = [](char *h, const char *tag, const char *val) {
    char *pos = strstr(h, tag);
    if (!pos) return;
    size_t tl = strlen(tag), vl = strlen(val);
    memmove(pos + vl, pos + tl, strlen(pos + tl) + 1);
    memcpy(pos, val, vl);
  };

  rep(p, "{{SSID}}", cfg.ssid);
  rep(p, "{{PASS}}", cfg.pass);
  rep(p, "{{TZ}}", cfg.tz);
  server.send(200, "text/html", p);
  free(p);
}

static void handleSettingsSave() {
  if (server.hasArg("ssid")) strlcpy(cfg.ssid, server.arg("ssid").c_str(), sizeof(cfg.ssid));
  if (server.hasArg("pass")) strlcpy(cfg.pass, server.arg("pass").c_str(), sizeof(cfg.pass));
  if (server.hasArg("tz")) strlcpy(cfg.tz, server.arg("tz").c_str(), sizeof(cfg.tz));
  saveCfg();
  server.sendHeader("Location", "/settings?saved=1");
  server.send(302);
  delay(1000);
  ESP.restart();
}

static void handlePortal() { server.send_P(200, "text/html", PORTAL_HTML); }

static void handlePortalSave() {
  if (server.hasArg("ssid")) strlcpy(cfg.ssid, server.arg("ssid").c_str(), sizeof(cfg.ssid));
  if (server.hasArg("pass")) strlcpy(cfg.pass, server.arg("pass").c_str(), sizeof(cfg.pass));
  saveCfg();
  server.send(200, "text/html",
    "<html><body style='background:#1a1a2e;color:#fff;text-align:center;padding:40px'>"
    "<h2>Saved! Rebooting...</h2></body></html>");
  delay(1500);
  ESP.restart();
}

static void handleNotFound() {
  if (apMode) {
    server.sendHeader("Location", "http://192.168.4.1/");
    server.send(302);
  } else {
    server.send(404, "text/plain", "Not Found");
  }
}

// ─── WiFi ───────────────────────────────────────────────────────────
static bool wifiConnect() {
  if (cfg.ssid[0] == '\0') return false;
  Serial.printf("WiFi: %s\n", cfg.ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(cfg.ssid, cfg.pass);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(250); Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());
    return true;
  }
  return false;
}

static void startAP() {
  apMode = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP("OLED13_Setup");
  delay(100);
  dnsServer.start(53, "*", WiFi.softAPIP());
  server.on("/", handlePortal);
  server.on("/save", HTTP_POST, handlePortalSave);
  server.on("/settings", HTTP_GET, handleSettings);
  server.on("/settings", HTTP_POST, handleSettingsSave);
  server.on("/api/data", handleAPI);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.printf("AP: %s\n", WiFi.softAPIP().toString().c_str());
}

static void startSTA() {
  apMode = false;
  configTime(cfg.tz, "pool.ntp.org", "time.nist.gov");
  server.on("/", handleRoot);
  server.on("/settings", HTTP_GET, handleSettings);
  server.on("/settings", HTTP_POST, handleSettingsSave);
  server.on("/api/data", handleAPI);
  server.onNotFound(handleNotFound);
  server.begin();
  ArduinoOTA.setHostname("OLED13");
  ArduinoOTA.begin();
  Serial.println("OTA: OLED13");
}

// ─── Setup ──────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== OLED13 NTP Clock V1 ===");

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);  // OFF (active low)

  // OLED init
  Wire.begin(OLED_SDA, OLED_SCL);
  delay(100);

  // I2C scan
  Serial.println("I2C scan:");
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0)
      Serial.printf("  0x%02X found\n", a);
  }

  oled.init();
  oled.setFont(ArialMT_Plain_16);
  oled.setTextAlignment(TEXT_ALIGN_CENTER);
  oled.drawString(64, 20, "OLED13 Clock");
  oled.display();
  Serial.println("OLED OK (SH1106 128x64)");

  loadCfg();
  Serial.printf("SSID='%s' TZ='%s'\n", cfg.ssid, cfg.tz);

  if (wifiConnect()) {
    startSTA();
    oled.clear();
    oled.setFont(ArialMT_Plain_16);
    oled.setTextAlignment(TEXT_ALIGN_CENTER);
    oled.drawString(64, 20, WiFi.localIP().toString().c_str());
    oled.display();
  } else {
    startAP();
  }

  Serial.printf("Heap: %u\n", ESP.getFreeHeap());
}

// ─── Loop ───────────────────────────────────────────────────────────
void loop() {
  if (!apMode) ntpUpdate();
  oledUpdate();
  server.handleClient();
  if (apMode) dnsServer.processNextRequest();
  if (!apMode) ArduinoOTA.handle();
  yield();
}
