/*
 * ESP8266 NTP Clock with OLED SSD1306
 *
 * OLED SSD1306: SDA=D1(GPIO5), SCL=D2(GPIO4) (128x64)
 * OTA:          Hostname "ESP8266_LCD"
 * Board:        esp8266:esp8266:d1_mini (HW-630)
 *
 * Features:
 *   - NTP clock on OLED with large time + date
 *   - Colorful web dashboard (auto-refreshing)
 *   - WiFi captive portal for setup
 *   - OTA firmware update
 *   - RAM-optimized (no MQTT, PROGMEM HTML, stack buffers)
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <ArduinoOTA.h>
#include <EEPROM.h>
#include <Wire.h>
#include <SSD1306Wire.h>
#include <time.h>

// ─── Pin Definitions (HW-630: SDA=D1, SCL=D2) ─────────────────────
#define OLED_SDA  5   // D1 (GPIO5)
#define OLED_SCL  4   // D2 (GPIO4)

// ─── EEPROM Config ──────────────────────────────────────────────────
#define EEPROM_SIZE   256
#define EEPROM_MAGIC  0xBE

struct DevConfig {
  uint8_t magic;
  char ssid[33];
  char pass[65];
  char tzInfo[40];   // POSIX TZ string e.g. "ICT-7"
};

static DevConfig cfg;

// ─── NTP Data ───────────────────────────────────────────────────────
struct NTPData {
  uint8_t hour, minute, second;
  uint8_t day, month;
  uint16_t year;
  uint8_t wday;
  bool synced;
};

static NTPData ntpData;

// ─── Objects ────────────────────────────────────────────────────────
static SSD1306Wire oled(0x3C, OLED_SDA, OLED_SCL);
static ESP8266WebServer server(80);
static DNSServer dnsServer;

static bool apMode = false;
static uint32_t lastOledUpdate = 0;

// ─── Day/Month Names ────────────────────────────────────────────────
static const char *const DAYS[] = {
  "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};
static const char *const MONTHS[] = {
  "Jan", "Feb", "Mar", "Apr", "May", "Jun",
  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

// ─── EEPROM Config ──────────────────────────────────────────────────
static void loadConfig() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(0, cfg);
  if (cfg.magic != EEPROM_MAGIC) {
    memset(&cfg, 0, sizeof(cfg));
    cfg.magic = EEPROM_MAGIC;
    strlcpy(cfg.tzInfo, "ICT-7", sizeof(cfg.tzInfo));  // Bangkok UTC+7
  }
  EEPROM.end();
}

static void saveConfig() {
  cfg.magic = EEPROM_MAGIC;
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.put(0, cfg);
  EEPROM.commit();
  EEPROM.end();
}

// ─── NTP Update ─────────────────────────────────────────────────────
static void ntpUpdate() {
  time_t now = time(nullptr);
  if (now < 100000) return;

  struct tm *lt = localtime(&now);
  ntpData.hour   = lt->tm_hour;
  ntpData.minute = lt->tm_min;
  ntpData.second = lt->tm_sec;
  ntpData.day    = lt->tm_mday;
  ntpData.month  = lt->tm_mon + 1;
  ntpData.year   = lt->tm_year + 1900;
  ntpData.wday   = lt->tm_wday;
  ntpData.synced = true;
}

// ─── OLED Display ───────────────────────────────────────────────────
static void oledUpdate() {
  if (millis() - lastOledUpdate < 500) return;
  lastOledUpdate = millis();

  oled.clear();

  if (!ntpData.synced) {
    oled.setFont(ArialMT_Plain_16);
    oled.setTextAlignment(TEXT_ALIGN_CENTER);
    if (apMode) {
      oled.drawString(64, 8, "WiFi Setup");
      oled.setFont(ArialMT_Plain_10);
      oled.drawString(64, 30, "Connect to:");
      oled.drawString(64, 44, "ESP8266_LCD");
    } else {
      oled.drawString(64, 12, "Syncing NTP...");
      oled.setFont(ArialMT_Plain_10);
      oled.drawString(64, 36, WiFi.localIP().toString().c_str());
    }
    oled.display();
    return;
  }

  // WiFi signal bars (top-right corner)
  if (!apMode) {
    int32_t rssi = WiFi.RSSI();
    uint8_t bars = (rssi > -55) ? 4 : (rssi > -70) ? 3 : (rssi > -80) ? 2 : (rssi > -90) ? 1 : 0;
    for (uint8_t i = 0; i < 4; i++) {
      uint8_t x = 112 + i * 4;
      uint8_t h = 3 + i * 3;  // bar heights: 3,6,9,12
      if (i < bars) {
        oled.fillRect(x, 12 - h, 3, h);
      } else {
        oled.drawRect(x, 12 - h, 3, h);
      }
    }
  }

  // Time in large font
  char timeBuf[9];
  snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d:%02d",
    ntpData.hour, ntpData.minute, ntpData.second);

  oled.setFont(ArialMT_Plain_24);
  oled.setTextAlignment(TEXT_ALIGN_CENTER);
  oled.drawString(64, 2, timeBuf);

  // Date line
  char dateBuf[20];
  snprintf(dateBuf, sizeof(dateBuf), "%s %02d %s %04d",
    DAYS[ntpData.wday], ntpData.day,
    MONTHS[ntpData.month - 1], ntpData.year);

  oled.setFont(ArialMT_Plain_10);
  oled.drawString(64, 32, dateBuf);

  // IP + RSSI line
  if (!apMode) {
    char ipBuf[28];
    snprintf(ipBuf, sizeof(ipBuf), "%s  %ddBm",
      WiFi.localIP().toString().c_str(), WiFi.RSSI());
    oled.drawString(64, 50, ipBuf);
  }

  oled.display();
}

// ─── Dashboard HTML (PROGMEM) ───────────────────────────────────────
static const char DASHBOARD_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP8266 NTP Clock</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:'Segoe UI',system-ui,sans-serif;background:linear-gradient(135deg,#0f0c29,#302b63,#24243e);
color:#e0e0e0;min-height:100vh;display:flex;flex-direction:column;align-items:center;justify-content:center;padding:16px}
h1{font-size:1.4em;margin-bottom:18px;
background:linear-gradient(90deg,#ff6b6b,#feca57,#48dbfb,#a18cd1);-webkit-background-clip:text;-webkit-text-fill-color:transparent}
.card{background:rgba(255,255,255,0.06);backdrop-filter:blur(14px);border:1px solid rgba(255,255,255,0.1);
border-radius:22px;padding:32px 44px;text-align:center;min-width:300px;box-shadow:0 8px 32px rgba(0,0,0,0.3)}
.time{font-size:4em;font-weight:700;letter-spacing:4px;
background:linear-gradient(90deg,#00d2ff,#3a7bd5,#a18cd1,#fbc2eb);background-size:200% auto;
-webkit-background-clip:text;-webkit-text-fill-color:transparent;animation:shine 3s linear infinite}
@keyframes shine{to{background-position:200% center}}
.sec{font-size:1.6em;color:#a18cd1;vertical-align:super}
.date{font-size:1.15em;color:#aaa;margin-top:10px;letter-spacing:2px}
.day{color:#fbc2eb;font-weight:600}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-top:20px}
.g{background:rgba(255,255,255,0.05);border-radius:12px;padding:12px;transition:transform .2s}
.g:hover{transform:scale(1.04)}
.gl{font-size:0.65em;color:#888;text-transform:uppercase;letter-spacing:1px}
.gv{font-size:1.05em;font-weight:600;margin-top:3px}
.c1{color:#1dd1a1}.c2{color:#48dbfb}.c3{color:#feca57}.c4{color:#ff6b6b}
.dot{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:6px;vertical-align:middle}
.dot-g{background:#1dd1a1;box-shadow:0 0 6px #1dd1a1}.dot-r{background:#ff6b6b;box-shadow:0 0 6px #ff6b6b}
.status{margin-top:18px;font-size:0.8em;color:#aaa}
.nav{margin-top:18px}
.nav a{color:#48dbfb;text-decoration:none;margin:0 14px;font-size:0.9em;transition:color .2s}
.nav a:hover{color:#fbc2eb}
footer{margin-top:24px;font-size:0.7em;color:#555}
</style></head><body>
<h1>ESP8266 NTP Clock</h1>
<div class="card">
<div class="time" id="time">--:--</div>
<div class="date"><span class="day" id="day">---</span> <span id="date">-- --- ----</span></div>
<div class="grid">
<div class="g"><div class="gl">IP Address</div><div class="gv c3" id="ip">--</div></div>
<div class="g"><div class="gl">Timezone</div><div class="gv c4" id="tz">--</div></div>
<div class="g"><div class="gl">Uptime</div><div class="gv c1" id="up">--</div></div>
<div class="g"><div class="gl">Free RAM</div><div class="gv c2" id="heap">--</div></div>
</div>
<div class="status"><span class="dot dot-g" id="dot"></span><span id="st">Connected</span></div>
</div>
<div class="nav"><a href="/">Dashboard</a><a href="/settings">Settings</a></div>
<footer>ESP8266_LCD &bull; OTA Enabled</footer>
<script>
var fail=0;
function u(){fetch('/api/data').then(r=>r.json()).then(d=>{
fail=0;document.getElementById('dot').className='dot dot-g';document.getElementById('st').textContent='Connected';
if(d.time){var p=d.time.split(':');
document.getElementById('time').innerHTML=p[0]+':'+p[1]+'<span class="sec">:'+p[2]+'</span>';}
if(d.date)document.getElementById('date').textContent=d.date;
if(d.day)document.getElementById('day').textContent=d.day;
if(d.ip)document.getElementById('ip').textContent=d.ip;
if(d.tz)document.getElementById('tz').textContent=d.tz;
if(d.heap)document.getElementById('heap').textContent=(d.heap/1024).toFixed(1)+' KB';
if(d.up!==undefined){var s=d.up;var h=Math.floor(s/3600);var m=Math.floor((s%3600)/60);
document.getElementById('up').textContent=h+'h '+m+'m';}
}).catch(e=>{fail++;if(fail>3){document.getElementById('dot').className='dot dot-r';
document.getElementById('st').textContent='Offline';}});}
u();setInterval(u,1000);
</script></body></html>)rawliteral";

// ─── Settings Page HTML (PROGMEM) ───────────────────────────────────
static const char SETTINGS_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Settings</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:'Segoe UI',system-ui,sans-serif;background:linear-gradient(135deg,#0f0c29,#302b63,#24243e);
color:#e0e0e0;min-height:100vh;padding:16px;display:flex;flex-direction:column;align-items:center}
h1{font-size:1.4em;margin:16px 0;
background:linear-gradient(90deg,#a18cd1,#fbc2eb);-webkit-background-clip:text;-webkit-text-fill-color:transparent}
form{background:rgba(255,255,255,0.08);backdrop-filter:blur(10px);border:1px solid rgba(255,255,255,0.12);
border-radius:14px;padding:22px;width:100%;max-width:380px}
label{display:block;font-size:0.8em;color:#aaa;margin:12px 0 4px;text-transform:uppercase;letter-spacing:1px}
input{width:100%;padding:9px 12px;border-radius:8px;border:1px solid rgba(255,255,255,0.2);
background:rgba(0,0,0,0.3);color:#fff;font-size:0.9em}
input:focus{outline:none;border-color:#a18cd1;box-shadow:0 0 8px rgba(161,140,209,0.3)}
button{margin-top:18px;width:100%;padding:12px;border:none;border-radius:10px;
background:linear-gradient(90deg,#a18cd1,#fbc2eb);color:#000;font-size:1em;font-weight:600;cursor:pointer;
transition:opacity .2s}
button:hover{opacity:0.85}
.nav{margin:14px 0}
.nav a{color:#48dbfb;text-decoration:none;margin:0 14px;font-size:0.85em}
.msg{text-align:center;color:#1dd1a1;margin-top:12px;font-size:0.85em}
h2{font-size:1em;color:#aaa;margin-top:18px;border-top:1px solid rgba(255,255,255,0.1);padding-top:14px}
.hint{font-size:0.7em;color:#666;margin-top:3px}
</style></head><body>
<h1>Settings</h1>
<div class="nav"><a href="/">Dashboard</a><a href="/settings">Settings</a></div>
<form method="POST" action="/settings">
<h2>WiFi</h2>
<label>SSID</label><input name="ssid" value="{{SSID}}" maxlength="32">
<label>Password</label><input name="pass" type="password" value="{{PASS}}" maxlength="64">
<h2>Time</h2>
<label>Timezone (POSIX)</label><input name="tz" value="{{TZ}}" maxlength="39">
<div class="hint">Examples: ICT-7 (Bangkok), EST5EDT (US East), GMT0BST (UK), JST-9 (Japan), AEST-10 (Sydney)</div>
<button type="submit">Save &amp; Reboot</button>
</form>
<div class="msg" id="msg"></div>
<script>
if(location.search==='?saved=1')document.getElementById('msg').textContent='Settings saved! Rebooting...';
</script></body></html>)rawliteral";

// ─── Captive Portal HTML (PROGMEM) ─────────────────────────────────
static const char PORTAL_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP8266 Clock Setup</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:'Segoe UI',sans-serif;background:linear-gradient(135deg,#0f0c29,#302b63,#24243e);color:#e0e0e0;
display:flex;flex-direction:column;align-items:center;justify-content:center;min-height:100vh;padding:20px}
h1{margin-bottom:20px;
background:linear-gradient(90deg,#ff6b6b,#feca57,#48dbfb);-webkit-background-clip:text;-webkit-text-fill-color:transparent}
form{background:rgba(255,255,255,0.08);border-radius:14px;padding:26px;width:100%;max-width:340px;
border:1px solid rgba(255,255,255,0.1)}
label{display:block;font-size:0.8em;color:#aaa;margin:12px 0 4px}
input{width:100%;padding:9px 12px;border-radius:8px;border:1px solid rgba(255,255,255,0.2);
background:rgba(0,0,0,0.3);color:#fff;font-size:0.9em}
input:focus{outline:none;border-color:#48dbfb}
button{margin-top:18px;width:100%;padding:12px;border:none;border-radius:10px;
background:linear-gradient(90deg,#48dbfb,#a18cd1);color:#000;font-size:1em;font-weight:600;cursor:pointer}
button:hover{opacity:0.85}
.info{margin-top:16px;font-size:0.75em;color:#666;text-align:center}
</style></head><body>
<h1>ESP8266 Clock Setup</h1>
<form method="POST" action="/save">
<label>WiFi SSID</label><input name="ssid" maxlength="32" required>
<label>WiFi Password</label><input name="pass" type="password" maxlength="64">
<label>Timezone</label><input name="tz" value="ICT-7" maxlength="39">
<button type="submit">Connect</button>
</form>
<div class="info">Connect to this AP and configure your WiFi credentials</div>
</body></html>)rawliteral";

// ─── Web Handlers ───────────────────────────────────────────────────
static void handleRoot() {
  server.send_P(200, "text/html", DASHBOARD_HTML);
}

static void handleAPI() {
  char buf[192];
  int len = 0;
  len += snprintf(buf + len, sizeof(buf) - len,
    "{\"heap\":%u,\"up\":%lu",
    ESP.getFreeHeap(), (unsigned long)(millis() / 1000));

  if (!apMode) {
    len += snprintf(buf + len, sizeof(buf) - len,
      ",\"ip\":\"%s\"", WiFi.localIP().toString().c_str());
  }

  len += snprintf(buf + len, sizeof(buf) - len, ",\"tz\":\"%s\"", cfg.tzInfo);

  if (ntpData.synced) {
    char timeBuf[9], dateBuf[16];
    snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d:%02d",
      ntpData.hour, ntpData.minute, ntpData.second);
    snprintf(dateBuf, sizeof(dateBuf), "%02d %s %04d",
      ntpData.day, MONTHS[ntpData.month - 1], ntpData.year);
    len += snprintf(buf + len, sizeof(buf) - len,
      ",\"time\":\"%s\",\"date\":\"%s\",\"day\":\"%s\"",
      timeBuf, dateBuf, DAYS[ntpData.wday]);
  }

  buf[len++] = '}';
  buf[len] = '\0';
  server.send(200, "application/json", buf);
}

static void handleSettings() {
  size_t pgmLen = strlen_P(SETTINGS_HTML);
  char *page = (char *)malloc(pgmLen + 200);
  if (!page) { server.send(500, "text/plain", "OOM"); return; }
  memcpy_P(page, SETTINGS_HTML, pgmLen + 1);

  auto replaceTag = [](char *html, const char *tag, const char *val) {
    char *pos = strstr(html, tag);
    if (!pos) return;
    size_t tagLen = strlen(tag);
    size_t valLen = strlen(val);
    size_t tailLen = strlen(pos + tagLen);
    memmove(pos + valLen, pos + tagLen, tailLen + 1);
    memcpy(pos, val, valLen);
  };

  replaceTag(page, "{{SSID}}", cfg.ssid);
  replaceTag(page, "{{PASS}}", cfg.pass);
  replaceTag(page, "{{TZ}}", cfg.tzInfo);

  server.send(200, "text/html", page);
  free(page);
}

static void handleSettingsSave() {
  if (server.hasArg("ssid")) strlcpy(cfg.ssid, server.arg("ssid").c_str(), sizeof(cfg.ssid));
  if (server.hasArg("pass")) strlcpy(cfg.pass, server.arg("pass").c_str(), sizeof(cfg.pass));
  if (server.hasArg("tz")) strlcpy(cfg.tzInfo, server.arg("tz").c_str(), sizeof(cfg.tzInfo));

  saveConfig();
  server.sendHeader("Location", "/settings?saved=1");
  server.send(302);
  delay(1000);
  ESP.restart();
}

static void handlePortal() {
  server.send_P(200, "text/html", PORTAL_HTML);
}

static void handlePortalSave() {
  if (server.hasArg("ssid")) strlcpy(cfg.ssid, server.arg("ssid").c_str(), sizeof(cfg.ssid));
  if (server.hasArg("pass")) strlcpy(cfg.pass, server.arg("pass").c_str(), sizeof(cfg.pass));
  if (server.hasArg("tz")) strlcpy(cfg.tzInfo, server.arg("tz").c_str(), sizeof(cfg.tzInfo));
  saveConfig();
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

// ─── WiFi Setup ─────────────────────────────────────────────────────
static bool wifiConnect() {
  if (cfg.ssid[0] == '\0') return false;

  Serial.printf("WiFi: %s\n", cfg.ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(cfg.ssid, cfg.pass);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());
    return true;
  }
  Serial.println("WiFi failed");
  return false;
}

static void startAP() {
  apMode = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP("ESP8266_LCD");
  delay(100);
  Serial.printf("AP: %s\n", WiFi.softAPIP().toString().c_str());

  dnsServer.start(53, "*", WiFi.softAPIP());

  server.on("/", handlePortal);
  server.on("/save", HTTP_POST, handlePortalSave);
  server.on("/settings", HTTP_GET, handleSettings);
  server.on("/settings", HTTP_POST, handleSettingsSave);
  server.on("/api/data", handleAPI);
  server.onNotFound(handleNotFound);
  server.begin();
}

static void startSTA() {
  apMode = false;

  configTime(cfg.tzInfo, "pool.ntp.org", "time.nist.gov");
  Serial.printf("NTP configured TZ=%s\n", cfg.tzInfo);

  server.on("/", handleRoot);
  server.on("/settings", HTTP_GET, handleSettings);
  server.on("/settings", HTTP_POST, handleSettingsSave);
  server.on("/api/data", handleAPI);
  server.onNotFound(handleNotFound);
  server.begin();

  ArduinoOTA.setHostname("ESP8266_LCD");
  ArduinoOTA.begin();
  Serial.println("OTA: ESP8266_LCD");
}

// ─── Setup ──────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== ESP8266 NTP Clock ===");

  // OLED init
  Wire.begin(OLED_SDA, OLED_SCL);
  oled.init();
  oled.flipScreenVertically();
  oled.setBrightness(255);
  oled.setContrast(255);
  oled.setFont(ArialMT_Plain_16);
  oled.setTextAlignment(TEXT_ALIGN_CENTER);
  oled.drawString(64, 20, "NTP Clock");
  oled.display();

  // Load config
  loadConfig();
  Serial.printf("SSID='%s' TZ='%s'\n", cfg.ssid, cfg.tzInfo);

  // WiFi
  if (wifiConnect()) {
    startSTA();
  } else {
    startAP();
  }

  Serial.printf("Heap: %u\n", ESP.getFreeHeap());
  Serial.println("Ready.");
}

// ─── Main Loop ──────────────────────────────────────────────────────
void loop() {
  if (!apMode) ntpUpdate();

  oledUpdate();

  server.handleClient();

  if (apMode) {
    dnsServer.processNextRequest();
  } else {
    ArduinoOTA.handle();
  }

  yield();
}
