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
#include <Update.h>
#include <Wire.h>
#include <Adafruit_VL53L0X.h>

// ===== SENSOR ABSTRACTION =====
enum SensorType { SENSOR_DIGITAL = 0, SENSOR_TOF = 1 };

struct SensorReading {
  bool valid;            // false on hardware fault (I2C timeout, ToF out-of-range)
  bool digitalState;     // HIGH = object/liquid present; meaningful when SENSOR_DIGITAL
  uint16_t distanceMm;   // mm to puck; meaningful when SENSOR_TOF
};

enum LevelState {
  LEVEL_LOW,           // distance > cfgTofLow  (or digital: no liquid)
  LEVEL_BELOW_HALF,    // cfgTofHalf < distance <= cfgTofLow
  LEVEL_ABOVE_HALF,    // cfgTofHigh < distance <= cfgTofHalf
  LEVEL_HIGH,          // distance <= cfgTofHigh (digital: liquid present)
  LEVEL_UNKNOWN        // no valid reading yet (boot)
};

const char* levelStateName(LevelState s) {
  switch (s) {
    case LEVEL_LOW:        return "LOW";
    case LEVEL_BELOW_HALF: return "BELOW_HALF";
    case LEVEL_ABOVE_HALF: return "ABOVE_HALF";
    case LEVEL_HIGH:       return "HIGH";
    default:               return "UNKNOWN";
  }
}

const int I2C_SDA_PIN = 21;            // ESP32 default
const int I2C_SCL_PIN = 22;            // ESP32 default
const int TOF_HYSTERESIS_MM = 5;       // band around each threshold
const int TOF_MIN_MM = 30;             // VL53L0X spec lower bound
const int TOF_MAX_MM = 2000;           // VL53L0X spec upper bound (mm we accept)
const int TOF_FAULT_CYCLES = 5;        // consecutive invalid reads → fault alert
const int TOF_RECOVERY_CYCLES = 3;     // consecutive invalid reads → I2C reinit attempt

// ===== FIRMWARE VERSION =====
const char* FW_VERSION = "1.1.0";

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
String cfgChatID2;
String cfgChatID3;
bool   cfgStaticIP    = false;
String cfgIP;
String cfgGateway;
String cfgSubnet;
String cfgDNS;
const char* WEB_USER  = "admin";
String cfgWebPassword = "admin";

// Sensor configuration
SensorType cfgSensorType = SENSOR_DIGITAL;   // default — protects v1.x upgraders
uint16_t cfgTofLow  = 200;                   // mm — alert threshold
uint16_t cfgTofHalf = 130;                   // mm — half mark
uint16_t cfgTofHigh = 60;                    // mm — refill complete

LevelState currentState = LEVEL_UNKNOWN;

// Sensor state
unsigned long lastAlertTime   = 0;
unsigned long lastSensorCheck = 0;
bool oilIsLow                = false;

// Session management
const unsigned long SESSION_TIMEOUT_MS = 900000UL;  // 15 minutes
unsigned long sessionToken    = 0;   // 0 = no active session
unsigned long lastActivityTime = 0;

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
  cfgChatID2   = prefs.getString("chat_id2", "");
  cfgChatID3   = prefs.getString("chat_id3", "");
  cfgStaticIP  = prefs.getBool("static_ip", false);
  cfgIP        = prefs.getString("ip", "");
  cfgGateway   = prefs.getString("gateway", "");
  cfgSubnet    = prefs.getString("subnet", "255.255.255.0");
  cfgDNS         = prefs.getString("dns", "8.8.8.8");
  cfgWebPassword = prefs.getString("web_pass", "admin");
  cfgSensorType = (SensorType)prefs.getUChar("sensor_type", 0);
  cfgTofLow     = prefs.getUShort("tof_low", 200);
  cfgTofHalf    = prefs.getUShort("tof_half", 130);
  cfgTofHigh    = prefs.getUShort("tof_high", 60);
  prefs.end();
  configured = (cfgSSID.length() > 0 && cfgBotToken.length() > 0 && cfgChatID.length() > 0);
}

