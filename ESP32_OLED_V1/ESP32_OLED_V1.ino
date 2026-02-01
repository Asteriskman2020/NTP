/*
 * ESP32_OLED_V1 - NTP Clock on SSD1306 OLED
 * Features: WiFi/MQTT Web Portal, OTA, Built-in LED, Colorful Dashboard
 * Board: ESP32 Dev Module
 * OLED: SSD1306 128x64 I2C (0x3C)
 * OTA Hostname: ESP32_OLCD
 */

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ArduinoOTA.h>
#include <EEPROM.h>
#include <Wire.h>
#include <time.h>
#include <PubSubClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ── OLED Config ──
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
#define OLED_ADDR     0x3C
#define OLED_SDA      21
#define OLED_SCL      22

// ── LED Config ──
#define LED_PIN       2

// ── EEPROM Config ──
#define EEPROM_SIZE   512
#define EEPROM_MAGIC  0xE7

struct DevConfig {
  uint8_t  magic;
  char     ssid[33];
  char     pass[65];
  char     tzInfo[40];
  char     mqttServer[65];
  uint16_t mqttPort;
  char     mqttUser[33];
  char     mqttPass[65];
  char     mqttTopic[65];
  uint8_t  ledBlink;
};

// ── Globals ──
DevConfig cfg;
Adafruit_SSD1306 oled(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
WebServer server(80);
DNSServer dnsServer;
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

bool apMode       = false;
bool ntpSynced    = false;
bool mqttEnabled  = false;
uint32_t lastOled = 0;
uint32_t lastLed  = 0;
uint32_t lastMqtt = 0;
bool ledState     = false;
int lastSec       = -1;

// ── Forward Declarations ──
void loadConfig();
void saveConfig();
void startAP();
bool connectWiFi();
void setupOTA();
void setupMQTT();
void mqttReconnect();
void setupRoutes();
void updateOLED();
void blinkLED();
void handleRoot();
void handleSettings();
void handleSettingsSave();
void handleAPI();
void handleReboot();

// ═══════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════
void setup() {
  Serial.begin(115200);
  Serial.println(F("\n[ESP32_OLED_V1] Booting..."));

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  // OLED Init
  Wire.begin(OLED_SDA, OLED_SCL);
  if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println(F("[OLED] Init FAILED"));
  }
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.println(F("ESP32 OLED V1"));
  oled.println(F("Booting..."));
  oled.display();

  // Load config
  loadConfig();

  // Connect WiFi or start AP
  if (strlen(cfg.ssid) > 0 && connectWiFi()) {
    apMode = false;
    setupOTA();
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    setenv("TZ", cfg.tzInfo, 1);
    tzset();
    Serial.printf("[WiFi] Connected: %s\n", WiFi.localIP().toString().c_str());

    // MQTT
    if (strlen(cfg.mqttServer) > 0) {
      mqttEnabled = true;
      setupMQTT();
    }
  } else {
    startAP();
  }

  setupRoutes();
  server.begin();
  Serial.println(F("[Web] Server started"));

  // Show IP
  oled.clearDisplay();
  oled.setCursor(0, 0);
  oled.setTextSize(1);
  if (apMode) {
    oled.println(F("AP Mode: ESP32_OLCD"));
    oled.print(F("IP: "));
    oled.println(WiFi.softAPIP());
    oled.println(F("Connect to setup"));
  } else {
    oled.println(F("Connected!"));
    oled.print(F("IP: "));
    oled.println(WiFi.localIP());
  }
  oled.display();
  delay(2000);
}

// ═══════════════════════════════════════
//  LOOP
// ═══════════════════════════════════════
void loop() {
  if (apMode) {
    dnsServer.processNextRequest();
  } else {
    ArduinoOTA.handle();
    if (mqttEnabled) {
      if (!mqtt.connected()) mqttReconnect();
      mqtt.loop();
    }
  }

  server.handleClient();
  updateOLED();
  blinkLED();

  // Publish time to MQTT every 60s
  if (mqttEnabled && mqtt.connected() && !apMode) {
    if (millis() - lastMqtt > 60000) {
      lastMqtt = millis();
      struct tm ti;
      if (getLocalTime(&ti)) {
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &ti);
        mqtt.publish(cfg.mqttTopic, buf);
      }
    }
  }
}

