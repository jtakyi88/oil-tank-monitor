# Oil Tank Level Monitor

ESP32 + XKC-Y25-V non-contact liquid level sensor that sends Telegram alerts when your oil tank is running low.

## What It Does

- Monitors oil level using a non-contact capacitive sensor mounted on the sight glass
- Sends an immediate Telegram alert when oil drops below the sensor
- Sends hourly reminders until the tank is refilled
- Sends a confirmation message when the level is restored
- Auto-reconnects WiFi if the connection drops
- Debounced sensor readings to prevent false alerts

## Hardware

| Component | Details |
|-----------|---------|
| Microcontroller | ESP32 (any dev board with CP210x or CH340 USB) |
| Sensor | XKC-Y25-V non-contact liquid level sensor |
| Power | 5V USB adapter |
| Cable | Micro-USB or USB-C data cable (not charge-only) |

### Wiring

| XKC-Y25-V Wire | ESP32 Pin |
|----------------|-----------|
| Brown | 3V3 |
| Blue | GND |
| Yellow | GPIO4 (D4) |

## Setup

### 1. Telegram Bot

1. Open Telegram and message [@BotFather](https://t.me/BotFather)
2. Send `/newbot` and follow the prompts to create your bot
3. Save the **bot token** it gives you
4. Message [@userinfobot](https://t.me/userinfobot) to get your **chat ID**

### 2. Configure the Sketch

Open `OilTankMonitor/OilTankMonitor.ino` and fill in your credentials:

```cpp
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* BOT_TOKEN     = "YOUR_TELEGRAM_BOT_TOKEN";
const char* CHAT_ID       = "YOUR_TELEGRAM_CHAT_ID";
```

### 3. Flash the ESP32

Using [Arduino CLI](https://arduino.github.io/arduino-cli/):

```bash
# Install ESP32 board support
arduino-cli config init
arduino-cli config add board_manager.additional_urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32

# Install libraries
arduino-cli lib install "UniversalTelegramBot"
arduino-cli lib install "ArduinoJson"

# Compile and upload
arduino-cli compile --fqbn esp32:esp32:esp32 OilTankMonitor
arduino-cli upload --fqbn esp32:esp32:esp32:UploadSpeed=115200 --port /dev/ttyUSB0 OilTankMonitor
```

Or use the [Arduino IDE](https://www.arduino.cc/en/software) — add the ESP32 board manager URL in Preferences, install the ESP32 board package, install the UniversalTelegramBot and ArduinoJson libraries, and upload.

### 4. Install

1. Attach the XKC-Y25-V sensor to the sight glass tube at your desired low-level threshold
2. Power the ESP32 from a USB adapter near the tank
3. Keep the USB cable under 5 meters — if you need more distance, use a longer extension cord to the adapter instead

## Sensor Notes

The XKC-Y25-V is a capacitive sensor that detects liquid through glass without any contact with the fluid. It outputs HIGH when liquid is present and LOW when absent. The firmware includes a 3-read debounce to prevent false triggers from sloshing or vibration.

If your sensor variant has inverted logic (LOW = liquid present), change this line in the sketch:

```cpp
bool noLiquid = digitalRead(SENSOR_PIN) == LOW;
```

to:

```cpp
bool noLiquid = digitalRead(SENSOR_PIN) == HIGH;
```

## Configuration

These constants in the sketch can be adjusted:

| Constant | Default | Description |
|----------|---------|-------------|
| `SENSOR_PIN` | `4` (GPIO4) | GPIO pin connected to sensor signal wire |
| `ALERT_INTERVAL_MS` | `3600000` (1 hour) | How often to re-send low-oil reminders |
| `SENSOR_CHECK_MS` | `5000` (5 sec) | How often to read the sensor |
| `DEBOUNCE_COUNT` | `3` | Consecutive same-readings required before acting |

## Contributing

Contributions are welcome. Some ideas:

- Web dashboard for level history
- Multiple sensor support for different tank levels (e.g. low, critical)
- OTA (over-the-air) firmware updates
- Battery-powered deep sleep mode
- MQTT integration for Home Assistant
- Temperature compensation for sensor accuracy

Fork the repo, make your changes, and open a pull request.

## License

[MIT](LICENSE)
