/*
 * Oil Tank Level Monitor
 * ESP32 + XKC-Y25-V non-contact liquid level sensor
 * Sends Telegram alert once per hour when oil is below sensor level.
 * Stops alerting once oil is detected again.
 *
 * Features:
 *   - Web-based configuration portal (no hardcoded credentials)
 *   - AP mode on first boot for initial setup
 *   - DHCP or static IP support
 *   - All settings saved to flash (survives power cycles)
 *   - Web interface accessible on local network for reconfiguration
 *
 * Wiring:
 *   XKC-Y25-V Brown  -> ESP32 3V3
 *   XKC-Y25-V Blue   -> ESP32 GND
 *   XKC-Y25-V Yellow -> ESP32 GPIO4 (D4)
 */

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>

// ===== AP MODE SETTINGS =====
const char* AP_SSID     = "OilMonitor-Setup";
const char* AP_PASSWORD = "oiltank123";
const IPAddress AP_IP(192, 168, 4, 1);
const IPAddress AP_GATEWAY(192, 168, 4, 1);
const IPAddress AP_SUBNET(255, 255, 255, 0);

// ===== FACTORY RESET BUTTON =====
const int RESET_BUTTON_PIN = 0;           // BOOT button (GPIO0)
const unsigned long RESET_HOLD_MS = 5000; // Hold 5 seconds to factory reset

// ===== SENSOR CONFIG =====
const int SENSOR_PIN = 4;
const unsigned long ALERT_INTERVAL_MS = 3600000UL;  // 1 hour
const unsigned long SENSOR_CHECK_MS   = 5000UL;     // 5 seconds
const int DEBOUNCE_COUNT = 3;

// ===== RUNTIME STATE =====
Preferences prefs;
WebServer server(80);
WiFiClientSecure secured_client;
UniversalTelegramBot* bot = nullptr;

// Saved settings
String cfgSSID;
String cfgPassword;
String cfgBotToken;
String cfgChatID;
bool   cfgStaticIP    = false;
String cfgIP;
String cfgGateway;
String cfgSubnet;
String cfgDNS;

// Sensor state
unsigned long lastAlertTime   = 0;
unsigned long lastSensorCheck = 0;
bool oilIsLow                = false;
int debounceCounter          = 0;
bool lastRawReading          = false;

// Mode tracking
bool apMode = false;
bool configured = false;

// =====================================================================
// Settings persistence
// =====================================================================

void factoryReset() {
  Serial.println("FACTORY RESET — wiping all settings...");
  prefs.begin("oilmon", false);
  prefs.clear();
  prefs.end();
  Serial.println("Settings cleared. Rebooting into setup mode...");
  delay(500);
  ESP.restart();
}

void checkResetButton() {
  if (digitalRead(RESET_BUTTON_PIN) == LOW) {  // Button pressed (active LOW)
    unsigned long pressStart = millis();
    Serial.print("BOOT button held — hold 5 sec to factory reset");
    while (digitalRead(RESET_BUTTON_PIN) == LOW) {
      unsigned long held = millis() - pressStart;
      if (held >= RESET_HOLD_MS) {
        Serial.println("\nFactory reset triggered!");
        factoryReset();
      }
      delay(100);
    }
    Serial.println(" — released, no reset.");
  }
}

void loadSettings() {
  prefs.begin("oilmon", true);  // read-only
  cfgSSID      = prefs.getString("ssid", "");
  cfgPassword  = prefs.getString("password", "");
  cfgBotToken  = prefs.getString("bot_token", "");
  cfgChatID    = prefs.getString("chat_id", "");
  cfgStaticIP  = prefs.getBool("static_ip", false);
  cfgIP        = prefs.getString("ip", "");
  cfgGateway   = prefs.getString("gateway", "");
  cfgSubnet    = prefs.getString("subnet", "255.255.255.0");
  cfgDNS       = prefs.getString("dns", "8.8.8.8");
  prefs.end();
  configured = (cfgSSID.length() > 0 && cfgBotToken.length() > 0 && cfgChatID.length() > 0);
}