// ═══════════════════════════════════════
//  CONFIG (EEPROM)
// ═══════════════════════════════════════
void loadConfig() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(0, cfg);
  EEPROM.end();

  if (cfg.magic != EEPROM_MAGIC) {
    memset(&cfg, 0, sizeof(cfg));
    cfg.magic    = EEPROM_MAGIC;
    cfg.mqttPort = 1883;
    cfg.ledBlink = 1;
    strlcpy(cfg.tzInfo, "ICT-7", sizeof(cfg.tzInfo));
    strlcpy(cfg.mqttTopic, "esp32/oled/time", sizeof(cfg.mqttTopic));
    saveConfig();
    Serial.println(F("[CFG] Defaults loaded"));
  }
  Serial.printf("[CFG] SSID=%s TZ=%s MQTT=%s:%d\n",
                cfg.ssid, cfg.tzInfo, cfg.mqttServer, cfg.mqttPort);
}

void saveConfig() {
  cfg.magic = EEPROM_MAGIC;
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.put(0, cfg);
  EEPROM.commit();
  EEPROM.end();
  Serial.println(F("[CFG] Saved"));
}

// ═══════════════════════════════════════
//  WIFI
// ═══════════════════════════════════════
bool connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(cfg.ssid, cfg.pass);
  Serial.printf("[WiFi] Connecting to %s", cfg.ssid);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
    // Blink LED while connecting
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
  }
  Serial.println();
  digitalWrite(LED_PIN, LOW);
  return WiFi.status() == WL_CONNECTED;
}

void startAP() {
  apMode = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP("ESP32_OLCD", "12345678");
  dnsServer.start(53, "*", WiFi.softAPIP());
  Serial.printf("[AP] Started IP: %s\n", WiFi.softAPIP().toString().c_str());
}

// ═══════════════════════════════════════
//  OTA
// ═══════════════════════════════════════
void setupOTA() {
  ArduinoOTA.setHostname("ESP32_OLCD");

  ArduinoOTA.onStart([]() {
    oled.clearDisplay();
    oled.setTextSize(2);
    oled.setCursor(10, 0);
    oled.println(F("OTA"));
    oled.setTextSize(1);
    oled.println(F("Updating..."));
    oled.display();
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    int pct = (progress * 100) / total;
    oled.fillRect(0, 48, 128, 16, SSD1306_BLACK);
    oled.setCursor(0, 48);
    oled.printf("Progress: %d%%", pct);
    // Progress bar
    oled.drawRect(0, 58, 128, 6, SSD1306_WHITE);
    oled.fillRect(1, 59, (126 * pct) / 100, 4, SSD1306_WHITE);
    oled.display();
  });

  ArduinoOTA.onEnd([]() {
    oled.clearDisplay();
    oled.setTextSize(2);
    oled.setCursor(10, 20);
    oled.println(F("OTA Done!"));
    oled.display();
  });

  ArduinoOTA.onError([](ota_error_t error) {
    oled.clearDisplay();
    oled.setCursor(0, 0);
    oled.printf("OTA Error: %u", error);
    oled.display();
  });

  ArduinoOTA.begin();
  Serial.println(F("[OTA] Ready - ESP32_OLCD"));
}

// ═══════════════════════════════════════
//  MQTT
// ═══════════════════════════════════════
void setupMQTT() {
  mqtt.setServer(cfg.mqttServer, cfg.mqttPort);
  mqttReconnect();
}

void mqttReconnect() {
  if (mqtt.connected()) return;
  static uint32_t lastTry = 0;
  if (millis() - lastTry < 10000) return;
  lastTry = millis();

  Serial.print(F("[MQTT] Connecting..."));
  String clientId = "ESP32_OLCD_" + String(random(0xFFFF), HEX);
  bool ok;
  if (strlen(cfg.mqttUser) > 0)
    ok = mqtt.connect(clientId.c_str(), cfg.mqttUser, cfg.mqttPass);
  else
    ok = mqtt.connect(clientId.c_str());

  Serial.println(ok ? F(" OK") : F(" FAIL"));
}

