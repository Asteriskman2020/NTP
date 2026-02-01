/*
 * ESP8266-01 NTP Clock with OLED V1
 *
 * OLED SSD1306: SCL=GPIO0, SDA=GPIO2 (128x64)
 * LED:          GPIO2 (built-in, flashes with I2C activity)
 * OTA:          Hostname "ESP01_LCD"
 * Board:        esp8266:esp8266:generic (1M flash)
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <PubSubClient.h>
#include <ArduinoOTA.h>
#include <EEPROM.h>
#include <Wire.h>
#include <SSD1306Wire.h>
#include <time.h>

// ─── Pin Definitions ────────────────────────────────────────────────
#define OLED_SDA  0   // GPIO0
#define OLED_SCL  2   // GPIO2

// ─── EEPROM Config ──────────────────────────────────────────────────
#define EEPROM_SIZE   512
#define EEPROM_MAGIC  0xCD

struct DevConfig {
  uint8_t magic;
  char ssid[33];
  char pass[65];
  char mqttHost[65];
  uint16_t mqttPort;
  char mqttUser[33];
  char mqttPass[65];
  char mqttTopic[65];
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
static WiFiClient wifiClient;
static PubSubClient mqtt(wifiClient);

static bool apMode = false;
static uint32_t lastMqttReconnect = 0;
static uint32_t lastOledUpdate = 0;
static uint32_t lastMqttPub = 0;

// ─── Day/Month Names (flash) ────────────────────────────────────────
static const char *const DAYS[] PROGMEM = {
  "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};
static const char *const MONTHS[] PROGMEM = {
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
    cfg.mqttPort = 1883;
    strlcpy(cfg.mqttTopic, "ntp/esp01", sizeof(cfg.mqttTopic));
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
  if (now < 100000) return;  // Not synced yet

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
      oled.drawString(64, 10, "WiFi Setup");
      oled.setFont(ArialMT_Plain_10);
      oled.drawString(64, 35, "Connect to:");
      oled.drawString(64, 48, "ESP01_Clock");
    } else {
      oled.drawString(64, 16, "Syncing NTP...");
      oled.setFont(ArialMT_Plain_10);
      oled.drawString(64, 40, WiFi.localIP().toString().c_str());
    }
    oled.display();
    return;
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

  // IP / status line
  if (!apMode) {
    oled.setFont(ArialMT_Plain_10);
    oled.setTextAlignment(TEXT_ALIGN_CENTER);
    oled.drawString(64, 50, WiFi.localIP().toString().c_str());
  }

  oled.display();
}

// ─── Dashboard HTML (PROGMEM) ───────────────────────────────────────
static const char DASHBOARD_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>NTP Clock</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:'Segoe UI',system-ui,sans-serif;background:linear-gradient(135deg,#0f0c29,#302b63,#24243e);
color:#e0e0e0;min-height:100vh;display:flex;flex-direction:column;align-items:center;justify-content:center;padding:16px}
h1{font-size:1.3em;margin-bottom:16px;
background:linear-gradient(90deg,#a18cd1,#fbc2eb);-webkit-background-clip:text;-webkit-text-fill-color:transparent}
.clock-card{background:rgba(255,255,255,0.06);backdrop-filter:blur(12px);border:1px solid rgba(255,255,255,0.1);
border-radius:20px;padding:30px 40px;text-align:center;min-width:280px}
.time{font-size:3.5em;font-weight:700;letter-spacing:4px;
background:linear-gradient(90deg,#00d2ff,#3a7bd5,#a18cd1);-webkit-background-clip:text;-webkit-text-fill-color:transparent}
.seconds{font-size:1.5em;color:#a18cd1;vertical-align:super}
.date{font-size:1.1em;color:#aaa;margin-top:8px;letter-spacing:2px}
.day{color:#fbc2eb;font-weight:600}
.info{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-top:18px}
.info-item{background:rgba(255,255,255,0.05);border-radius:10px;padding:10px}
.info-label{font-size:0.65em;color:#888;text-transform:uppercase;letter-spacing:1px}
.info-value{font-size:1em;font-weight:600;margin-top:2px}
.uptime{color:#1dd1a1}.heap{color:#48dbfb}.ip{color:#feca57}.tz{color:#f368e0}
.nav{margin-top:16px}
.nav a{color:#48dbfb;text-decoration:none;margin:0 12px;font-size:0.85em}
</style></head><body>
<h1>NTP Clock</h1>
<div class="clock-card">
<div class="time" id="time">--:--</div>
<div class="date"><span class="day" id="day">---</span> <span id="date">-- --- ----</span></div>
<div class="info">
<div class="info-item"><div class="info-label">IP Address</div><div class="info-value ip" id="ip">--</div></div>
<div class="info-item"><div class="info-label">Timezone</div><div class="info-value tz" id="tz">--</div></div>
<div class="info-item"><div class="info-label">Uptime</div><div class="info-value uptime" id="up">--</div></div>
<div class="info-item"><div class="info-label">Free Heap</div><div class="info-value heap" id="heap">--</div></div>
</div>
</div>
<div class="nav"><a href="/">Dashboard</a><a href="/settings">Settings</a></div>
<script>
function u(){fetch('/api/data').then(r=>r.json()).then(d=>{
if(d.time){
var p=d.time.split(':');
document.getElementById('time').innerHTML=p[0]+':'+p[1]+'<span class="seconds">:'+p[2]+'</span>';}
if(d.date)document.getElementById('date').textContent=d.date;
if(d.day)document.getElementById('day').textContent=d.day;
if(d.ip)document.getElementById('ip').textContent=d.ip;
if(d.tz)document.getElementById('tz').textContent=d.tz;
if(d.heap)document.getElementById('heap').textContent=d.heap;
if(d.up!==undefined){var s=d.up;var h=Math.floor(s/3600);var m=Math.floor((s%3600)/60);
document.getElementById('up').textContent=h+'h '+m+'m';}
}).catch(e=>console.error(e));}
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
h1{font-size:1.4em;margin-bottom:12px;
background:linear-gradient(90deg,#a18cd1,#fbc2eb);-webkit-background-clip:text;-webkit-text-fill-color:transparent}
form{background:rgba(255,255,255,0.08);backdrop-filter:blur(10px);border:1px solid rgba(255,255,255,0.12);
border-radius:12px;padding:20px;width:100%;max-width:380px}
label{display:block;font-size:0.8em;color:#aaa;margin:10px 0 3px;text-transform:uppercase;letter-spacing:1px}
input{width:100%;padding:8px 10px;border-radius:6px;border:1px solid rgba(255,255,255,0.2);
background:rgba(0,0,0,0.3);color:#fff;font-size:0.9em}
input:focus{outline:none;border-color:#a18cd1}
button{margin-top:16px;width:100%;padding:10px;border:none;border-radius:8px;
background:linear-gradient(90deg,#a18cd1,#fbc2eb);color:#000;font-size:1em;font-weight:600;cursor:pointer}
button:hover{opacity:0.9}
.nav{margin:14px 0}
.nav a{color:#48dbfb;text-decoration:none;margin:0 12px;font-size:0.85em}
.msg{text-align:center;color:#1dd1a1;margin-top:10px;font-size:0.85em}
h2{font-size:1em;color:#aaa;margin-top:16px;border-top:1px solid rgba(255,255,255,0.1);padding-top:12px}
.hint{font-size:0.7em;color:#666;margin-top:2px}
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
<h2>MQTT</h2>
<label>Host</label><input name="mqttHost" value="{{MQTT_HOST}}" maxlength="64">
<label>Port</label><input name="mqttPort" type="number" value="{{MQTT_PORT}}">
<label>User</label><input name="mqttUser" value="{{MQTT_USER}}" maxlength="32">
<label>Password</label><input name="mqttPass" type="password" value="{{MQTT_PASS}}" maxlength="64">
<label>Topic</label><input name="mqttTopic" value="{{MQTT_TOPIC}}" maxlength="64">
<button type="submit">Save &amp; Reboot</button>
</form>
<div class="msg" id="msg"></div>
<script>
if(location.search==='?saved=1')document.getElementById('msg').textContent='Settings saved! Rebooting...';
</script></body></html>)rawliteral";

// ─── Captive Portal HTML (PROGMEM) ─────────────────────────────────
static const char PORTAL_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Clock Setup</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:'Segoe UI',sans-serif;background:#1a1a2e;color:#e0e0e0;
display:flex;flex-direction:column;align-items:center;justify-content:center;min-height:100vh;padding:20px}
h1{color:#a18cd1;margin-bottom:20px}
form{background:rgba(255,255,255,0.08);border-radius:12px;padding:24px;width:100%;max-width:340px}
label{display:block;font-size:0.8em;color:#aaa;margin:10px 0 3px}
input{width:100%;padding:8px 10px;border-radius:6px;border:1px solid rgba(255,255,255,0.2);
background:rgba(0,0,0,0.3);color:#fff;font-size:0.9em}
button{margin-top:16px;width:100%;padding:10px;border:none;border-radius:8px;
background:linear-gradient(90deg,#a18cd1,#fbc2eb);color:#000;font-size:1em;font-weight:600;cursor:pointer}
</style></head><body>
<h1>NTP Clock Setup</h1>
<form method="POST" action="/save">
<label>WiFi SSID</label><input name="ssid" maxlength="32" required>
<label>WiFi Password</label><input name="pass" type="password" maxlength="64">
<button type="submit">Connect</button>
</form></body></html>)rawliteral";

// ─── Web Handlers ───────────────────────────────────────────────────
static void handleRoot() {
  server.send_P(200, "text/html", DASHBOARD_HTML);
}

static void handleAPI() {
  char buf[256];
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

  len += snprintf(buf + len, sizeof(buf) - len, "}");
  server.send(200, "application/json", buf);
}

static void handleSettings() {
  size_t pgmLen = strlen_P(SETTINGS_HTML);
  char *page = (char *)malloc(pgmLen + 300);
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

  char portStr[6];
  snprintf(portStr, sizeof(portStr), "%d", cfg.mqttPort);

  replaceTag(page, "{{SSID}}", cfg.ssid);
  replaceTag(page, "{{PASS}}", cfg.pass);
  replaceTag(page, "{{TZ}}", cfg.tzInfo);
  replaceTag(page, "{{MQTT_HOST}}", cfg.mqttHost);
  replaceTag(page, "{{MQTT_PORT}}", portStr);
  replaceTag(page, "{{MQTT_USER}}", cfg.mqttUser);
  replaceTag(page, "{{MQTT_PASS}}", cfg.mqttPass);
  replaceTag(page, "{{MQTT_TOPIC}}", cfg.mqttTopic);

  server.send(200, "text/html", page);
  free(page);
}

static void handleSettingsSave() {
  if (server.hasArg("ssid")) strlcpy(cfg.ssid, server.arg("ssid").c_str(), sizeof(cfg.ssid));
  if (server.hasArg("pass")) strlcpy(cfg.pass, server.arg("pass").c_str(), sizeof(cfg.pass));
  if (server.hasArg("tz")) strlcpy(cfg.tzInfo, server.arg("tz").c_str(), sizeof(cfg.tzInfo));
  if (server.hasArg("mqttHost")) strlcpy(cfg.mqttHost, server.arg("mqttHost").c_str(), sizeof(cfg.mqttHost));
  if (server.hasArg("mqttPort")) cfg.mqttPort = server.arg("mqttPort").toInt();
  if (server.hasArg("mqttUser")) strlcpy(cfg.mqttUser, server.arg("mqttUser").c_str(), sizeof(cfg.mqttUser));
  if (server.hasArg("mqttPass")) strlcpy(cfg.mqttPass, server.arg("mqttPass").c_str(), sizeof(cfg.mqttPass));
  if (server.hasArg("mqttTopic")) strlcpy(cfg.mqttTopic, server.arg("mqttTopic").c_str(), sizeof(cfg.mqttTopic));

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

// ─── MQTT ───────────────────────────────────────────────────────────
static void mqttPublish() {
  if (cfg.mqttHost[0] == '\0') return;
  if (!mqtt.connected()) return;
  if (!ntpData.synced) return;

  char buf[128];
  snprintf(buf, sizeof(buf),
    "{\"time\":\"%02d:%02d:%02d\",\"date\":\"%04d-%02d-%02d\"}",
    ntpData.hour, ntpData.minute, ntpData.second,
    ntpData.year, ntpData.month, ntpData.day);
  mqtt.publish(cfg.mqttTopic, buf);
}

static void mqttReconnect() {
  if (cfg.mqttHost[0] == '\0') return;
  if (mqtt.connected()) return;
  if (millis() - lastMqttReconnect < 10000) return;
  lastMqttReconnect = millis();

  mqtt.setServer(cfg.mqttHost, cfg.mqttPort);
  bool ok;
  if (cfg.mqttUser[0] != '\0')
    ok = mqtt.connect("ESP01_LCD", cfg.mqttUser, cfg.mqttPass);
  else
    ok = mqtt.connect("ESP01_LCD");
  Serial.printf("MQTT %s: %s\n", ok ? "connected" : "failed", cfg.mqttHost);
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
  WiFi.softAP("ESP01_Clock");
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

  // Configure NTP
  configTime(cfg.tzInfo, "pool.ntp.org", "time.nist.gov");
  Serial.printf("NTP configured TZ=%s\n", cfg.tzInfo);

  server.on("/", handleRoot);
  server.on("/settings", HTTP_GET, handleSettings);
  server.on("/settings", HTTP_POST, handleSettingsSave);
  server.on("/api/data", handleAPI);
  server.onNotFound(handleNotFound);
  server.begin();

  ArduinoOTA.setHostname("ESP01_LCD");
  ArduinoOTA.begin();
  Serial.println("OTA: ESP01_LCD");
}

// ─── Setup ──────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== ESP01 NTP Clock V1 ===");

  // I2C scan to find OLED address
  Wire.begin(OLED_SDA, OLED_SCL);
  delay(200);
  Serial.println("I2C scan:");
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  Found device at 0x%02X\n", addr);
    }
  }

  // OLED init
  oled.init();
  //oled.flipScreenVertically();
  oled.setBrightness(255);
  oled.setContrast(255);
  oled.setFont(ArialMT_Plain_16);
  oled.setTextAlignment(TEXT_ALIGN_CENTER);
  oled.drawString(64, 20, "NTP Clock");
  oled.display();
  Serial.println("OLED init OK");

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
  // NTP time update
  if (!apMode) {
    ntpUpdate();
  }

  // OLED update
  oledUpdate();

  // Web server
  server.handleClient();

  // DNS captive portal
  if (apMode) {
    dnsServer.processNextRequest();
  }

  // OTA
  if (!apMode) {
    ArduinoOTA.handle();
  }

  // MQTT
  if (!apMode && cfg.mqttHost[0] != '\0') {
    mqttReconnect();
    mqtt.loop();

    // Publish every 60 seconds
    if (millis() - lastMqttPub > 60000 && ntpData.synced) {
      mqttPublish();
      lastMqttPub = millis();
    }
  }

  yield();
}
