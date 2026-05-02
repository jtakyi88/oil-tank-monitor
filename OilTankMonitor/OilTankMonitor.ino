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
#include <Adafruit_VL53L1X.h>
#include "esp_task_wdt.h"

// ===== SENSOR ABSTRACTION =====
enum SensorType { SENSOR_DIGITAL = 0, SENSOR_TOF = 1, SENSOR_IR_BREAK = 2 };
enum TofChip { TOF_NONE, TOF_VL53L1X };
enum Units { UNITS_METRIC = 0, UNITS_IMPERIAL = 1 };

struct SensorReading {
  bool valid;            // false on hardware fault (I2C timeout, ToF out-of-range)
  bool digitalState;     // HIGH = object/liquid present (DIGITAL) or beam clear (IR_BREAK); meaningful when SENSOR_DIGITAL or SENSOR_IR_BREAK
  uint16_t distanceMm;   // mm to puck; meaningful when SENSOR_TOF
};

enum LevelState {
  LEVEL_LOW,           // distance > cfgTofLow  (or digital: no liquid)
  LEVEL_BELOW_HALF,    // cfgTofHalf < distance <= cfgTofLow
  LEVEL_ABOVE_HALF,    // cfgTofHigh < distance <= cfgTofHalf
  LEVEL_HIGH,          // distance <= cfgTofHigh (digital: liquid present)
  LEVEL_OIL_OK,        // IR break-beam: beam clear (puck above low-oil mark)
  LEVEL_OIL_LOW,       // IR break-beam: beam broken (puck has reached the mark)
  LEVEL_UNKNOWN        // no valid reading yet (boot)
};

const char* levelStateName(LevelState s) {
  switch (s) {
    case LEVEL_LOW:        return "LOW";
    case LEVEL_BELOW_HALF: return "BELOW_HALF";
    case LEVEL_ABOVE_HALF: return "ABOVE_HALF";
    case LEVEL_HIGH:       return "HIGH";
    case LEVEL_OIL_OK:     return "OIL_OK";
    case LEVEL_OIL_LOW:    return "OIL_LOW";
    case LEVEL_UNKNOWN:    return "UNKNOWN";
  }
  return "UNKNOWN";
}

const char* sensorTypeJsonName(SensorType t) {
  switch (t) {
    case SENSOR_TOF:      return "tof";
    case SENSOR_IR_BREAK: return "ir_break";
    default:              return "digital";
  }
}

const char* sensorTypeBootName(SensorType t) {
  switch (t) {
    case SENSOR_TOF:      return "TOF";
    case SENSOR_IR_BREAK: return "IR_BREAK";
    default:              return "DIGITAL";
  }
}

const char* sensorTypeDisplayName(SensorType t) {
  switch (t) {
    case SENSOR_TOF:      return "ToF";
    case SENSOR_IR_BREAK: return "IR break-beam (sight gauge)";
    default:              return "Digital";
  }
}

extern Units cfgUnits;  // defined with runtime state below

const char* unitsLabel(Units u) {
  return (u == UNITS_IMPERIAL) ? "in" : "mm";
}

String formatDistance(uint16_t mm) {
  if (cfgUnits == UNITS_IMPERIAL) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%.2f", mm / 25.4f);
    return String(buf);
  }
  return String(mm);
}

bool parseDistanceInput(const String& s, uint16_t& outMm) {
  if (s.length() == 0) return false;
  float v = s.toFloat();
  if (v <= 0.0f) return false;  // toFloat() returns 0 on parse failure; also rejects literal 0
  if (cfgUnits == UNITS_IMPERIAL) {
    long mm = lroundf(v * 25.4f);
    if (mm < 0 || mm > 65535) return false;
    outMm = (uint16_t)mm;
  } else {
    long mm = (long)v;
    if (mm < 0 || mm > 65535) return false;
    outMm = (uint16_t)mm;
  }
  return true;
}

const int I2C_SDA_PIN = 21;            // ESP32 default
const int I2C_SCL_PIN = 22;            // ESP32 default
const int TOF_HYSTERESIS_MM = 5;       // band around each threshold
const int TOF_MIN_MM = 30;             // VL53L1X spec lower bound
const int TOF_MAX_MM = 4000;           // VL53L1X long-mode upper bound
const int TOF_FAULT_CYCLES = 5;        // consecutive invalid reads → fault alert
const int TOF_RECOVERY_CYCLES = 3;     // consecutive invalid reads → I2C reinit attempt

// ===== FIRMWARE VERSION =====
const char* FW_VERSION = "2.0.0";

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
Units cfgUnits = UNITS_METRIC;               // default — preserves pre-toggle output
uint16_t cfgTofLow  = 200;                   // mm — alert threshold
uint16_t cfgTofHalf = 130;                   // mm — half mark
uint16_t cfgTofHigh = 60;                    // mm — refill complete
extern TofChip activeTofChip;   // defined with tofL1x near initTof()

LevelState currentState = LEVEL_UNKNOWN;
SensorReading lastReading = { false, false, 0 };

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
  cfgUnits      = (Units)prefs.getUChar("units", 0);
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
  prefs.putUChar("units", (uint8_t)cfgUnits);
  prefs.end();
}

// =====================================================================
// HTML pages
// =====================================================================