void saveSettings() {
  prefs.begin("oilmon", false);  // read-write
  prefs.putString("ssid", cfgSSID);
  prefs.putString("password", cfgPassword);
  prefs.putString("bot_token", cfgBotToken);
  prefs.putString("chat_id", cfgChatID);
  prefs.putString("chat_id2", cfgChatID2);
  prefs.putString("chat_id3", cfgChatID3);
  prefs.putBool("static_ip", cfgStaticIP);
  prefs.putString("ip", cfgIP);
  prefs.putString("gateway", cfgGateway);
  prefs.putString("subnet", cfgSubnet);
  prefs.putString("dns", cfgDNS);
  prefs.putString("web_pass", cfgWebPassword);
  prefs.putUChar("sensor_type", (uint8_t)cfgSensorType);
  prefs.putUShort("tof_low", cfgTofLow);
  prefs.putUShort("tof_half", cfgTofHalf);
  prefs.putUShort("tof_high", cfgTofHigh);
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
  h += ".ip-fields,.tof-fields{display:none;}.ip-fields.show,.tof-fields.show{display:block;}";
  h += "a{color:#16c79a;}";
  h += ".nav{margin:16px 0;font-size:0.9em;}";
  h += ".eye-btn{background:none;border:none;color:#e0e0e0;cursor:pointer;font-size:1.2em;padding:8px;margin-top:0;width:auto;}";
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
    page += "<strong>Oil Level:</strong> " + String(oilIsLow ? "LOW" : "OK") + "<br>";
    page += "<strong>Firmware:</strong> v" + String(FW_VERSION);
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
  page += "<div style='display:flex;gap:8px;align-items:center;'>";
  page += "<input type='password' id='bot_token' name='bot_token' value='" + cfgBotToken + "' placeholder='123456789:ABCdef...' required style='flex:1;'>";
  page += "<button type='button' class='eye-btn' onclick=\"toggleVis('bot_token',this)\">&#128065;</button>";
  page += "</div>";
  page += "<label for='chat_id'>Chat ID</label>";
  page += "<div style='display:flex;gap:8px;align-items:center;'>";
  page += "<input type='password' id='chat_id' name='chat_id' value='" + cfgChatID + "' placeholder='123456789' required style='flex:1;'>";
  page += "<button type='button' class='eye-btn' onclick=\"toggleVis('chat_id',this)\">&#128065;</button>";
  page += "<button type='button' onclick=\"addChatID()\" style='width:40px;padding:10px;margin-top:0;background:#16c79a;font-size:1.2em;'>+</button>";
  page += "</div>";

  // Chat ID 2 (hidden if empty)
  page += "<div id='chat2-row' style='display:" + String(cfgChatID2.length() > 0 ? "flex" : "none") + ";gap:8px;align-items:center;margin-top:8px;'>";
  page += "<input type='password' id='chat_id2' name='chat_id2' value='" + cfgChatID2 + "' placeholder='Chat ID 2 (optional)' style='flex:1;'>";
  page += "<button type='button' class='eye-btn' onclick=\"toggleVis('chat_id2',this)\">&#128065;</button>";
  page += "<button type='button' onclick=\"addChatID()\" style='width:40px;padding:10px;margin-top:0;background:#16c79a;font-size:1.2em;'>+</button>";
  page += "<button type='button' onclick=\"removeChatID(2)\" style='width:40px;padding:10px;margin-top:0;background:#e94560;font-size:1.2em;'>-</button>";
  page += "</div>";

  // Chat ID 3 (hidden if empty)
  page += "<div id='chat3-row' style='display:" + String(cfgChatID3.length() > 0 ? "flex" : "none") + ";gap:8px;align-items:center;margin-top:8px;'>";
  page += "<input type='password' id='chat_id3' name='chat_id3' value='" + cfgChatID3 + "' placeholder='Chat ID 3 (optional)' style='flex:1;'>";
  page += "<button type='button' class='eye-btn' onclick=\"toggleVis('chat_id3',this)\">&#128065;</button>";
  page += "<button type='button' onclick=\"removeChatID(3)\" style='width:40px;padding:10px;margin-top:0;background:#e94560;font-size:1.2em;'>-</button>";
  page += "</div>";

  // Sensor Configuration section
  page += "<h2>Sensor Configuration</h2>";
  page += "<label for='sensor_type'>Sensor Type</label>";
  page += "<select id='sensor_type' name='sensor_type' onchange='toggleTof()' style='width:100%;padding:10px;background:#16213e;color:#e0e0e0;border:1px solid #333;border-radius:6px;'>";
  page += "<option value='0'" + String(cfgSensorType == SENSOR_DIGITAL ? " selected" : "") + ">Digital threshold (XKC-Y25-V, IR break-beam, reed switch, etc.)</option>";
  page += "<option value='1'" + String(cfgSensorType == SENSOR_TOF ? " selected" : "") + ">ToF distance (VL53L0X)</option>";
  page += "</select>";

  page += "<div class='tof-fields" + String(cfgSensorType == SENSOR_TOF ? " show" : "") + "' id='tof-fields'>";
  page += "<div class='status' id='tof-live' style='margin-top:12px;'>Current Reading: <span id='tof-distance'>—</span> mm</div>";
  page += "<label for='tof_low'>LOW threshold (mm) — alert below this</label>";
  page += "<input type='text' id='tof_low' name='tof_low' value='" + String(cfgTofLow) + "'>";
  page += "<label for='tof_half'>HALF threshold (mm)</label>";
  page += "<input type='text' id='tof_half' name='tof_half' value='" + String(cfgTofHalf) + "'>";
  page += "<label for='tof_high'>HIGH threshold (mm) — refill complete above this</label>";
  page += "<input type='text' id='tof_high' name='tof_high' value='" + String(cfgTofHigh) + "'>";
  page += "<p style='font-size:0.85em;color:#999;'>Smaller mm = puck closer to sensor (fuller tank). Must satisfy HIGH &lt; HALF &lt; LOW. Range: 30–2000 mm.</p>";
  page += "</div>";

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

  // Web interface password section
  page += "<h2>Web Interface Password</h2>";
  page += "<label>Username</label>";
  page += "<input type='text' value='admin' disabled style='opacity:0.6;'>";
  page += "<label for='web_pass'>Password</label>";
  page += "<input type='password' id='web_pass' name='web_pass' value='" + cfgWebPassword + "'>";

  page += "<button type='submit'>Save &amp; Restart</button>";
  page += "</form>";

  // Firmware update link
  page += "<h2>Firmware</h2>";
  page += "<p style='font-size:0.9em;'>Current version: <strong>v" + String(FW_VERSION) + "</strong></p>";
  page += "<a href='/update' style='display:block;text-align:center;padding:12px;background:#0f3460;";
  page += "border-radius:6px;color:#e0e0e0;text-decoration:none;'>Upload Firmware Update</a>";

  // Logout and danger zone
  page += "<div style='margin-top:24px;text-align:center;'>";
  page += "<a href='/logout' style='color:#e0e0e0;font-size:0.9em;'>Log Out</a>";
  page += "</div>";

  page += "<h2 style='margin-top:40px;color:#e94560;'>Danger Zone</h2>";
  page += "<button style='background:#333;border:1px solid #e94560;' ";
  page += "onclick=\"if(confirm('Are you sure you want to factory reset? This will erase ALL settings (WiFi, Telegram, network) and reboot into setup mode.')){window.location='/factory-reset'}\">";
  page += "Factory Reset</button>";

  page += "<script>";
  page += "function toggleStatic(){";
  page += "  document.getElementById('ip-fields').classList.toggle('show',";
  page += "    document.getElementById('static_ip').checked);}";
  page += "function toggleTof(){";
  page += "  var v=document.getElementById('sensor_type').value;";
  page += "  document.getElementById('tof-fields').classList.toggle('show', v==='1');";
  page += "}";
  page += "var tofPoll=null;";
  page += "function startTofPoll(){";
  page += "  if(tofPoll) clearInterval(tofPoll);";
  page += "  tofPoll=setInterval(function(){";
  page += "    if(document.getElementById('sensor_type').value!=='1'){clearInterval(tofPoll);tofPoll=null;return;}";
  page += "    fetch('/status').then(r=>r.json()).then(j=>{";
  page += "      var el=document.getElementById('tof-distance');";
  page += "      if(el && j.distance_mm!==undefined){el.textContent=j.distance_mm;}";
  page += "      else if(el){el.textContent='—';}";
  page += "    }).catch(()=>{});";
  page += "  },2000);";
  page += "}";
  page += "if(document.getElementById('sensor_type').value==='1'){startTofPoll();}";
  page += "document.getElementById('sensor_type').addEventListener('change',function(){";
  page += "  if(this.value==='1') startTofPoll();";
  page += "});";
  page += "function toggleVis(id,btn){";
  page += "  var f=document.getElementById(id);";
  page += "  if(f.type==='password'){f.type='text';btn.style.opacity='0.5';}";
  page += "  else{f.type='password';btn.style.opacity='1';}}";
  page += "function addChatID(){";
  page += "  var r2=document.getElementById('chat2-row');";
  page += "  var r3=document.getElementById('chat3-row');";
  page += "  if(r2.style.display==='none'){r2.style.display='flex';}";
  page += "  else if(r3.style.display==='none'){r3.style.display='flex';}";
  page += "}";
  page += "function removeChatID(n){";
  page += "  var r=document.getElementById('chat'+n+'-row');";
  page += "  r.style.display='none';";
  page += "  document.getElementById('chat_id'+n).value='';";
  page += "}";
  page += "</script>";

  page += htmlFooter();
  return page;
}