// ═══════════════════════════════════════
//  OLED DISPLAY
// ═══════════════════════════════════════
void updateOLED() {
  if (millis() - lastOled < 500) return;
  lastOled = millis();

  if (apMode) {
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setCursor(0, 0);
    oled.println(F("== SETUP MODE =="));
    oled.println();
    oled.println(F("WiFi: ESP32_OLCD"));
    oled.println(F("Pass: 12345678"));
    oled.println();
    oled.print(F("IP: "));
    oled.println(WiFi.softAPIP());
    oled.println(F("Open browser to setup"));
    oled.display();
    return;
  }

  struct tm ti;
  if (!getLocalTime(&ti)) {
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setCursor(0, 24);
    oled.println(F("Waiting NTP sync..."));
    oled.display();
    return;
  }

  if (ti.tm_sec == lastSec) return;
  lastSec = ti.tm_sec;

  oled.clearDisplay();

  // Time - large
  char timeBuf[9];
  strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", &ti);
  oled.setTextSize(2);
  oled.setCursor(4, 2);
  oled.print(timeBuf);

  // Date
  char dateBuf[20];
  strftime(dateBuf, sizeof(dateBuf), "%a %d %b %Y", &ti);
  oled.setTextSize(1);
  oled.setCursor(4, 24);
  oled.print(dateBuf);

  // Divider
  oled.drawLine(0, 35, 128, 35, SSD1306_WHITE);

  // IP Address
  oled.setCursor(0, 39);
  oled.print(F("IP: "));
  oled.print(WiFi.localIP());

  // MQTT status
  oled.setCursor(0, 49);
  oled.print(F("MQTT: "));
  if (mqttEnabled)
    oled.print(mqtt.connected() ? F("Connected") : F("Disconnected"));
  else
    oled.print(F("Off"));

  // Free heap
  oled.setCursor(0, 57);
  oled.printf("Heap: %dKB", ESP.getFreeHeap() / 1024);

  oled.display();
}

// ═══════════════════════════════════════
//  LED BLINK
// ═══════════════════════════════════════
void blinkLED() {
  if (!cfg.ledBlink) return;
  if (millis() - lastLed < 1000) return;
  lastLed = millis();
  ledState = !ledState;
  digitalWrite(LED_PIN, ledState);
}

// ═══════════════════════════════════════
//  WEB SERVER ROUTES
// ═══════════════════════════════════════
void setupRoutes() {
  server.on("/",           HTTP_GET,  handleRoot);
  server.on("/settings",   HTTP_GET,  handleSettings);
  server.on("/settings",   HTTP_POST, handleSettingsSave);
  server.on("/api/data",   HTTP_GET,  handleAPI);
  server.on("/reboot",     HTTP_POST, handleReboot);
  server.onNotFound([]() {
    if (apMode) {
      server.sendHeader("Location", "http://192.168.4.1/settings");
      server.send(302, "text/plain", "");
    } else {
      server.send(404, "text/plain", "Not Found");
    }
  });
}

// ── Dashboard Page ──
void handleRoot() {
  // Use F() and chunked sending to save RAM
  String html;
  html.reserve(3200);
  html = F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ESP32 OLED Dashboard</title>"
    "<style>"
    "*{margin:0;padding:0;box-sizing:border-box}"
    "body{font-family:'Segoe UI',sans-serif;background:linear-gradient(135deg,#0f0c29,#302b63,#24243e);color:#fff;min-height:100vh}"
    ".hdr{background:linear-gradient(90deg,#667eea,#764ba2);padding:20px;text-align:center;font-size:24px;font-weight:700;letter-spacing:2px;text-shadow:0 2px 4px rgba(0,0,0,.3)}"
    ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:16px;padding:20px;max-width:900px;margin:0 auto}"
    ".card{background:rgba(255,255,255,.08);border-radius:16px;padding:20px;backdrop-filter:blur(10px);border:1px solid rgba(255,255,255,.12);transition:transform .2s}"
    ".card:hover{transform:translateY(-4px)}"
    ".card h3{font-size:13px;text-transform:uppercase;letter-spacing:1px;opacity:.7;margin-bottom:8px}"
    ".card .val{font-size:28px;font-weight:700}"
    ".t{color:#00e5ff}.d{color:#ff9100}.ip{color:#69f0ae}.mq{color:#ea80fc}.hp{color:#ffd740}.up{color:#ff5252}"
    ".led{display:inline-block;width:12px;height:12px;border-radius:50%;margin-right:6px}"
    ".led.on{background:#69f0ae;box-shadow:0 0 8px #69f0ae}.led.off{background:#555}"
    ".btn{display:inline-block;margin:20px;padding:12px 28px;background:linear-gradient(90deg,#667eea,#764ba2);border:0;border-radius:8px;color:#fff;font-size:15px;cursor:pointer;text-decoration:none}"
    ".btn:hover{opacity:.85}"
    ".foot{text-align:center;padding:16px;opacity:.5;font-size:12px}"
    "</style></head><body>"
    "<div class='hdr'>ESP32 OLED Dashboard</div>"
    "<div class='grid'>"
    "<div class='card'><h3>Time</h3><div class='val t' id='tm'>--:--:--</div></div>"
    "<div class='card'><h3>Date</h3><div class='val d' id='dt'>---</div></div>"
    "<div class='card'><h3>IP Address</h3><div class='val ip' id='ip'>---</div></div>"
    "<div class='card'><h3>MQTT</h3><div class='val mq' id='mq'>---</div></div>"
    "<div class='card'><h3>Free Heap</h3><div class='val hp' id='hp'>---</div></div>"
    "<div class='card'><h3>Uptime</h3><div class='val up' id='ut'>---</div></div>"
    "</div>"
    "<div style='text-align:center'>"
    "<a class='btn' href='/settings'>Settings</a>"
    "</div>"
    "<div class='foot'>ESP32_OLCD &mdash; NTP Clock v1</div>"
    "<script>"
    "function u(){fetch('/api/data').then(r=>r.json()).then(d=>{"
    "document.getElementById('tm').textContent=d.time;"
    "document.getElementById('dt').textContent=d.date;"
    "document.getElementById('ip').textContent=d.ip;"
    "document.getElementById('mq').textContent=d.mqtt;"
    "document.getElementById('hp').textContent=d.heap;"
    "document.getElementById('ut').textContent=d.uptime;"
    "}).catch(e=>console.log(e))}"
    "u();setInterval(u,2000);"
    "</script></body></html>");
  server.send(200, "text/html", html);
}