void streamHtmlHeader(const String& title) {
  server.sendContent(F("<!DOCTYPE html><html><head><meta charset='UTF-8'>"
                       "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                       "<title>"));
  server.sendContent(title);
  server.sendContent(F("</title><style>"
                       "body{font-family:system-ui,-apple-system,sans-serif;margin:0;padding:20px;background:#1a1a2e;color:#e0e0e0;}"
                       ".container{max-width:480px;margin:0 auto;}"
                       "h1{color:#e94560;font-size:1.5em;border-bottom:2px solid #e94560;padding-bottom:8px;}"
                       "h2{color:#0f3460;font-size:1.1em;margin-top:24px;color:#16c79a;}"
                       "label{display:block;margin:12px 0 4px;font-weight:600;font-size:0.9em;}"
                       "input[type=text],input[type=password]{width:100%;padding:10px;border:1px solid #333;border-radius:6px;"
                       "  box-sizing:border-box;font-size:1em;background:#16213e;color:#e0e0e0;}"
                       "input:focus{outline:none;border-color:#e94560;}"
                       "button{background:#e94560;color:#fff;border:none;padding:12px 24px;border-radius:6px;"
                       "  font-size:1em;cursor:pointer;margin-top:20px;width:100%;}"
                       "button:hover{background:#c81e45;}"
                       ".toggle{display:flex;align-items:center;gap:10px;margin:12px 0;}"
                       ".toggle input{width:auto;}"
                       ".status{background:#16213e;padding:16px;border-radius:8px;margin:16px 0;border-left:4px solid #16c79a;}"
                       ".status.warn{border-left-color:#e94560;}"
                       ".ip-fields,.tof-fields{display:none;}.ip-fields.show,.tof-fields.show{display:block;}"
                       "a{color:#16c79a;}"
                       ".nav{margin:16px 0;font-size:0.9em;}"
                       ".eye-btn{background:none;border:none;color:#e0e0e0;cursor:pointer;font-size:1.2em;padding:8px;margin-top:0;width:auto;}"
                       "</style></head><body><div class='container'>"));
}

void streamHtmlFooter() {
  server.sendContent(F("</div></body></html>"));
}

// HTTP chunked transfer encoding interprets a zero-length chunk as end-of-response,
// so an empty String passed to server.sendContent() truncates the page mid-render.
// streamDyn skips empty strings — safe drop-in for any user-supplied / optional config field.
inline void streamDyn(const String& s) {
  if (s.length() > 0) server.sendContent(s);
}


void streamConfigPage() {
  streamHtmlHeader("Oil Tank Monitor - Settings");

  // Status section
  if (!apMode && WiFi.status() == WL_CONNECTED) {
    server.sendContent(F("<div class='status'>"
                         "<strong>Status:</strong> Connected to <em>"));
    streamDyn(cfgSSID);
    server.sendContent(F("</em><br>"
                         "<strong>IP:</strong> "));
    server.sendContent(WiFi.localIP().toString());
    server.sendContent(F("<br>"
                         "<strong>Oil Level:</strong> "));
    server.sendContent(oilIsLow ? F("LOW") : F("OK"));
    server.sendContent(F("<br>"
                         "<strong>Firmware:</strong> v"));
    server.sendContent(FW_VERSION);
    server.sendContent(F("</div>"));
  } else if (apMode) {
    server.sendContent(F("<div class='status warn'>"
                         "<strong>Setup Mode</strong> — Configure your settings below, then the device will connect to your WiFi."
                         "</div>"));
  }

  server.sendContent(F("<h1>Oil Tank Monitor</h1>"
                       "<form method='POST' action='/save'>"
                       // WiFi section
                       "<h2>WiFi Network</h2>"
                       "<label for='ssid'>SSID (Network Name)</label>"
                       "<input type='text' id='ssid' name='ssid' value='"));
  streamDyn(cfgSSID);
  server.sendContent(F("' required>"
                       "<label for='password'>Password</label>"
                       "<input type='password' id='password' name='password' value='"));
  streamDyn(cfgPassword);
  server.sendContent(F("'>"
                       // Telegram section
                       "<h2>Telegram Notifications</h2>"
                       "<label for='bot_token'>Bot Token</label>"
                       "<div style='display:flex;gap:8px;align-items:center;'>"
                       "<input type='password' id='bot_token' name='bot_token' value='"));
  streamDyn(cfgBotToken);
  server.sendContent(F("' placeholder='123456789:ABCdef...' required style='flex:1;'>"
                       "<button type='button' class='eye-btn' onclick=\"toggleVis('bot_token',this)\">&#128065;</button>"
                       "</div>"
                       "<label for='chat_id'>Chat ID</label>"
                       "<div style='display:flex;gap:8px;align-items:center;'>"
                       "<input type='password' id='chat_id' name='chat_id' value='"));
  streamDyn(cfgChatID);
  server.sendContent(F("' placeholder='123456789' required style='flex:1;'>"
                       "<button type='button' class='eye-btn' onclick=\"toggleVis('chat_id',this)\">&#128065;</button>"
                       "<button type='button' onclick=\"addChatID()\" style='width:40px;padding:10px;margin-top:0;background:#16c79a;font-size:1.2em;'>+</button>"
                       "</div>"
                       // Chat ID 2
                       "<div id='chat2-row' style='display:"));
  server.sendContent(cfgChatID2.length() > 0 ? F("flex") : F("none"));
  server.sendContent(F(";gap:8px;align-items:center;margin-top:8px;'>"
                       "<input type='password' id='chat_id2' name='chat_id2' value='"));
  streamDyn(cfgChatID2);
  server.sendContent(F("' placeholder='Chat ID 2 (optional)' style='flex:1;'>"
                       "<button type='button' class='eye-btn' onclick=\"toggleVis('chat_id2',this)\">&#128065;</button>"
                       "<button type='button' onclick=\"addChatID()\" style='width:40px;padding:10px;margin-top:0;background:#16c79a;font-size:1.2em;'>+</button>"
                       "<button type='button' onclick=\"removeChatID(2)\" style='width:40px;padding:10px;margin-top:0;background:#e94560;font-size:1.2em;'>-</button>"
                       "</div>"
                       // Chat ID 3
                       "<div id='chat3-row' style='display:"));
  server.sendContent(cfgChatID3.length() > 0 ? F("flex") : F("none"));
  server.sendContent(F(";gap:8px;align-items:center;margin-top:8px;'>"
                       "<input type='password' id='chat_id3' name='chat_id3' value='"));
  streamDyn(cfgChatID3);
  server.sendContent(F("' placeholder='Chat ID 3 (optional)' style='flex:1;'>"
                       "<button type='button' class='eye-btn' onclick=\"toggleVis('chat_id3',this)\">&#128065;</button>"
                       "<button type='button' onclick=\"removeChatID(3)\" style='width:40px;padding:10px;margin-top:0;background:#e94560;font-size:1.2em;'>-</button>"
                       "</div>"
                       // Sensor Configuration
                       "<h2>Sensor Configuration</h2>"
                       "<label for='units'>Display Units</label>"
                       "<select id='units' name='units' onchange='convertUnits()' style='width:100%;padding:10px;background:#16213e;color:#e0e0e0;border:1px solid #333;border-radius:6px;'>"
                       "<option value='0'"));
  if (cfgUnits == UNITS_METRIC) server.sendContent(F(" selected"));
  server.sendContent(F(">Metric (mm)</option>"
                       "<option value='1'"));
  if (cfgUnits == UNITS_IMPERIAL) server.sendContent(F(" selected"));
  server.sendContent(F(">US Customary (inches)</option>"
                       "</select>"
                       "<label for='sensor_type'>Sensor Type</label>"
                       "<select id='sensor_type' name='sensor_type' onchange='toggleTof()' style='width:100%;padding:10px;background:#16213e;color:#e0e0e0;border:1px solid #333;border-radius:6px;'>"
                       "<option value='0'"));
  if (cfgSensorType == SENSOR_DIGITAL) server.sendContent(F(" selected"));
  server.sendContent(F(">Digital threshold (XKC-Y25-V)</option>"
                       "<option value='1'"));
  if (cfgSensorType == SENSOR_TOF) server.sendContent(F(" selected"));
  server.sendContent(F(">ToF distance (VL53L1X)</option>"
                       "<option value='2'"));
  if (cfgSensorType == SENSOR_IR_BREAK) server.sendContent(F(" selected"));
  server.sendContent(F(">IR break-beam (sight gauge puck)</option>"
                       "</select>"
                       "<div class='tof-fields"));
  if (cfgSensorType == SENSOR_TOF) server.sendContent(F(" show"));
  server.sendContent(F("' id='tof-fields'>"
                       "<div class='status' id='tof-live' style='margin-top:12px;'>Current Reading: <span id='tof-distance'>—</span> <span id='tof-unit'>"));
  server.sendContent(unitsLabel(cfgUnits));
  server.sendContent(F("</span></div>"
                       "<label for='tof_low'>LOW threshold ("));
  server.sendContent(unitsLabel(cfgUnits));
  server.sendContent(F(") — alert below this</label>"
                       "<input type='text' id='tof_low' name='tof_low' value='"));
  server.sendContent(formatDistance(cfgTofLow));
  server.sendContent(F("'>"
                       "<label for='tof_half'>HALF threshold ("));
  server.sendContent(unitsLabel(cfgUnits));
  server.sendContent(F(")</label>"
                       "<input type='text' id='tof_half' name='tof_half' value='"));
  server.sendContent(formatDistance(cfgTofHalf));
  server.sendContent(F("'>"
                       "<label for='tof_high'>HIGH threshold ("));
  server.sendContent(unitsLabel(cfgUnits));
  server.sendContent(F(") — refill complete above this</label>"
                       "<input type='text' id='tof_high' name='tof_high' value='"));
  server.sendContent(formatDistance(cfgTofHigh));
  server.sendContent(F("'>"
                       "<p style='font-size:0.85em;color:#999;'>Smaller value = puck closer to sensor (fuller tank). Must satisfy HIGH &lt; HALF &lt; LOW. Range: 30–2000 mm (1.18–78.74 in).</p>"
                       "</div>"
                       // Network
                       "<h2>Network Settings</h2>"
                       "<div class='toggle'>"
                       "<input type='checkbox' id='static_ip' name='static_ip' value='1'"));
  if (cfgStaticIP) server.sendContent(F(" checked"));
  server.sendContent(F(" onchange='toggleStatic()'>"
                       "<label for='static_ip' style='display:inline;margin:0;'>Use Static IP (instead of DHCP)</label>"
                       "</div>"
                       "<div class='ip-fields"));
  if (cfgStaticIP) server.sendContent(F(" show"));
  server.sendContent(F("' id='ip-fields'>"
                       "<label for='ip'>IP Address</label>"
                       "<input type='text' id='ip' name='ip' value='"));
  streamDyn(cfgIP);
  server.sendContent(F("' placeholder='192.168.1.100'>"
                       "<label for='gateway'>Gateway</label>"
                       "<input type='text' id='gateway' name='gateway' value='"));
  streamDyn(cfgGateway);
  server.sendContent(F("' placeholder='192.168.1.1'>"
                       "<label for='subnet'>Subnet Mask</label>"
                       "<input type='text' id='subnet' name='subnet' value='"));
  streamDyn(cfgSubnet);
  server.sendContent(F("' placeholder='255.255.255.0'>"
                       "<label for='dns'>DNS Server</label>"
                       "<input type='text' id='dns' name='dns' value='"));
  streamDyn(cfgDNS);
  server.sendContent(F("' placeholder='8.8.8.8'>"
                       "</div>"
                       // Web interface password
                       "<h2>Web Interface Password</h2>"
                       "<label>Username</label>"
                       "<input type='text' value='admin' disabled style='opacity:0.6;'>"
                       "<label for='web_pass'>Password</label>"
                       "<input type='password' id='web_pass' name='web_pass' value='"));
  streamDyn(cfgWebPassword);
  server.sendContent(F("'>"
                       "<button type='submit'>Save &amp; Restart</button>"
                       "</form>"
                       // Firmware update link
                       "<h2>Firmware</h2>"
                       "<p style='font-size:0.9em;'>Current version: <strong>v"));
  server.sendContent(FW_VERSION);
  server.sendContent(F("</strong></p>"
                       "<a href='/update' style='display:block;text-align:center;padding:12px;background:#0f3460;"
                       "border-radius:6px;color:#e0e0e0;text-decoration:none;'>Upload Firmware Update</a>"
                       // Logout and danger zone
                       "<div style='margin-top:24px;text-align:center;'>"
                       "<a href='/logout' style='color:#e0e0e0;font-size:0.9em;'>Log Out</a>"
                       "</div>"
                       "<h2 style='margin-top:40px;color:#e94560;'>Danger Zone</h2>"
                       "<button style='background:#333;border:1px solid #e94560;' "
                       "onclick=\"if(confirm('Are you sure you want to factory reset? This will erase ALL settings (WiFi, Telegram, network) and reboot into setup mode.')){window.location='/factory-reset'}\">"
                       "Factory Reset</button>"
                       "<script>"
                       "function toggleStatic(){"
                       "  document.getElementById('ip-fields').classList.toggle('show',"
                       "    document.getElementById('static_ip').checked);}"
                       "function toggleTof(){"
                       "  var v=document.getElementById('sensor_type').value;"
                       "  document.getElementById('tof-fields').classList.toggle('show', v==='1');"
                       "}"
                       "function convertUnits(){"
                       "  var u=document.getElementById('units').value;"
                       "  var ids=['tof_low','tof_half','tof_high'];"
                       "  for(var i=0;i<ids.length;i++){"
                       "    var f=document.getElementById(ids[i]);"
                       "    var v=parseFloat(f.value);"
                       "    if(isNaN(v)) continue;"
                       "    f.value=(u==='1')?(v/25.4).toFixed(2):Math.round(v*25.4).toString();"
                       "  }"
                       "  var labels=document.querySelectorAll(\"label[for='tof_low'],label[for='tof_half'],label[for='tof_high']\");"
                       "  var symbol=(u==='1')?'in':'mm';"
                       "  labels[0].innerHTML='LOW threshold ('+symbol+') — alert below this';"
                       "  labels[1].innerHTML='HALF threshold ('+symbol+')';"
                       "  labels[2].innerHTML='HIGH threshold ('+symbol+') — refill complete above this';"
                       "  var unitSpan=document.getElementById('tof-unit');"
                       "  if(unitSpan) unitSpan.textContent=symbol;"
                       "}"
                       "var tofPoll=null;"
                       "function startTofPoll(){"
                       "  if(tofPoll) clearInterval(tofPoll);"
                       "  tofPoll=setInterval(function(){"
                       "    if(document.getElementById('sensor_type').value!=='1'){clearInterval(tofPoll);tofPoll=null;return;}"
                       "    fetch('/status').then(r=>r.json()).then(j=>{"
                       "      var el=document.getElementById('tof-distance');"
                       "      if(!el) return;"
                       "      if(j.distance_mm===undefined||!j.sensor_valid){el.textContent='—';return;}"
                       "      var u=document.getElementById('units').value;"
                       "      el.textContent=(u==='1')?(j.distance_mm/25.4).toFixed(2):j.distance_mm;"
                       "    }).catch(()=>{});"
                       "  },2000);"
                       "}"
                       "if(document.getElementById('sensor_type').value==='1'){startTofPoll();}"
                       "document.getElementById('sensor_type').addEventListener('change',function(){"
                       "  if(this.value==='1') startTofPoll();"
                       "});"
                       "function toggleVis(id,btn){"
                       "  var f=document.getElementById(id);"
                       "  if(f.type==='password'){f.type='text';btn.style.opacity='0.5';}"
                       "  else{f.type='password';btn.style.opacity='1';}}"
                       "function addChatID(){"
                       "  var r2=document.getElementById('chat2-row');"
                       "  var r3=document.getElementById('chat3-row');"
                       "  if(r2.style.display==='none'){r2.style.display='flex';}"
                       "  else if(r3.style.display==='none'){r3.style.display='flex';}"
                       "}"
                       "function removeChatID(n){"
                       "  var r=document.getElementById('chat'+n+'-row');"
                       "  r.style.display='none';"
                       "  document.getElementById('chat_id'+n).value='';"
                       "}"
                       "</script>"));
  streamHtmlFooter();
}


void streamSavedPage() {
  String targetIP = cfgStaticIP && cfgIP.length() > 0 ? cfgIP : WiFi.localIP().toString();
  String targetURL = "http://" + targetIP;

  streamHtmlHeader("Settings Saved");
  server.sendContent(F("<h1>Settings Saved</h1>"
                       "<div class='status'>"
                       "Configuration saved. The device is restarting and connecting to <strong>"));
  streamDyn(cfgSSID);
  server.sendContent(F("</strong>.<br><br>"
                       "Redirecting to <strong>"));
  server.sendContent(targetURL);
  server.sendContent(F("</strong> in <span id='countdown'>15</span> seconds..."
                       "</div>"
                       "<script>"
                       "var sec=15;var el=document.getElementById('countdown');"
                       "var t=setInterval(function(){sec--;el.textContent=sec;"
                       "if(sec<=0){clearInterval(t);window.location='"));
  server.sendContent(targetURL);
  server.sendContent(F("';}"
                       "},1000);"
                       "</script>"));
  streamHtmlFooter();
}


void streamUpdatePage() {
  streamHtmlHeader("Firmware Update");
  server.sendContent(F("<h1>Firmware Update</h1>"
                       "<div class='status'>"
                       "<strong>Current Version:</strong> v"));
  server.sendContent(FW_VERSION);
  server.sendContent(F("<br>"
                       "<strong>Free Space:</strong> "));
  server.sendContent(String(ESP.getFreeSketchSpace() / 1024));
  server.sendContent(F(" KB"
                       "</div>"
                       "<p style='font-size:0.9em;'>Upload a compiled <code>.bin</code> firmware file. The device will flash itself and reboot.</p>"
                       "<form method='POST' action='/update' enctype='multipart/form-data'>"
                       "<label for='firmware'>Firmware File (.bin)</label>"
                       "<input type='file' id='firmware' name='firmware' accept='.bin' required "
                       "style='padding:10px;background:#16213e;border:1px solid #333;border-radius:6px;width:100%;box-sizing:border-box;'>"
                       "<button type='submit' onclick=\"this.innerText='Uploading... do not power off';this.disabled=true;this.form.submit();\">Upload &amp; Install</button>"
                       "</form>"
                       "<div class='nav' style='margin-top:16px;'><a href='/'>&larr; Back to Settings</a></div>"));
  streamHtmlFooter();
}


void streamUpdateResultPage(bool success, const String& message) {
  streamHtmlHeader(String("Update ") + (success ? "Complete" : "Failed"));
  server.sendContent(F("<h1>Firmware Update "));
  server.sendContent(success ? F("Complete") : F("Failed"));
  server.sendContent(F("</h1>"
                       "<div class='status"));
  if (!success) server.sendContent(F(" warn"));
  server.sendContent(F("'>"));
  server.sendContent(message);
  if (success) {
    server.sendContent(F("<br><br>Redirecting in <span id='countdown'>15</span> seconds..."
                         "</div>"
                         "<script>"
                         "var sec=15;var el=document.getElementById('countdown');"
                         "var t=setInterval(function(){sec--;el.textContent=sec;"
                         "if(sec<=0){clearInterval(t);window.location='/';}"
                         "},1000);"
                         "</script>"));
  } else {
    server.sendContent(F("</div>"
                         "<div class='nav' style='margin-top:16px;'><a href='/update'>&larr; Try Again</a></div>"));
  }
  streamHtmlFooter();
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


void streamLoginPage(bool failed) {
  streamHtmlHeader("Oil Tank Monitor - Login");
  server.sendContent(F("<h1>Oil Tank Monitor</h1>"));
  if (failed) {
    server.sendContent(F("<div class='status warn'>Invalid password. Try again.</div>"));
  }
  server.sendContent(F("<form method='POST' action='/login'>"
                       "<label for='username'>Username</label>"
                       "<input type='text' id='username' value='admin' disabled style='opacity:0.6;'>"
                       "<label for='password'>Password</label>"
                       "<input type='password' id='password' name='password' autofocus required>"
                       "<button type='submit'>Log In</button>"
                       "</form>"));
  streamHtmlFooter();
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
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html", "");
    streamLoginPage(true);
    return;
  }
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  streamLoginPage(false);
}

void handleLogout() {
  sessionToken = 0;
  server.sendHeader("Set-Cookie", "session=; Path=/; Max-Age=0; HttpOnly");
  server.sendHeader("Location", "/login");
  server.send(302, "text/plain", "Logged out");
}

void handleRoot() {
  if (!requireAuth()) return;
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  streamConfigPage();
}

void sendValidationError(const String& message) {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(400, "text/html", "");
  streamHtmlHeader("Configuration Error");
  server.sendContent(F("<h1>Configuration Error</h1>"
                       "<div class='status warn'>"));
  server.sendContent(message);
  server.sendContent(F("</div>"
                       "<div class='nav' style='margin-top:16px;'><a href='/'>&larr; Back to Settings</a></div>"));
  streamHtmlFooter();
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

  // Sensor configuration — stage into locals first, validate, then commit to globals.
  // This prevents in-memory corruption if validation fails (NVS is also untouched on failure).
  SensorType newSensorType = cfgSensorType;
  Units newUnits = cfgUnits;
  uint16_t newTofLow  = cfgTofLow;
  uint16_t newTofHalf = cfgTofHalf;
  uint16_t newTofHigh = cfgTofHigh;

  if (server.hasArg("sensor_type")) {
    int t = server.arg("sensor_type").toInt();
    if (t == 1) newSensorType = SENSOR_TOF;
    else if (t == 2) newSensorType = SENSOR_IR_BREAK;
    else newSensorType = SENSOR_DIGITAL;
  }
  if (server.hasArg("units")) {
    int u = server.arg("units").toInt();
    newUnits = (u == 1) ? UNITS_IMPERIAL : UNITS_METRIC;
  }

  // Apply the new units setting to the global *temporarily* so parseDistanceInput()
  // reads the request's intended unit. We restore on parse failure to preserve the
  // staged-write invariant (no global mutation until validation passes).
  Units savedUnits = cfgUnits;
  cfgUnits = newUnits;
  bool parseOk = true;
  if (server.hasArg("tof_low")  && !parseDistanceInput(server.arg("tof_low"),  newTofLow))  parseOk = false;
  if (server.hasArg("tof_half") && !parseDistanceInput(server.arg("tof_half"), newTofHalf)) parseOk = false;
  if (server.hasArg("tof_high") && !parseDistanceInput(server.arg("tof_high"), newTofHigh)) parseOk = false;
  cfgUnits = savedUnits;

  if (!parseOk) {
    sendValidationError("Each ToF threshold must be a positive number.");
    return;
  }

  if (newSensorType == SENSOR_TOF) {
    if (newTofHigh < TOF_MIN_MM || newTofHigh > TOF_MAX_MM ||
        newTofHalf < TOF_MIN_MM || newTofHalf > TOF_MAX_MM ||
        newTofLow  < TOF_MIN_MM || newTofLow  > TOF_MAX_MM) {
      char minIn[8], maxIn[8];
      snprintf(minIn, sizeof(minIn), "%.2f", TOF_MIN_MM / 25.4f);
      snprintf(maxIn, sizeof(maxIn), "%.2f", TOF_MAX_MM / 25.4f);
      sendValidationError("Each ToF threshold must be between " + String(TOF_MIN_MM) + " mm (" + String(minIn) + " in) and " + String(TOF_MAX_MM) + " mm (" + String(maxIn) + " in).");
      return;
    }
    if (!(newTofHigh < newTofHalf && newTofHalf < newTofLow)) {
      const char* u = unitsLabel(newUnits);
      // formatDistance reads cfgUnits, so render against newUnits via temporary swap.
      Units savedUnits2 = cfgUnits;
      cfgUnits = newUnits;
      String hi = formatDistance(newTofHigh);
      String hf = formatDistance(newTofHalf);
      String lo = formatDistance(newTofLow);
      cfgUnits = savedUnits2;
      sendValidationError("ToF thresholds must satisfy HIGH &lt; HALF &lt; LOW (smaller value = fuller tank). Got HIGH=" + hi + " " + u + " HALF=" + hf + " " + u + " LOW=" + lo + " " + u + ".");
      return;
    }
  }

  // Validation passed — commit to globals
  cfgSensorType = newSensorType;
  cfgUnits      = newUnits;
  cfgTofLow  = newTofLow;
  cfgTofHalf = newTofHalf;
  cfgTofHigh = newTofHigh;

  if (cfgSubnet.length() == 0) cfgSubnet = "255.255.255.0";
  if (cfgDNS.length() == 0)    cfgDNS = "8.8.8.8";

  String newPass = server.arg("web_pass");
  if (newPass.length() > 0) cfgWebPassword = newPass;

  saveSettings();
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  streamSavedPage();
  delay(2000);
  ESP.restart();
}

void handleFactoryReset() {
  if (!requireAuth()) return;
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  streamHtmlHeader("Factory Reset");
  server.sendContent(F("<h1>Factory Reset Complete</h1>"
                       "<div class='status warn'>"
                       "All settings have been erased. The device is rebooting into setup mode.<br><br>"
                       "Connect to WiFi <strong>"));
  server.sendContent(AP_SSID);
  server.sendContent(F("</strong> (password: <strong>"));
  server.sendContent(AP_PASSWORD);
  server.sendContent(F("</strong>)<br>"
                       "Then open <strong>http://192.168.4.1</strong> to reconfigure."
                       "</div>"));
  streamHtmlFooter();
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
  json += "\"firmware\":\"" + String(FW_VERSION) + "\",";
  json += "\"units\":\"";
  json += (cfgUnits == UNITS_IMPERIAL) ? "imperial" : "metric";
  json += "\",";
  json += "\"sensor_type\":\"";
  json += sensorTypeJsonName(cfgSensorType);
  json += "\",";
  json += "\"sensor_valid\":" + String(lastReading.valid ? "true" : "false") + ",";
  json += "\"level\":\"" + String(levelStateName(currentState)) + "\"";
  if (cfgSensorType == SENSOR_TOF) {
    json += ",\"distance_mm\":" + String(lastReading.distanceMm);
    json += ",\"thresholds\":{";
    json += "\"low\":" + String(cfgTofLow) + ",";
    json += "\"half\":" + String(cfgTofHalf) + ",";
    json += "\"high\":" + String(cfgTofHigh);
    json += "}";
    const char* tofChipName;
    switch (activeTofChip) {
      case TOF_VL53L1X: tofChipName = "vl53l1x"; break;
      default:          tofChipName = "none"; break;   // TOF_NONE
    }
    json += ",\"tof_chip\":\"" + String(tofChipName) + "\"";
  }
  if (cfgSensorType == SENSOR_IR_BREAK) {
    // Fresh pin read — beam_state is an instantaneous snapshot, not the debounced level.
    bool clear = (digitalRead(SENSOR_PIN) == HIGH);
    json += ",\"beam_state\":\"" + String(clear ? "clear" : "broken") + "\"";
  }
  json += "}";
  server.send(200, "application/json", json);
}

void handleUpdatePage() {
  if (!requireAuth()) return;
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  streamUpdatePage();
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
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  if (success) {
    streamUpdateResultPage(true, "Firmware updated successfully.");
    delay(2000);
    ESP.restart();
  } else {
    streamUpdateResultPage(false, "Update failed. Please try again with a valid .bin file.");
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
    esp_task_wdt_reset();
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
      esp_task_wdt_reset();
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

Adafruit_VL53L1X tofL1x;       // Constructed but not initialized until initTof() (Task 3) probes the bus.
TofChip activeTofChip = TOF_NONE;
int tofInvalidCount = 0;        // consecutive invalid reads (Task 9)
bool sensorFaultActive = false; // Task 9

// Initialize the VL53L1X ToF sensor. Configures long mode + 50ms timing budget
// and starts continuous ranging. Sets activeTofChip on success. Returns true on success.
bool initTof() {
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  // Fast NACK probe before tofL1x.begin(): the Adafruit/ST library's InitSensor()
  // contains a `while(tmp==0)` poll on CheckForDataReady that never returns when
  // no sensor is on the bus, freezing the entire Arduino loop. A 0x29 ping fails
  // in <1 ms when nothing is wired and avoids that trap entirely.
  Wire.beginTransmission(0x29);
  if (Wire.endTransmission() != 0) {
    activeTofChip = TOF_NONE;
    return false;
  }
  if (!tofL1x.begin(0x29, &Wire)) {
    activeTofChip = TOF_NONE;
    return false;
  }
  tofL1x.VL53L1X_SetDistanceMode(2);   // 2 = Long mode (~4 m range)
  tofL1x.setTimingBudget(50);          // 50 ms per measurement
  if (!tofL1x.startRanging()) {
    Serial.println("VL53L1X detected but startRanging() failed — bailing out of ToF init");
    activeTofChip = TOF_NONE;
    return false;
  }
  activeTofChip = TOF_VL53L1X;
  Serial.println("Sensor: TOF (VL53L1X, long mode) on I2C SDA="
                 + String(I2C_SDA_PIN) + " SCL=" + String(I2C_SCL_PIN));
  return true;
}

bool initSensor() {
  if (cfgSensorType == SENSOR_DIGITAL) {
    pinMode(SENSOR_PIN, INPUT);
    Serial.println("Sensor: DIGITAL on GPIO" + String(SENSOR_PIN));
    return true;
  }
  if (cfgSensorType == SENSOR_IR_BREAK) {
    pinMode(SENSOR_PIN, INPUT_PULLUP);
    Serial.println("Sensor: IR break-beam on GPIO" + String(SENSOR_PIN) +
                   " (HIGH=clear, LOW=broken)");
    return true;
  }
  // ToF path — VL53L1X
  if (!initTof()) {
    Serial.println("Sensor: TOF init FAILED — VL53L1X did not respond on I2C (SDA=21, SCL=22, VCC=3V3, GND=GND)");
    return false;
  }
  return true;
}

SensorReading readSensorRaw() {
  SensorReading r = { false, false, 0 };
  if (cfgSensorType == SENSOR_DIGITAL || cfgSensorType == SENSOR_IR_BREAK) {
    r.digitalState = (digitalRead(SENSOR_PIN) == HIGH);
    r.valid = true;
    return r;
  }
  // ToF path — VL53L1X only
  if (activeTofChip != TOF_VL53L1X) return r;     // TOF_NONE — should not occur in normal flow
  if (!tofL1x.dataReady()) return r;              // measurement not ready; debounce will skip this read
  // INVARIANT: once dataReady() is true, distance/GetRangeStatus/clearInterrupt MUST run
  // back-to-back with no early returns between them. Skipping clearInterrupt() leaves the
  // chip stuck — dataReady() will never re-assert. Validation guards live AFTER clearInterrupt.
  int16_t mm = tofL1x.distance();                 // signed; -1 on error
  uint8_t status = 0xFF;
  tofL1x.VL53L1X_GetRangeStatus(&status);
  tofL1x.clearInterrupt();                        // arm next measurement (must run before any return below)
  if (status != 0 || mm < 0) return r;
  if ((uint16_t)mm < TOF_MIN_MM || (uint16_t)mm > TOF_MAX_MM) return r;
  r.distanceMm = (uint16_t)mm;
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

  if (cfgSensorType == SENSOR_DIGITAL || cfgSensorType == SENSOR_IR_BREAK) {
    if (raw.digitalState == lastRaw.digitalState && raw.valid) {
      if (debounce < DEBOUNCE_COUNT) debounce++;
    } else {
      debounce = 1;
      lastRaw = raw;
    }
    if (debounce >= DEBOUNCE_COUNT) return raw;
    // Not yet stable — return last accepted reading
    // oilIsLow is true when the last accepted state was LOW (digital) or OIL_LOW (IR_BREAK);
    // !oilIsLow reconstructs the corresponding digitalState for the unstable-debounce fallback.
    SensorReading last = { true, !oilIsLow, 0 };
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
  if (cfgSensorType == SENSOR_IR_BREAK) {
    // Pin HIGH (with pullup) = beam detected = clear path = oil above low-oil mark.
    // Pin LOW = beam broken (puck has reached the mark) = oil low.
    return r.digitalState ? LEVEL_OIL_OK : LEVEL_OIL_LOW;
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
  if (cfgSensorType == SENSOR_IR_BREAK) {
    if (from == LEVEL_OIL_OK && to == LEVEL_OIL_LOW) {
      String msg = "⚠️ Low oil — sight-gauge puck has reached the low-oil mark. Please refill.";
      msg += "\nSettings: http://" + WiFi.localIP().toString();
      sendTelegram(msg);
    } else if (from == LEVEL_OIL_LOW && to == LEVEL_OIL_OK) {
      sendTelegram("✅ Oil level restored — puck is above the low-oil mark.");
    }
    return;
  }
  // Defensive: ToF matrix below uses ordinal (<=, >=) comparisons on LevelState.
  // If the sensor type isn't ToF, exit before those run on out-of-mode states.
  if (cfgSensorType != SENSOR_TOF) return;
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

  const esp_reset_reason_t bootReason = esp_reset_reason();
  const bool recoveredFromWdt = (bootReason == ESP_RST_TASK_WDT);
  if (recoveredFromWdt) {
    Serial.println("Boot reason: ESP_RST_TASK_WDT (recovered from a hang)");
  }

  // Arduino-ESP32 v3 pre-initializes the TWDT with its own (shorter) default timeout.
  // Reconfigure to our 15 s budget so Telegram POSTs and bus-recovery don't false-trigger.
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = 15000,        // 15 s
    .idle_core_mask = 0,        // don't watch the idle tasks; we only care about the loop task
    .trigger_panic = true,      // panic → reboot on miss
  };
  if (esp_task_wdt_reconfigure(&wdt_config) != ESP_OK) {
    esp_task_wdt_init(&wdt_config);  // first-time init if no prior watchdog
  }
  esp_task_wdt_add(NULL);        // subscribe the current (Arduino loop) task

  initSensor();
  pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);

  // Check if BOOT button is held at startup for factory reset
  checkResetButton();

  loadSettings();
  String tlow  = formatDistance(cfgTofLow);
  String thalf = formatDistance(cfgTofHalf);
  String thigh = formatDistance(cfgTofHigh);
  const char* u = unitsLabel(cfgUnits);
  Serial.printf("Sensor type: %s | ToF thresholds: low=%s %s half=%s %s high=%s %s\n",
                sensorTypeBootName(cfgSensorType),
                tlow.c_str(),  u,
                thalf.c_str(), u,
                thigh.c_str(), u);

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
      if (cfgSensorType == SENSOR_TOF && activeTofChip == TOF_NONE) {
        sendTelegram("⚠️ ToF init failed — VL53L1X did not respond on I2C. Check wiring or switch sensor type via web UI.");
      }
      // Take an initial sensor reading to establish currentState before announcing online.
      delay(200);  // let sensor stabilize
      SensorReading r = readSensor();
      currentState = bucketReading(r, LEVEL_UNKNOWN);
      Serial.printf("Boot: initial state=%s (after WiFi connect)\n", levelStateName(currentState));

      String msg = "🛢️ Oil tank monitor is ONLINE.\nSensor: ";
      msg += sensorTypeDisplayName(cfgSensorType);
      msg += "\nLevel: " + String(levelStateName(currentState));
      if (cfgSensorType == SENSOR_TOF && r.valid) {
        msg += " (" + formatDistance(r.distanceMm) + " " + unitsLabel(cfgUnits) + ")";
      }
      msg += "\nSettings: http://" + WiFi.localIP().toString();
      sendTelegram(msg);
      if (recoveredFromWdt) {
        sendTelegram("⚠️ Recovered from a hang (watchdog reset). The device auto-rebooted to restore service.");
      }
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
  esp_task_wdt_reset();
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
    esp_task_wdt_reset();
    delay(100);
    return;
  }

  unsigned long now = millis();

  if (now - lastSensorCheck >= SENSOR_CHECK_MS) {
    lastSensorCheck = now;

    SensorReading reading = readSensor();
    lastReading = reading;

    // Fault tracking — only meaningful for ToF (digital readSensor() returns valid=true always)
    if (cfgSensorType == SENSOR_TOF) {
      if (!reading.valid) {
        tofInvalidCount++;
        if (tofInvalidCount == TOF_RECOVERY_CYCLES) {
          Serial.println("ToF: attempting I2C bus recovery");
          Wire.end();
          initTof();   // re-runs probe + long-mode + startRanging on the VL53L1X
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
      if (newState == LEVEL_LOW || newState == LEVEL_OIL_LOW) lastAlertTime = now;
    }

    // Maintain legacy oilIsLow for /status backward compat
    oilIsLow = (currentState == LEVEL_LOW || currentState == LEVEL_OIL_LOW);
  }

  // Hourly LOW reminder — same behavior as v1.x, extended to cover IR break-beam
  if ((currentState == LEVEL_LOW || currentState == LEVEL_OIL_LOW) &&
      (now - lastAlertTime >= ALERT_INTERVAL_MS)) {
    Serial.println("Sending hourly low-oil reminder.");
    if (currentState == LEVEL_OIL_LOW) {
      sendTelegram("⚠️ REMINDER: Sight-gauge puck still at the low-oil mark. Please refill.");
    } else {
      sendTelegram("⚠️ REMINDER: Oil tank is still LOW. Please refill.");
    }
    lastAlertTime = now;
  }

  delay(100);
}