String buildSavedPage() {
  String targetIP = cfgStaticIP && cfgIP.length() > 0 ? cfgIP : WiFi.localIP().toString();
  String targetURL = "http://" + targetIP;

  String page = htmlHeader("Settings Saved");
  page += "<h1>Settings Saved</h1>";
  page += "<div class='status'>";
  page += "Configuration saved. The device is restarting and connecting to <strong>" + cfgSSID + "</strong>.<br><br>";
  page += "Redirecting to <strong>" + targetURL + "</strong> in <span id='countdown'>15</span> seconds...";
  page += "</div>";
  page += "<script>";
  page += "var sec=15;var el=document.getElementById('countdown');";
  page += "var t=setInterval(function(){sec--;el.textContent=sec;";
  page += "if(sec<=0){clearInterval(t);window.location='" + targetURL + "';}";
  page += "},1000);";
  page += "</script>";
  page += htmlFooter();
  return page;
}

String buildUpdatePage() {
  String page = htmlHeader("Firmware Update");
  page += "<h1>Firmware Update</h1>";
  page += "<div class='status'>";
  page += "<strong>Current Version:</strong> v" + String(FW_VERSION) + "<br>";
  page += "<strong>Free Space:</strong> " + String(ESP.getFreeSketchSpace() / 1024) + " KB";
  page += "</div>";
  page += "<p style='font-size:0.9em;'>Upload a compiled <code>.bin</code> firmware file. The device will flash itself and reboot.</p>";
  page += "<form method='POST' action='/update' enctype='multipart/form-data'>";
  page += "<label for='firmware'>Firmware File (.bin)</label>";
  page += "<input type='file' id='firmware' name='firmware' accept='.bin' required ";
  page += "style='padding:10px;background:#16213e;border:1px solid #333;border-radius:6px;width:100%;box-sizing:border-box;'>";
  page += "<button type='submit' onclick=\"this.innerText='Uploading... do not power off';this.disabled=true;this.form.submit();\">Upload &amp; Install</button>";
  page += "</form>";
  page += "<div class='nav' style='margin-top:16px;'><a href='/'>&larr; Back to Settings</a></div>";
  page += htmlFooter();
  return page;
}

