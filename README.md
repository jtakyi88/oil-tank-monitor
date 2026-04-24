# Oil Tank Level Monitor

ESP32 + XKC-Y25-V non-contact liquid level sensor that sends Telegram alerts when your oil tank is running low. Includes a built-in web interface for configuration — no hardcoded credentials required.

## What It Does

- **Web-based setup** — on first boot, creates a WiFi hotspot for configuration via your phone or laptop
- Monitors oil level using a non-contact capacitive sensor mounted on the sight glass
- Sends an immediate Telegram alert when oil drops below the sensor
- Sends hourly reminders until the tank is refilled
- Sends a confirmation message when the level is restored
- DHCP or static IP support, configurable from the web interface
- All settings saved to flash — survives power cycles
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

### 2. Flash the ESP32

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

### 3. Configure via Web Interface

1. On your phone or laptop, connect to WiFi network **`OilMonitor-Setup`** (password: `oiltank123`)
2. Open a browser and go to **http://192.168.4.1**
3. Enter your WiFi network name and password
4. Enter your Telegram bot token and chat ID
5. Optionally configure a static IP instead of DHCP
6. Click **Save & Restart**

The device will reboot, connect to your WiFi, and send a Telegram message confirming it's online — including its IP address for future access to the settings page.

### 4. Install

1. Attach the XKC-Y25-V sensor to the sight glass tube at your desired low-level threshold
2. Power the ESP32 from a USB adapter near the tank
3. Keep the USB cable under 5 meters — if you need more distance, use a longer extension cord to the adapter instead

### Reconfiguring

You can access the settings page at any time by visiting the device's IP address in a browser. If the device can't connect to WiFi (e.g. after a network change), it will automatically fall back to AP mode so you can reconfigure.

## API

The device exposes a JSON status endpoint:

```
GET http://<device-ip>/status
```

Returns:
```json
{
  "oil_low": false,
  "wifi_connected": true,
  "ip": "192.168.1.100",
  "ssid": "YourNetwork",
  "uptime_sec": 3600
}
```

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

- Multiple sensor support for different tank levels (e.g. low, critical)
- OTA (over-the-air) firmware updates
- Battery-powered deep sleep mode
- MQTT integration for Home Assistant
- Level history logging and graphing on the web interface
- Temperature compensation for sensor accuracy
- Email notifications as an alternative to Telegram

Fork the repo, make your changes, and open a pull request.

## License

[MIT](LICENSE)