// ── Settings Page ──
void handleSettings() {
  String html;
  html.reserve(3000);
  html = F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Settings</title>"
    "<style>"
    "*{margin:0;padding:0;box-sizing:border-box}"
    "body{font-family:'Segoe UI',sans-serif;background:linear-gradient(135deg,#0f0c29,#302b63,#24243e);color:#fff;min-height:100vh}"
    ".hdr{background:linear-gradient(90deg,#f857a6,#ff5858);padding:20px;text-align:center;font-size:22px;font-weight:700}"
    ".wrap{max-width:500px;margin:20px auto;padding:0 16px}"
    "fieldset{border:1px solid rgba(255,255,255,.15);border-radius:12px;padding:16px;margin-bottom:16px;background:rgba(255,255,255,.05)}"
    "legend{color:#ff9100;font-weight:700;padding:0 8px}"
    "label{display:block;margin:10px 0 4px;font-size:13px;opacity:.8}"
    "input[type=text],input[type=password],input[type=number]{width:100%;padding:10px;border:1px solid rgba(255,255,255,.2);border-radius:8px;background:rgba(255,255,255,.08);color:#fff;font-size:14px;outline:0}"
    "input:focus{border-color:#667eea}"
    "select{width:100%;padding:10px;border-radius:8px;background:#1a1a2e;color:#fff;border:1px solid rgba(255,255,255,.2)}"
    ".chk{margin:10px 0}"
    ".chk input{margin-right:6px}"
    ".btn{display:block;width:100%;padding:14px;margin:10px 0;border:0;border-radius:8px;font-size:16px;font-weight:700;cursor:pointer;color:#fff}"
    ".btn-save{background:linear-gradient(90deg,#667eea,#764ba2)}"
    ".btn-reboot{background:linear-gradient(90deg,#f857a6,#ff5858)}"
    ".btn-back{background:rgba(255,255,255,.15)}"
    ".btn:hover{opacity:.85}"
    ".msg{background:#69f0ae;color:#000;padding:10px;border-radius:8px;text-align:center;margin:10px 0}"
    "</style></head><body>"
    "<div class='hdr'>Settings</div>"
    "<div class='wrap'>");

  if (server.hasArg("saved")) {
    html += F("<div class='msg'>Settings saved! Rebooting...</div>");
  }

  html += F("<form method='POST' action='/settings'>"
    "<fieldset><legend>WiFi</legend>"
    "<label>SSID</label>"
    "<input type='text' name='ssid' value='");
  html += cfg.ssid;
  html += F("' maxlength='32'>"
    "<label>Password</label>"
    "<input type='password' name='pass' value='");
  html += cfg.pass;
  html += F("' maxlength='64'>"
    "</fieldset>"
    "<fieldset><legend>Timezone</legend>"
    "<label>POSIX TZ String</label>"
    "<input type='text' name='tz' value='");
  html += cfg.tzInfo;
  html += F("' maxlength='39'>"
    "<label style='font-size:11px;opacity:.5'>Examples: ICT-7 (Bangkok), EST5EDT (US East), GMT0BST (UK), JST-9 (Japan)</label>"
    "</fieldset>"
    "<fieldset><legend>MQTT</legend>"
    "<label>Server</label>"
    "<input type='text' name='mqsrv' value='");
  html += cfg.mqttServer;
  html += F("' maxlength='64'>"
    "<label>Port</label>"
    "<input type='number' name='mqport' value='");
  html += String(cfg.mqttPort);
  html += F("'>"
    "<label>Username</label>"
    "<input type='text' name='mquser' value='");
  html += cfg.mqttUser;
  html += F("' maxlength='32'>"
    "<label>Password</label>"
    "<input type='password' name='mqpass' value='");
  html += cfg.mqttPass;
  html += F("' maxlength='64'>"
    "<label>Topic</label>"
    "<input type='text' name='mqtopic' value='");
  html += cfg.mqttTopic;
  html += F("' maxlength='64'>"
    "</fieldset>"
    "<fieldset><legend>Options</legend>"
    "<div class='chk'><label><input type='checkbox' name='led' value='1'");
  if (cfg.ledBlink) html += F(" checked");
  html += F("> Blink Built-in LED</label></div>"
    "</fieldset>"
    "<button class='btn btn-save' type='submit'>Save & Reboot</button>"
    "</form>"
    "<form method='POST' action='/reboot'>"
    "<button class='btn btn-reboot' type='submit'>Reboot Only</button>"
    "</form>"
    "<a class='btn btn-back' href='/'>Back to Dashboard</a>"
    "</div></body></html>");
  server.send(200, "text/html", html);
}

