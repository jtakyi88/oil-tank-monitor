/*
 * Oil Tank Level Monitor
 * ESP32 + XKC-Y25-V non-contact liquid level sensor
 * Sends Telegram alert once per hour when oil is below sensor level.
 * Stops alerting once oil is detected again.
 *
 * Wiring:
 *   XKC-Y25-V Brown  -> ESP32 3V3
 *   XKC-Y25-V Blue   -> ESP32 GND
 *   XKC-Y25-V Yellow -> ESP32 GPIO4 (D4)
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>

// ===== CONFIGURATION — FILL THESE IN =====
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* BOT_TOKEN     = "YOUR_TELEGRAM_BOT_TOKEN";
const char* CHAT_ID       = "YOUR_TELEGRAM_CHAT_ID";
// ==========================================

// Sensor pin (GPIO4 = D4 on most ESP32 boards)
const int SENSOR_PIN = 4;

// How often to re-alert when oil is low (milliseconds)
const unsigned long ALERT_INTERVAL_MS = 3600000UL;  // 1 hour

// How often to read the sensor (milliseconds)
const unsigned long SENSOR_CHECK_MS = 5000UL;  // every 5 seconds

// Debounce: require N consecutive same-readings before acting
const int DEBOUNCE_COUNT = 3;

WiFiClientSecure secured_client;
UniversalTelegramBot bot(BOT_TOKEN, secured_client);

unsigned long lastAlertTime   = 0;
unsigned long lastSensorCheck = 0;
bool oilIsLow                = false;
bool alertSentThisCycle      = false;
int debounceCounter          = 0;
bool lastRawReading          = false;

void connectWiFi() {
  Serial.print("Connecting to WiFi");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected. IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\nWiFi FAILED — will retry in loop.");
  }
}

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
    return noLiquid;  // true = oil is low
  }
  return oilIsLow;  // hold previous state until debounce settles
}

bool sendTelegram(const String& message) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi down — reconnecting before send.");
    connectWiFi();
    if (WiFi.status() != WL_CONNECTED) return false;
  }
  bool sent = bot.sendMessage(CHAT_ID, message, "");
  if (sent) {
    Serial.println("Telegram sent: " + message);
  } else {
    Serial.println("Telegram FAILED to send.");
  }
  return sent;
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== Oil Tank Monitor Starting ===");

  pinMode(SENSOR_PIN, INPUT);

  connectWiFi();
  secured_client.setInsecure();  // skip TLS cert verification (OK for bot API)

  // Send startup confirmation
  sendTelegram("🛢️ Oil tank monitor is ONLINE and watching the level.");

  Serial.println("Setup complete. Monitoring sensor on GPIO4.");
}

void loop() {
  unsigned long now = millis();

  // Reconnect WiFi if dropped
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  // Read sensor on interval
  if (now - lastSensorCheck >= SENSOR_CHECK_MS) {
    lastSensorCheck = now;

    bool currentlyLow = readSensorDebounced();

    if (currentlyLow && !oilIsLow) {
      // Transition: oil just went LOW
      oilIsLow = true;
      alertSentThisCycle = false;
      Serial.println("OIL LOW detected!");
      sendTelegram("⚠️ OIL TANK LOW — the level has dropped below the sensor. Please refill.");
      lastAlertTime = now;
      alertSentThisCycle = true;

    } else if (!currentlyLow && oilIsLow) {
      // Transition: oil is back above sensor
      oilIsLow = false;
      alertSentThisCycle = false;
      Serial.println("Oil level RESTORED.");
      sendTelegram("✅ Oil tank level restored — sensor detects oil above the line.");
    }
  }

  // Hourly reminder while oil stays low
  if (oilIsLow && (now - lastAlertTime >= ALERT_INTERVAL_MS)) {
    Serial.println("Sending hourly low-oil reminder.");
    sendTelegram("⚠️ REMINDER: Oil tank is still LOW. Please refill.");
    lastAlertTime = now;
  }

  delay(100);
}