void saveSettings() {
  prefs.begin("oilmon", false);  // read-write
  prefs.putString("ssid", cfgSSID);
  prefs.putString("password", cfgPassword);
  prefs.putString("bot_token", cfgBotToken);
  prefs.putString("chat_id", cfgChatID);
  prefs.putBool("static_ip", cfgStaticIP);
  prefs.putString("ip", cfgIP);
  prefs.putString("gateway", cfgGateway);
  prefs.putString("subnet", cfgSubnet);
  prefs.putString("dns", cfgDNS);
  prefs.end();
}

// =====================================================================
// HTML pages
// =====================================================================

String htmlHeader(const String& title) {
  String h = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  h += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  h += "<title>" + title + "</title>";
  h += "<style>";
  h += "body{font-family:system-ui,-apple-system,sans-serif;margin:0;padding:20px;background:#1a1a2e;color:#e0e0e0;}";
  h += ".container{max-width:480px;margin:0 auto;}";
  h += "h1{color:#e94560;font-size:1.5em;border-bottom:2px solid #e94560;padding-bottom:8px;}";
  h += "h2{color:#0f3460;font-size:1.1em;margin-top:24px;color:#16c79a;}";
  h += "label{display:block;margin:12px 0 4px;font-weight:600;font-size:0.9em;}";
  h += "input[type=text],input[type=password]{width:100%;padding:10px;border:1px solid #333;border-radius:6px;";
  h += "  box-sizing:border-box;font-size:1em;background:#16213e;color:#e0e0e0;}";
  h += "input:focus{outline:none;border-color:#e94560;}";
  h += "button{background:#e94560;color:#fff;border:none;padding:12px 24px;border-radius:6px;";
  h += "  font-size:1em;cursor:pointer;margin-top:20px;width:100%;}";
  h += "button:hover{background:#c81e45;}";
  h += ".toggle{display:flex;align-items:center;gap:10px;margin:12px 0;}";
  h += ".toggle input{width:auto;}";
  h += ".status{background:#16213e;padding:16px;border-radius:8px;margin:16px 0;border-left:4px solid #16c79a;}";
  h += ".status.warn{border-left-color:#e94560;}";
  h += ".ip-fields{display:none;}.ip-fields.show{display:block;}";
  h += "a{color:#16c79a;}";
  h += ".nav{margin:16px 0;font-size:0.9em;}";
  h += "</style></head><body><div class='container'>";
  return h;
}

String htmlFooter() {
  return "</div></body></html>";
}

String buildConfigPage() {
  String page = htmlHeader("Oil Tank Monitor - Settings");

  // Status section
  if (!apMode && WiFi.status() == WL_CONNECTED) {
    page += "<div class='status'>";
    page += "<strong>Status:</strong> Connected to <em>" + cfgSSID + "</em><br>";
    page += "<strong>IP:</strong> " + WiFi.localIP().toString() + "<br>";
    page += "<strong>Oil Level:</strong> " + String(oilIsLow ? "LOW" : "OK");
    page += "</div>";
  } else if (apMode) {
    page += "<div class='status warn'>";
    page += "<strong>Setup Mode</strong> — Configure your settings below, then the device will connect to your WiFi.";
    page += "</div>";
  }

  page += "<h1>Oil Tank Monitor</h1>";
  page += "<form method='POST' action='/save'>";

  // WiFi section
  page += "<h2>WiFi Network</h2>";
  page += "<label for='ssid'>SSID (Network Name)</label>";
  page += "<input type='text' id='ssid' name='ssid' value='" + cfgSSID + "' required>";
  page += "<label for='password'>Password</label>";
  page += "<input type='password' id='password' name='password' value='" + cfgPassword + "'>";

  // Telegram section
  page += "<h2>Telegram Notifications</h2>";
  page += "<label for='bot_token'>Bot Token</label>";
  page += "<input type='text' id='bot_token' name='bot_token' value='" + cfgBotToken + "' placeholder='123456789:ABCdef...' required>";
  page += "<label for='chat_id'>Chat ID</label>";
  page += "<input type='text' id='chat_id' name='chat_id' value='" + cfgChatID + "' placeholder='123456789' required>";

  // Network section
  page += "<h2>Network Settings</h2>";
  page += "<div class='toggle'>";
  page += "<input type='checkbox' id='static_ip' name='static_ip' value='1'";
  if (cfgStaticIP) page += " checked";
  page += " onchange='toggleStatic()'>";
  page += "<label for='static_ip' style='display:inline;margin:0;'>Use Static IP (instead of DHCP)</label>";
  page += "</div>";

  page += "<div class='ip-fields" + String(cfgStaticIP ? " show" : "") + "' id='ip-fields'>";
  page += "<label for='ip'>IP Address</label>";
  page += "<input type='text' id='ip' name='ip' value='" + cfgIP + "' placeholder='192.168.1.100'>";
  page += "<label for='gateway'>Gateway</label>";
  page += "<input type='text' id='gateway' name='gateway' value='" + cfgGateway + "' placeholder='192.168.1.1'>";
  page += "<label for='subnet'>Subnet Mask</label>";
  page += "<input type='text' id='subnet' name='subnet' value='" + cfgSubnet + "' placeholder='255.255.255.0'>";
  page += "<label for='dns'>DNS Server</label>";
  page += "<input type='text' id='dns' name='dns' value='" + cfgDNS + "' placeholder='8.8.8.8'>";
  page += "</div>";

  page += "<button type='submit'>Save &amp; Restart</button>";
  page += "</form>";

  // Factory reset section
  page += "<h2 style='margin-top:40px;color:#e94560;'>Danger Zone</h2>";
  page += "<button style='background:#333;border:1px solid #e94560;' ";
  page += "onclick=\"if(confirm('Are you sure you want to factory reset? This will erase ALL settings (WiFi, Telegram, network) and reboot into setup mode.')){window.location='/factory-reset'}\">";
  page += "Factory Reset</button>";

  page += "<script>";
  page += "function toggleStatic(){";
  page += "  document.getElementById('ip-fields').classList.toggle('show',";
  page += "    document.getElementById('static_ip').checked);}";
  page += "</script>";

  page += htmlFooter();
  return page;
}