// ── Save Settings ──
void handleSettingsSave() {
  if (server.hasArg("ssid"))    strlcpy(cfg.ssid,       server.arg("ssid").c_str(),    sizeof(cfg.ssid));
  if (server.hasArg("pass"))    strlcpy(cfg.pass,       server.arg("pass").c_str(),    sizeof(cfg.pass));
  if (server.hasArg("tz"))      strlcpy(cfg.tzInfo,     server.arg("tz").c_str(),      sizeof(cfg.tzInfo));
  if (server.hasArg("mqsrv"))   strlcpy(cfg.mqttServer, server.arg("mqsrv").c_str(),   sizeof(cfg.mqttServer));
  if (server.hasArg("mqport"))  cfg.mqttPort = server.arg("mqport").toInt();
  if (server.hasArg("mquser"))  strlcpy(cfg.mqttUser,   server.arg("mquser").c_str(),  sizeof(cfg.mqttUser));
  if (server.hasArg("mqpass"))  strlcpy(cfg.mqttPass,   server.arg("mqpass").c_str(),  sizeof(cfg.mqttPass));
  if (server.hasArg("mqtopic")) strlcpy(cfg.mqttTopic,  server.arg("mqtopic").c_str(), sizeof(cfg.mqttTopic));
  cfg.ledBlink = server.hasArg("led") ? 1 : 0;

  saveConfig();
  server.sendHeader("Location", "/settings?saved=1");
  server.send(302, "text/plain", "");
  delay(1000);
  ESP.restart();
}

// ── API JSON ──
void handleAPI() {
  struct tm ti;
  char timeBuf[9]  = "--:--:--";
  char dateBuf[20] = "---";

  if (getLocalTime(&ti)) {
    strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", &ti);
    strftime(dateBuf, sizeof(dateBuf), "%a %d %b %Y", &ti);
  }

  uint32_t sec = millis() / 1000;
  uint32_t d = sec / 86400; sec %= 86400;
  uint32_t h = sec / 3600;  sec %= 3600;
  uint32_t m = sec / 60;    sec %= 60;
  char upBuf[32];
  snprintf(upBuf, sizeof(upBuf), "%ud %uh %um", d, h, m);

  char json[256];
  snprintf(json, sizeof(json),
    "{\"time\":\"%s\",\"date\":\"%s\",\"ip\":\"%s\","
    "\"mqtt\":\"%s\",\"heap\":\"%d KB\",\"uptime\":\"%s\"}",
    timeBuf, dateBuf,
    apMode ? WiFi.softAPIP().toString().c_str() : WiFi.localIP().toString().c_str(),
    mqttEnabled ? (mqtt.connected() ? "Connected" : "Disconnected") : "Off",
    ESP.getFreeHeap() / 1024, upBuf);

  server.send(200, "application/json", json);
}

// ── Reboot ──
void handleReboot() {
  server.send(200, "text/html",
    "<html><body style='background:#0f0c29;color:#fff;text-align:center;padding:40px;font-family:sans-serif'>"
    "<h2>Rebooting...</h2><p>Please wait 5 seconds then <a href='/' style='color:#69f0ae'>click here</a></p>"
    "</body></html>");
  delay(1000);
  ESP.restart();
}