String buildUpdateResultPage(bool success, const String& message) {
  String page = htmlHeader("Update " + String(success ? "Complete" : "Failed"));
  page += "<h1>Firmware Update " + String(success ? "Complete" : "Failed") + "</h1>";
  page += "<div class='status" + String(success ? "" : " warn") + "'>";
  page += message;
  if (success) {
    page += "<br><br>Redirecting in <span id='countdown'>15</span> seconds...";
    page += "</div>";
    page += "<script>";
    page += "var sec=15;var el=document.getElementById('countdown');";
    page += "var t=setInterval(function(){sec--;el.textContent=sec;";
    page += "if(sec<=0){clearInterval(t);window.location='/';}";
    page += "},1000);";
    page += "</script>";
  } else {
    page += "</div>";
    page += "<div class='nav' style='margin-top:16px;'><a href='/update'>&larr; Try Again</a></div>";
  }
  page += htmlFooter();
  return page;
}

// =====================================================================
// Session management
// =====================================================================

unsigned long generateToken() {
  unsigned long t = esp_random();
  if (t == 0) t = 1;  // reserve 0 for "no session"
  return t;
}

String getSessionCookie() {
  if (!server.hasHeader("Cookie")) return "";
  String cookies = server.header("Cookie");
  int idx = cookies.indexOf("session=");
  if (idx < 0) return "";
  int start = idx + 8;
  int end = cookies.indexOf(';', start);
  if (end < 0) end = cookies.length();
  return cookies.substring(start, end);
}