String buildSavedPage() {
  String page = htmlHeader("Settings Saved");
  page += "<h1>Settings Saved</h1>";
  page += "<div class='status'>";
  page += "Configuration saved. The device is restarting and will connect to <strong>" + cfgSSID + "</strong>.<br><br>";
  if (cfgStaticIP && cfgIP.length() > 0) {
    page += "It will be available at <strong>http://" + cfgIP + "</strong>";
  } else {
    page += "Check your router or the serial monitor for the assigned IP address.";
  }
  page += "</div>";
  page += htmlFooter();
  return page;
}

// =====================================================================
// Web server handlers
// =====================================================================

void handleRoot() {
  server.send(200, "text/html", buildConfigPage());
}

void handleSave() {
  cfgSSID     = server.arg("ssid");
  cfgPassword = server.arg("password");
  cfgBotToken = server.arg("bot_token");
  cfgChatID   = server.arg("chat_id");
  cfgStaticIP = server.hasArg("static_ip");
  cfgIP       = server.arg("ip");
  cfgGateway  = server.arg("gateway");
  cfgSubnet   = server.arg("subnet");
  cfgDNS      = server.arg("dns");

  if (cfgSubnet.length() == 0) cfgSubnet = "255.255.255.0";
  if (cfgDNS.length() == 0)    cfgDNS = "8.8.8.8";

  saveSettings();
  server.send(200, "text/html", buildSavedPage());
  delay(2000);
  ESP.restart();
}

void handleFactoryReset() {
  String page = htmlHeader("Factory Reset");
  page += "<h1>Factory Reset Complete</h1>";
  page += "<div class='status warn'>";
  page += "All settings have been erased. The device is rebooting into setup mode.<br><br>";
  page += "Connect to WiFi <strong>" + String(AP_SSID) + "</strong> (password: <strong>" + String(AP_PASSWORD) + "</strong>)<br>";
  page += "Then open <strong>http://192.168.4.1</strong> to reconfigure.";
  page += "</div>";
  page += htmlFooter();
  server.send(200, "text/html", page);
  delay(2000);
  factoryReset();
}

void handleStatus() {
  String json = "{";
  json += "\"oil_low\":" + String(oilIsLow ? "true" : "false") + ",";
  json += "\"wifi_connected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",";
  json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"ssid\":\"" + cfgSSID + "\",";
  json += "\"uptime_sec\":" + String(millis() / 1000);
  json += "}";
  server.send(200, "application/json", json);
}

// =====================================================================
// WiFi connection
// =====================================================================

void startAPMode() {
  apMode = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_SUBNET);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.println("AP Mode started. SSID: " + String(AP_SSID));
  Serial.println("Connect to WiFi '" + String(AP_SSID) + "' password '" + String(AP_PASSWORD) + "'");
  Serial.println("Then open http://192.168.4.1");
}

