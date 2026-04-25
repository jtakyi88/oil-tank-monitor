# Oil Tank Level Monitor

ESP32 + XKC-Y25-V non-contact liquid level sensor that sends Telegram alerts when your oil tank is running low. Includes a built-in web interface for configuration — no hardcoded credentials required.

![Telegram Alert Messages](images/TelegramBotSensorMessages.png)

## What It Does

- **Web-based setup** — on first boot, creates a WiFi hotspot for configuration via your phone or laptop
- Monitors oil level using a non-contact capacitive sensor mounted on the sight glass
- Sends an immediate Telegram alert when oil drops below the sensor
- Sends hourly reminders until the tank is refilled
- Sends a confirmation message when the level is restored
- Supports up to 3 Telegram chat IDs — notify multiple people
- DHCP or static IP support, configurable from the web interface
- Password-protected web interface with session timeout
- OTA firmware updates via the web interface — no USB needed after initial flash
- Factory reset via web interface or by holding the BOOT button for 5 seconds
- All settings saved to flash — survives power cycles
- Auto-reconnects WiFi if the connection drops
- Debounced sensor readings to prevent false alerts

## Hardware

| Component | Details | Price |
|-----------|---------|-------|
| ESP32 Dev Board | ESP-WROOM-32 with CP210x USB | [$9.99 (1-pack)](https://www.amazon.com/HiLetgo-ESP-WROOM-32-Development-Microcontroller-Integrated/dp/B0718T232Z) or [$17.99 (3-pack)](https://www.amazon.com/HiLetgo-ESP-WROOM-32-Bluetooth-ESP32-DevKitC-32-Development/dp/B0CNYK7WT2) |
| XKC-Y25-V Sensor | Non-contact capacitive liquid level sensor | [$8.34 (1-pack)](https://www.amazon.com/caralin-XKC-Y25-V-Non-Contact-Liquid-Induction/dp/B0FT2CG9B2) or [$15.99 (4-pack)](https://www.amazon.com/DEVMO-Non-Contact-Induction-Detector-XKC-Y25-V/dp/B07TB3KZX7) |
| Jumper Wires | Female-to-female Dupont jumpers (or solder direct) | [$6.98](https://www.amazon.com/EDGELEC-Breadboard-Optional-Assorted-Multicolored/dp/B07GD2BWPY) |
| USB Power Adapter | Any 5V/1A USB adapter | ~$5 |
| USB Cable | Micro-USB **data** cable (not charge-only) | ~$5 |

**Total cost: under $30**

### ESP32 Dev Board

![ESP32-WROOM-32 Development Board](images/ESP-32.jpg)

### ESP32 Pin Diagram

![ESP32 Pin Diagram](images/ESP-32_Pin_Diagram.jpg)

### XKC-Y25-V Liquid Level Sensor

![XKC-Y25-V Non-Contact Liquid Level Sensor](images/XKC-Y25-V.jpg)

### Jumper Wires

![Female-to-Female Dupont Jumper Wires](images/BreadBoard_Jumper_Wires.jpg)

Female-to-female Dupont jumpers connect the sensor's three leads directly to the ESP32 header pins — no soldering or breadboard required.

### Wiring

| XKC-Y25-V Wire | ESP32 Pin |
|----------------|-----------|
| Brown | 3V3 (Pin 1) |
| Blue | GND (Pin 38) |
| Yellow | GPIO4 / D4 (Pin 26) |

Refer to the pin diagram above to locate the correct pins on your board.

### Assembled Hardware

![Assembled ESP32 + XKC-Y25-V on USB power](images/Exposed_Working_Sensor_With_Power.jpg)

A complete build: ESP32 powered over USB with the XKC-Y25-V sensor wired in. The sensor's red LED lights when liquid is detected.

## Setup

### 1. Telegram Bot

1. Open Telegram and message [@BotFather](https://t.me/BotFather)
2. Send `/newbot` and follow the prompts to create your bot
3. Save the **bot token** it gives you
4. Message [@userinfobot](https://t.me/userinfobot) to get your **chat ID**
5. (Optional) Get chat IDs for up to 2 additional people to notify

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
4. Enter your Telegram bot token and chat ID(s)
5. Optionally configure a static IP instead of DHCP
6. Set a web interface password (default: `admin` / `admin`)
7. Click **Save & Restart**

The device will reboot, connect to your WiFi, and send a Telegram message confirming it's online — including its IP address for future access to the settings page.

#### Mobile Setup Screens

| WiFi & Telegram | Network, Password & Firmware |
|-----------------|-------------------------------|
| ![Setup form top half](images/ESP-32_Monitor_Interface_1.jpg) | ![Setup form bottom half](images/ESP-32_Monitor_Interface_2.jpg) |

The setup portal at `http://192.168.4.1` is mobile-friendly — phone, tablet, or laptop all work.

### 4. Install

1. Attach the XKC-Y25-V sensor to the sight glass tube at your desired low-level threshold
2. Power the ESP32 from a USB adapter near the tank
3. Keep the USB cable under 5 meters — if you need more distance, use a longer extension cord to the adapter instead

#### Portable / Backup Power

![ESP32 powered by a USB power bank](images/Exposed_Working_Sensor_With_BatteryPower.jpg)

The ESP32 runs on any 5V USB source, so a power bank works for bench testing or short outages. For permanent install, use a wall adapter — the always-on web portal will drain a typical 10,000 mAh bank in roughly 1–2 days.

### Reconfiguring

You can access the settings page at any time by visiting the device's IP address in a browser (login: `admin` + your password). If the device can't connect to WiFi (e.g. after a network change), it will automatically fall back to AP mode so you can reconfigure.

## Web Interface Features

- **Session-based authentication** with 15-minute inactivity timeout
- **Sensitive fields** (bot token, chat IDs) are masked by default with eye toggle to reveal
- **OTA firmware updates** — upload a `.bin` file to update without USB
- **Factory reset** — from the web interface (with confirmation) or by holding the BOOT button 5 seconds
- **DHCP/Static IP toggle** — configure network settings from the browser
- **Auto-redirect** — after saving settings or updating firmware, the page counts down and redirects back

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
  "uptime_sec": 3600,
  "firmware": "1.1.0"
}
```

## Sensor Notes

The XKC-Y25-V is a capacitive sensor that detects liquid through glass without any contact with the fluid. It outputs HIGH when liquid is present and LOW when absent. The firmware includes a 3-read debounce to prevent false triggers from sloshing or vibration.

### How the Sensor Reads Liquid

| Liquid above sensor (HIGH) | Liquid below sensor (LOW) |
|----------------------------|----------------------------|
| ![Sensor detecting liquid — red LED on](images/Liquid_High_Level.jpg) | ![Sensor sees no liquid — LED off](images/Liquid_Low_Level.jpg) |
| Red LED on, signal HIGH, oil **present** — no alert. | LED off, signal LOW, oil **absent** — Telegram alert triggers after debounce. |

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
| `SESSION_TIMEOUT_MS` | `900000` (15 min) | Web interface session inactivity timeout |

## Contributing

Contributions are welcome. Some ideas:

- Multiple sensor support for different tank levels (e.g. low, critical)
- Battery-powered deep sleep mode
- MQTT integration for Home Assistant
- Level history logging and graphing on the web interface
- Temperature compensation for sensor accuracy
- Email or SMS notifications as an alternative to Telegram
- Percentage-based level estimation with multiple sensors

Fork the repo, make your changes, and open a pull request.

## License

[MIT](LICENSE)