bool isSessionValid() {
  if (sessionToken == 0) return false;
  String cookie = getSessionCookie();
  if (cookie != String(sessionToken)) return false;
  if (millis() - lastActivityTime > SESSION_TIMEOUT_MS) {
    sessionToken = 0;  // expired
    return false;
  }
  lastActivityTime = millis();  // refresh on activity
  return true;
}

bool requireAuth() {
  if (apMode) return true;
  if (!isSessionValid()) {
    server.sendHeader("Location", "/login");
    server.send(302, "text/plain", "Redirecting to login...");
    return false;
  }
  return true;
}

String buildLoginPage(bool failed) {
  String page = htmlHeader("Oil Tank Monitor - Login");
  page += "<h1>Oil Tank Monitor</h1>";
  if (failed) {
    page += "<div class='status warn'>Invalid password. Try again.</div>";
  }
  page += "<form method='POST' action='/login'>";
  page += "<label for='username'>Username</label>";
  page += "<input type='text' id='username' value='admin' disabled style='opacity:0.6;'>";
  page += "<label for='password'>Password</label>";
  page += "<input type='password' id='password' name='password' autofocus required>";
  page += "<button type='submit'>Log In</button>";
  page += "</form>";
  page += htmlFooter();
  return page;
}

// =====================================================================
// Web server handlers
// =====================================================================

void handleLogin() {
  if (server.method() == HTTP_POST) {
    String password = server.arg("password");
    if (password == cfgWebPassword) {
      sessionToken = generateToken();
      lastActivityTime = millis();
      server.sendHeader("Set-Cookie", "session=" + String(sessionToken) + "; Path=/; HttpOnly");
      server.sendHeader("Location", "/");
      server.send(302, "text/plain", "Login successful");
      return;
    }
    server.send(200, "text/html", buildLoginPage(true));
    return;
  }
  server.send(200, "text/html", buildLoginPage(false));
}

void handleLogout() {
  sessionToken = 0;
  server.sendHeader("Set-Cookie", "session=; Path=/; Max-Age=0; HttpOnly");
  server.sendHeader("Location", "/login");
  server.send(302, "text/plain", "Logged out");
}

void handleRoot() {
  if (!requireAuth()) return;
  server.send(200, "text/html", buildConfigPage());
}

void handleSave() {
  if (!requireAuth()) return;
  cfgSSID     = server.arg("ssid");
  cfgPassword = server.arg("password");
  cfgBotToken = server.arg("bot_token");
  cfgChatID   = server.arg("chat_id");
  cfgChatID2  = server.arg("chat_id2");
  cfgChatID3  = server.arg("chat_id3");
  cfgStaticIP = server.hasArg("static_ip");
  cfgIP       = server.arg("ip");
  cfgGateway  = server.arg("gateway");
  cfgSubnet   = server.arg("subnet");
  cfgDNS      = server.arg("dns");

  // Sensor configuration
  if (server.hasArg("sensor_type")) {
    int t = server.arg("sensor_type").toInt();
    cfgSensorType = (t == 1) ? SENSOR_TOF : SENSOR_DIGITAL;
  }
  if (server.hasArg("tof_low"))  cfgTofLow  = server.arg("tof_low").toInt();
  if (server.hasArg("tof_half")) cfgTofHalf = server.arg("tof_half").toInt();
  if (server.hasArg("tof_high")) cfgTofHigh = server.arg("tof_high").toInt();

  if (cfgSubnet.length() == 0) cfgSubnet = "255.255.255.0";
  if (cfgDNS.length() == 0)    cfgDNS = "8.8.8.8";

  String newPass = server.arg("web_pass");
  if (newPass.length() > 0) cfgWebPassword = newPass;

  saveSettings();
  server.send(200, "text/html", buildSavedPage());
  delay(2000);
  ESP.restart();
}

void handleFactoryReset() {
  if (!requireAuth()) return;
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
  if (!requireAuth()) return;
  String json = "{";
  json += "\"oil_low\":" + String(oilIsLow ? "true" : "false") + ",";
  json += "\"wifi_connected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",";
  json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"ssid\":\"" + cfgSSID + "\",";
  json += "\"uptime_sec\":" + String(millis() / 1000) + ",";
  json += "\"firmware\":\"" + String(FW_VERSION) + "\"";
  json += "}";
  server.send(200, "application/json", json);
}

void handleUpdatePage() {
  if (!requireAuth()) return;
  server.send(200, "text/html", buildUpdatePage());
}