bool connectWiFi() {
  Serial.print("Connecting to " + cfgSSID);
  WiFi.mode(WIFI_STA);

  if (cfgStaticIP && cfgIP.length() > 0) {
    IPAddress ip, gw, sn, dns;
    ip.fromString(cfgIP);
    gw.fromString(cfgGateway);
    sn.fromString(cfgSubnet);
    dns.fromString(cfgDNS);
    WiFi.config(ip, gw, sn, dns);
    Serial.print(" (static: " + cfgIP + ")");
  }

  WiFi.begin(cfgSSID.c_str(), cfgPassword.c_str());
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nConnected! IP: " + WiFi.localIP().toString());
    return true;
  }
  Serial.println("\nFailed to connect.");
  return false;
}

// =====================================================================
// Telegram
// =====================================================================

void initBot() {
  if (bot) delete bot;
  secured_client.setInsecure();
  bot = new UniversalTelegramBot(cfgBotToken.c_str(), secured_client);
}

bool sendTelegram(const String& message) {
  if (!bot || WiFi.status() != WL_CONNECTED) return false;
  bool sent = bot->sendMessage(cfgChatID.c_str(), message, "");
  Serial.println(sent ? ("Telegram sent: " + message) : "Telegram FAILED.");
  return sent;
}

// =====================================================================
// Sensor
// =====================================================================

bool readSensorDebounced() {
  // XKC-Y25-V: HIGH = liquid present, LOW = no liquid
  bool noLiquid = digitalRead(SENSOR_PIN) == LOW;

  if (noLiquid == lastRawReading) {
    if (debounceCounter < DEBOUNCE_COUNT) debounceCounter++;
  } else {
    debounceCounter = 1;
    lastRawReading = noLiquid;
  }

  if (debounceCounter >= DEBOUNCE_COUNT) {
    return noLiquid;
  }
  return oilIsLow;
}

// =====================================================================
// Setup & Loop
// =====================================================================

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== Oil Tank Monitor Starting ===");

  pinMode(SENSOR_PIN, INPUT);
  pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);

  // Check if BOOT button is held at startup for factory reset
  checkResetButton();

  loadSettings();

  if (!configured) {
    Serial.println("No configuration found — starting AP mode.");
    startAPMode();
  } else {
    if (!connectWiFi()) {
      Serial.println("WiFi failed — starting AP mode for reconfiguration.");
      startAPMode();
    } else {
      apMode = false;
      initBot();
      sendTelegram("🛢️ Oil tank monitor is ONLINE and watching the level.\nSettings: http://" + WiFi.localIP().toString());
    }
  }

  // Start web server in both modes
  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/status", handleStatus);
  server.on("/factory-reset", handleFactoryReset);
  server.begin();
  Serial.println("Web server started.");
}

void loop() {
  server.handleClient();
  checkResetButton();

  // Only run sensor logic when connected to WiFi (not in AP mode)
  if (apMode || WiFi.status() != WL_CONNECTED) {
    if (!apMode && WiFi.status() != WL_CONNECTED) {
      // Try to reconnect; fall back to AP if it fails
      if (!connectWiFi()) {
        startAPMode();
        server.begin();
      }
    }
    delay(100);
    return;
  }

  unsigned long now = millis();

  if (now - lastSensorCheck >= SENSOR_CHECK_MS) {
    lastSensorCheck = now;

    bool currentlyLow = readSensorDebounced();

    if (currentlyLow && !oilIsLow) {
      oilIsLow = true;
      Serial.println("OIL LOW detected!");
      sendTelegram("⚠️ OIL TANK LOW — the level has dropped below the sensor. Please refill.");
      lastAlertTime = now;
    } else if (!currentlyLow && oilIsLow) {
      oilIsLow = false;
      Serial.println("Oil level RESTORED.");
      sendTelegram("✅ Oil tank level restored — sensor detects oil above the line.");
    }
  }

  if (oilIsLow && (now - lastAlertTime >= ALERT_INTERVAL_MS)) {
    Serial.println("Sending hourly low-oil reminder.");
    sendTelegram("⚠️ REMINDER: Oil tank is still LOW. Please refill.");
    lastAlertTime = now;
  }

  delay(100);
}