void handleUpdateUpload() {
  if (!requireAuth()) return;
  HTTPUpload& upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    Serial.println("OTA update starting: " + upload.filename);
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      Serial.println("OTA update success: " + String(upload.totalSize) + " bytes");
    } else {
      Update.printError(Serial);
    }
  }
}

void handleUpdateResult() {
  if (!requireAuth()) return;
  bool success = !Update.hasError();
  if (success) {
    server.send(200, "text/html", buildUpdateResultPage(true, "Firmware updated successfully."));
    delay(2000);
    ESP.restart();
  } else {
    server.send(200, "text/html", buildUpdateResultPage(false, "Update failed. Please try again with a valid .bin file."));
  }
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
  bool anySent = false;
  String ids[] = {cfgChatID, cfgChatID2, cfgChatID3};
  for (int i = 0; i < 3; i++) {
    if (ids[i].length() > 0) {
      bool sent = bot->sendMessage(ids[i].c_str(), message, "");
      Serial.println(sent ? ("Telegram sent to " + ids[i]) : ("Telegram FAILED for " + ids[i]));
      if (sent) anySent = true;
    }
  }
  return anySent;
}

// =====================================================================
// Sensor — dispatch on cfgSensorType
// =====================================================================

Adafruit_VL53L0X tofSensor;     // unused until Task 4 wires the ToF path
int tofInvalidCount = 0;        // consecutive invalid reads (Task 9)
bool sensorFaultActive = false; // Task 9

bool initSensor() {
  if (cfgSensorType == SENSOR_DIGITAL) {
    pinMode(SENSOR_PIN, INPUT);
    Serial.println("Sensor: DIGITAL on GPIO" + String(SENSOR_PIN));
    return true;
  }
  // ToF path — implemented in Task 4
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  if (!tofSensor.begin()) {
    Serial.println("Sensor: TOF init FAILED — check I2C wiring (SDA=21, SCL=22, VCC=3V3, GND=GND)");
    return false;
  }
  Serial.println("Sensor: TOF (VL53L0X) on I2C SDA=" + String(I2C_SDA_PIN) + " SCL=" + String(I2C_SCL_PIN));
  return true;
}

SensorReading readSensorRaw() {
  SensorReading r = { false, false, 0 };
  if (cfgSensorType == SENSOR_DIGITAL) {
    r.digitalState = (digitalRead(SENSOR_PIN) == HIGH);
    r.valid = true;
    return r;
  }
  // ToF path — implemented in Task 4
  VL53L0X_RangingMeasurementData_t data;
  tofSensor.rangingTest(&data, false);
  if (data.RangeStatus == 4) {       // out of range / no signal
    return r;                         // r.valid stays false
  }
  uint16_t mm = data.RangeMilliMeter;
  if (mm < TOF_MIN_MM || mm > TOF_MAX_MM) {
    return r;                         // out of accepted range
  }
  r.distanceMm = mm;
  r.valid = true;
  return r;
}

uint16_t medianOf3(uint16_t a, uint16_t b, uint16_t c) {
  if ((a >= b && a <= c) || (a <= b && a >= c)) return a;
  if ((b >= a && b <= c) || (b <= a && b >= c)) return b;
  return c;
}

// Existing 3-read debounce, now operating on SensorReading
SensorReading readSensor() {
  static int debounce = 0;
  static SensorReading lastRaw = { false, false, 0 };
  SensorReading raw = readSensorRaw();

  if (cfgSensorType == SENSOR_DIGITAL) {
    if (raw.digitalState == lastRaw.digitalState && raw.valid) {
      if (debounce < DEBOUNCE_COUNT) debounce++;
    } else {
      debounce = 1;
      lastRaw = raw;
    }
    if (debounce >= DEBOUNCE_COUNT) return raw;
    // Not yet stable — return last accepted reading
    SensorReading last = { true, !oilIsLow, 0 };  // oilIsLow tracks "no liquid"
    return last;
  }
  // ToF: take 3 readings ~50 ms apart, return the median (rejects single-shot spikes)
  SensorReading r1 = raw;
  delay(50);
  SensorReading r2 = readSensorRaw();
  delay(50);
  SensorReading r3 = readSensorRaw();
  int validCount = (int)r1.valid + (int)r2.valid + (int)r3.valid;
  SensorReading out = { false, false, 0 };
  if (validCount >= 2) {
    // Use only valid readings — substitute equal values for invalids so median works
    uint16_t a = r1.valid ? r1.distanceMm : (r2.valid ? r2.distanceMm : r3.distanceMm);
    uint16_t b = r2.valid ? r2.distanceMm : (r1.valid ? r1.distanceMm : r3.distanceMm);
    uint16_t c = r3.valid ? r3.distanceMm : (r1.valid ? r1.distanceMm : r2.distanceMm);
    out.distanceMm = medianOf3(a, b, c);
    out.valid = true;
  }
  return out;
}

// Map a SensorReading to a LevelState, applying hysteresis around ToF thresholds.
// `prev` is the current state — used to bias the bucketing toward stability when the
// reading sits near a threshold.
LevelState bucketReading(const SensorReading& r, LevelState prev) {
  if (!r.valid) return prev;   // hold last state on a single bad reading
  if (cfgSensorType == SENSOR_DIGITAL) {
    // Digital model: only LOW or HIGH (HIGH = liquid present)
    return r.digitalState ? LEVEL_HIGH : LEVEL_LOW;
  }
  // ToF: distance in mm; smaller = fuller. Hysteresis: require crossing by ±H.
  uint16_t d = r.distanceMm;
  uint16_t hyst = TOF_HYSTERESIS_MM;
  // Adjust thresholds based on direction of approach.
  // Going UP into a fuller state (smaller d): use lowerBound = threshold - hyst.
  // Going DOWN into an emptier state (larger d): use upperBound = threshold + hyst.
  // We pick effective thresholds that resist re-crossing in the opposite direction.
  uint16_t lowT  = cfgTofLow;
  uint16_t halfT = cfgTofHalf;
  uint16_t highT = cfgTofHigh;
  switch (prev) {
    case LEVEL_LOW:        if (d >  lowT  - hyst) return LEVEL_LOW;
                            if (d >  halfT - hyst) return LEVEL_BELOW_HALF;
                            if (d >  highT - hyst) return LEVEL_ABOVE_HALF;
                            return LEVEL_HIGH;
    case LEVEL_BELOW_HALF: if (d >  lowT  + hyst) return LEVEL_LOW;
                            if (d >  halfT - hyst) return LEVEL_BELOW_HALF;
                            if (d >  highT - hyst) return LEVEL_ABOVE_HALF;
                            return LEVEL_HIGH;
    case LEVEL_ABOVE_HALF: if (d >  lowT  + hyst) return LEVEL_LOW;
                            if (d >  halfT + hyst) return LEVEL_BELOW_HALF;
                            if (d >  highT - hyst) return LEVEL_ABOVE_HALF;
                            return LEVEL_HIGH;
    case LEVEL_HIGH:       if (d >  lowT  + hyst) return LEVEL_LOW;
                            if (d >  halfT + hyst) return LEVEL_BELOW_HALF;
                            if (d >  highT + hyst) return LEVEL_ABOVE_HALF;
                            return LEVEL_HIGH;
    default:               // LEVEL_UNKNOWN — initial bucketing without hysteresis bias
                            if (d >  lowT)  return LEVEL_LOW;
                            if (d >  halfT) return LEVEL_BELOW_HALF;
                            if (d >  highT) return LEVEL_ABOVE_HALF;
                            return LEVEL_HIGH;
  }
}

// =====================================================================
// Setup & Loop
// =====================================================================

void fireTransitionMessage(LevelState from, LevelState to) {
  if (cfgSensorType == SENSOR_DIGITAL) {
    // Digital: only LOW <-> HIGH transitions are meaningful
    if (from == LEVEL_LOW && to == LEVEL_HIGH) {
      sendTelegram("✅ Oil tank level restored — sensor detects oil above the line.");
    } else if (from == LEVEL_HIGH && to == LEVEL_LOW) {
      sendTelegram("⚠️ OIL TANK LOW — the level has dropped below the sensor. Please refill.");
    }
    return;
  }
  // ToF: full multi-state transition matrix
  if (from == LEVEL_LOW && to >= LEVEL_BELOW_HALF) {
    sendTelegram("✅ Oil tank above low mark — refill detected.");
  }
  if (from <= LEVEL_BELOW_HALF && to >= LEVEL_ABOVE_HALF) {
    sendTelegram("🔼 Tank is half refilled.");
  }
  if (from <= LEVEL_ABOVE_HALF && to == LEVEL_HIGH) {
    sendTelegram("🔝 Tank at HIGH — refill complete.");
  }
  if (from >= LEVEL_ABOVE_HALF && to <= LEVEL_BELOW_HALF && to != LEVEL_LOW) {
    sendTelegram("📉 Tank is half empty — plan a refill.");
  }
  if (from >= LEVEL_BELOW_HALF && to == LEVEL_LOW) {
    sendTelegram("⚠️ OIL TANK LOW — please refill.");
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== Oil Tank Monitor Starting ===");

  initSensor();
  pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);

  // Check if BOOT button is held at startup for factory reset
  checkResetButton();

  loadSettings();
  Serial.printf("Sensor type: %s | ToF thresholds (mm): low=%u half=%u high=%u\n",
                cfgSensorType == SENSOR_DIGITAL ? "DIGITAL" : "TOF",
                cfgTofLow, cfgTofHalf, cfgTofHigh);

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
      // If ToF init silently failed earlier, alert now that we have Telegram
      if (cfgSensorType == SENSOR_TOF && !tofSensor.begin()) {
        sendTelegram("⚠️ ToF init failed — check I2C wiring or switch sensor type via web UI.");
      }
      // Take an initial sensor reading to establish currentState before announcing online.
      delay(200);  // let sensor stabilize
      SensorReading r = readSensor();
      currentState = bucketReading(r, LEVEL_UNKNOWN);
      Serial.printf("Boot: initial state=%s (after WiFi connect)\n", levelStateName(currentState));

      String msg = "🛢️ Oil tank monitor is ONLINE.\nSensor: ";
      msg += (cfgSensorType == SENSOR_DIGITAL ? "Digital" : "ToF");
      msg += "\nLevel: " + String(levelStateName(currentState));
      if (cfgSensorType == SENSOR_TOF && r.valid) {
        msg += " (" + String(r.distanceMm) + "mm)";
      }
      msg += "\nSettings: http://" + WiFi.localIP().toString();
      sendTelegram(msg);
    }
  }

  // Start web server in both modes
  const char* headerKeys[] = {"Cookie"};
  server.collectHeaders(headerKeys, 1);
  server.on("/login", handleLogin);
  server.on("/logout", handleLogout);
  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/status", handleStatus);
  server.on("/update", HTTP_GET, handleUpdatePage);
  server.on("/update", HTTP_POST, handleUpdateResult, handleUpdateUpload);
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

    SensorReading reading = readSensor();
    // Fault tracking — only meaningful for ToF (digital readSensor() returns valid=true always)
    if (cfgSensorType == SENSOR_TOF) {
      if (!reading.valid) {
        tofInvalidCount++;
        if (tofInvalidCount == TOF_RECOVERY_CYCLES) {
          Serial.println("ToF: attempting I2C bus recovery");
          Wire.end();
          Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
          tofSensor.begin();
        }
        if (tofInvalidCount == TOF_FAULT_CYCLES && !sensorFaultActive) {
          sensorFaultActive = true;
          sendTelegram("⚠️ Sensor fault — no valid reading for 25s. Check wiring.");
        }
      } else {
        if (sensorFaultActive) {
          Serial.println("ToF: readings recovered");
          sensorFaultActive = false;
        }
        tofInvalidCount = 0;
      }
    }
    LevelState newState = bucketReading(reading, currentState);

    if (currentState == LEVEL_UNKNOWN && newState != LEVEL_UNKNOWN) {
      // Boot: first valid reading initializes silently. Online message handled in setup().
      currentState = newState;
      Serial.printf("Boot: initial level=%s\n", levelStateName(currentState));
    } else if (newState != currentState && newState != LEVEL_UNKNOWN) {
      Serial.printf("Transition: %s -> %s\n", levelStateName(currentState), levelStateName(newState));
      fireTransitionMessage(currentState, newState);
      currentState = newState;
      if (newState == LEVEL_LOW) lastAlertTime = now;
    }

    // Maintain legacy oilIsLow for /status backward compat
    oilIsLow = (currentState == LEVEL_LOW);
  }

  // Hourly LOW reminder — same behavior as v1.x, now keyed on currentState
  if (currentState == LEVEL_LOW && (now - lastAlertTime >= ALERT_INTERVAL_MS)) {
    Serial.println("Sending hourly low-oil reminder.");
    sendTelegram("⚠️ REMINDER: Oil tank is still LOW. Please refill.");
    lastAlertTime = now;
  }

  delay(100);
}
