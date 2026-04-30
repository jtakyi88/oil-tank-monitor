# Interchangeable Sensor Support — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add runtime-selectable sensor support (digital threshold + ToF distance) to the ESP32 oil tank monitor while preserving v1.x behavior on upgrade.

**Architecture:** Single `OilTankMonitor.ino` file (monolithic, per spec). Tagged-struct dispatch via `SensorType` enum and `SensorReading` struct. Four-state level machine for ToF (`LOW`, `BELOW_HALF`, `ABOVE_HALF`, `HIGH`) with bidirectional Half notifications. Additive NVS schema (4 new keys) so v1.x → v2.0 OTA upgrade is zero-friction.

**Tech Stack:** Arduino C++ / ESP32 / arduino-cli / Adafruit_VL53L0X / UniversalTelegramBot / WebServer / Preferences (NVS).

**Spec reference:** `docs/superpowers/specs/2026-04-28-interchangeable-sensors-design.md` (commit `482975c`).

**Testing note:** This codebase has no unit-test framework. "Verify" steps in this plan use compile checks (`arduino-cli compile`), Serial monitor output, `curl` against `/status`, and observed Telegram messages — not pytest-style assertions. This is the manual integration approach explicitly chosen in the spec.

---

## File structure

| File | Status | Responsibility |
|---|---|---|
| `OilTankMonitor/OilTankMonitor.ino` | Modify | All firmware code (sensor abstraction, state machine, web UI, persistence, Telegram, OTA) |
| `README.md` | Modify | Hardware BOM, wiring, library install, sensor selection guide |
| `docs/superpowers/specs/2026-04-28-interchangeable-sensors-design.md` | Reference only | Approved design — already committed |

No new files created. Decision per spec Section "File layout": monolithic single `.ino` is preferred for newcomer readability.

---

## Common commands

| Action | Command |
|---|---|
| Compile | `arduino-cli compile --fqbn esp32:esp32:esp32 OilTankMonitor` |
| Flash (USB) | `arduino-cli upload --fqbn esp32:esp32:esp32:UploadSpeed=115200 --port /dev/ttyUSB0 OilTankMonitor` |
| Serial monitor | `arduino-cli monitor --port /dev/ttyUSB0 --config baudrate=115200` |
| Status check | `curl -u admin:<password> http://<device-ip>/status \| jq` |

All commands run from `/home/juliettango/oil-monitor`.

---

## Task 0: Setup — feature branch and library install

**Files:** none (environment only)

- [ ] **Step 1: Create feature branch**

```bash
cd /home/juliettango/oil-monitor
git checkout -b feat/v2-interchangeable-sensors
```

- [ ] **Step 2: Install Adafruit_VL53L0X library**

```bash
arduino-cli lib install "Adafruit_VL53L0X"
```

Expected output: a line like `Adafruit_VL53L0X@1.2.x installed`.

- [ ] **Step 3: Verify library resolves**

```bash
arduino-cli lib list | grep -i vl53l0x
```

Expected: `Adafruit_VL53L0X   1.2.x   ...`

- [ ] **Step 4: Compile current main as baseline**

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 OilTankMonitor 2>&1 | tail -5
```

Expected: clean build, sketch size reported. No errors. Note the byte count — useful for comparing growth after each task.

- [ ] **Step 5: No commit needed (no files changed)**

---

## Task 1: Add SensorType enum, SensorReading struct, and constants

**Files:**
- Modify: `OilTankMonitor/OilTankMonitor.ino` (insert after line 25 `#include <Update.h>`)

- [ ] **Step 1: Add new types and constants block**

Insert immediately after `#include <Update.h>` (line 25) and before `// ===== FIRMWARE VERSION =====` (line 27):

```cpp
#include <Wire.h>
#include <Adafruit_VL53L0X.h>

// ===== SENSOR ABSTRACTION =====
enum SensorType { SENSOR_DIGITAL = 0, SENSOR_TOF = 1 };

struct SensorReading {
  bool valid;            // false on hardware fault (I2C timeout, ToF out-of-range)
  bool digitalState;     // HIGH = object/liquid present; meaningful when SENSOR_DIGITAL
  uint16_t distanceMm;   // mm to puck; meaningful when SENSOR_TOF
};

const int I2C_SDA_PIN = 21;            // ESP32 default
const int I2C_SCL_PIN = 22;            // ESP32 default
const int TOF_HYSTERESIS_MM = 5;       // band around each threshold
const int TOF_MIN_MM = 30;             // VL53L0X spec lower bound
const int TOF_MAX_MM = 2000;           // VL53L0X spec upper bound (mm we accept)
const int TOF_FAULT_CYCLES = 5;        // consecutive invalid reads → fault alert
const int TOF_RECOVERY_CYCLES = 3;     // consecutive invalid reads → I2C reinit attempt
```

- [ ] **Step 2: Compile**

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 OilTankMonitor 2>&1 | tail -5
```

Expected: clean build. The new includes should resolve.

- [ ] **Step 3: Commit**

```bash
git add OilTankMonitor/OilTankMonitor.ino
git commit -m "Add SensorType, SensorReading, and ToF constants"
```

---

## Task 2: Add sensor-config globals and NVS persistence

**Files:**
- Modify: `OilTankMonitor/OilTankMonitor.ino` — globals block (around line 53), `loadSettings()` (line 114), `saveSettings()` (line 132)

- [ ] **Step 1: Add new globals next to existing config variables**

Find the block beginning with `// Saved settings` (~line 53) and insert these new variables after `String cfgWebPassword = "admin";` (line 66):

```cpp
// Sensor configuration
SensorType cfgSensorType = SENSOR_DIGITAL;   // default — protects v1.x upgraders
uint16_t cfgTofLow  = 200;                   // mm — alert threshold
uint16_t cfgTofHalf = 130;                   // mm — half mark
uint16_t cfgTofHigh = 60;                    // mm — refill complete
```

- [ ] **Step 2: Update `loadSettings()` to read new keys with defaults**

Find `loadSettings()` (line 114) and add these four lines immediately before `prefs.end();` (line 128):

```cpp
  cfgSensorType = (SensorType)prefs.getUChar("sensor_type", 0);
  cfgTofLow     = prefs.getUShort("tof_low", 200);
  cfgTofHalf    = prefs.getUShort("tof_half", 130);
  cfgTofHigh    = prefs.getUShort("tof_high", 60);
```

- [ ] **Step 3: Update `saveSettings()` to write new keys**

Find `saveSettings()` (line 132) and add these four lines immediately before `prefs.end();` (line 146):

```cpp
  prefs.putUChar("sensor_type", (uint8_t)cfgSensorType);
  prefs.putUShort("tof_low", cfgTofLow);
  prefs.putUShort("tof_half", cfgTofHalf);
  prefs.putUShort("tof_high", cfgTofHigh);
```

- [ ] **Step 4: Add Serial-print of loaded values in `setup()`**

In `setup()`, immediately after the `loadSettings();` call (line 662), add:

```cpp
  Serial.printf("Sensor type: %s | ToF thresholds (mm): low=%u half=%u high=%u\n",
                cfgSensorType == SENSOR_DIGITAL ? "DIGITAL" : "TOF",
                cfgTofLow, cfgTofHalf, cfgTofHigh);
```

- [ ] **Step 5: Compile, flash, verify Serial output**

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 OilTankMonitor
arduino-cli upload --fqbn esp32:esp32:esp32:UploadSpeed=115200 --port /dev/ttyUSB0 OilTankMonitor
arduino-cli monitor --port /dev/ttyUSB0 --config baudrate=115200
```

Expected Serial line on boot: `Sensor type: DIGITAL | ToF thresholds (mm): low=200 half=130 high=60`

For an existing v1.x device upgrading: this line confirms zero-friction default — sensor type is DIGITAL, behavior unchanged.

- [ ] **Step 6: Commit**

```bash
git add OilTankMonitor/OilTankMonitor.ino
git commit -m "Add sensor-config globals and NVS persistence"
```

---

## Task 3: Replace `readSensorDebounced()` with `initSensor()` + `readSensor()` (digital path only)

**Files:**
- Modify: `OilTankMonitor/OilTankMonitor.ino` — sensor section (lines 627–645), `setup()` (line 651), `loop()` (line 693)

- [ ] **Step 1: Replace the existing sensor section**

Delete lines 627–645 (the existing `// ===== Sensor =====` block including `readSensorDebounced()`) and replace with:

```cpp
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
  Serial.println("Sensor: TOF init not yet implemented");
  return false;
}

SensorReading readSensorRaw() {
  SensorReading r = { false, false, 0 };
  if (cfgSensorType == SENSOR_DIGITAL) {
    r.digitalState = (digitalRead(SENSOR_PIN) == HIGH);
    r.valid = true;
    return r;
  }
  // ToF path — implemented in Task 4
  return r;
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
  // ToF filtering implemented in Task 5
  return raw;
}
```

- [ ] **Step 2: Update `setup()` to call `initSensor()` instead of plain `pinMode`**

Find `setup()` (line 651). Locate `pinMode(SENSOR_PIN, INPUT);` (line 656) and replace with:

```cpp
  initSensor();
```

- [ ] **Step 3: Update `loop()` to use the new return type**

Find `loop()` (line 693). Locate the block (line 712–727):

```cpp
  if (now - lastSensorCheck >= SENSOR_CHECK_MS) {
    lastSensorCheck = now;

    bool currentlyLow = readSensorDebounced();

    if (currentlyLow && !oilIsLow) {
      ...
```

Replace `bool currentlyLow = readSensorDebounced();` with:

```cpp
    SensorReading reading = readSensor();
    // For digital sensor: digitalState HIGH = liquid present; LOW = no liquid (oil low)
    bool currentlyLow = reading.valid && !reading.digitalState;
```

The rest of the if/else transition block is unchanged for now — Task 7 will replace it with the state machine.

- [ ] **Step 4: Compile**

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 OilTankMonitor 2>&1 | tail -5
```

Expected: clean build.

- [ ] **Step 5: Flash and verify v1.x behavior unchanged (digital regression)**

Flash, then test with the actual XKC-Y25-V (or jumper GPIO4 to GND/3V3 manually):

```bash
arduino-cli upload --fqbn esp32:esp32:esp32:UploadSpeed=115200 --port /dev/ttyUSB0 OilTankMonitor
arduino-cli monitor --port /dev/ttyUSB0 --config baudrate=115200
```

Expected: Serial shows `Sensor: DIGITAL on GPIO4`. Disconnecting the sensor signal (or pulling GPIO4 LOW with a jumper) for 15 seconds → Telegram `⚠️ OIL TANK LOW`. Reconnecting → `✅ Oil tank level restored`.

- [ ] **Step 6: Commit**

```bash
git add OilTankMonitor/OilTankMonitor.ino
git commit -m "Replace readSensorDebounced with dispatch-based initSensor/readSensor (digital path)"
```

---

## Task 4: Add ToF init and read paths

**Files:**
- Modify: `OilTankMonitor/OilTankMonitor.ino` — `initSensor()` and `readSensorRaw()` (created in Task 3)

- [ ] **Step 1: Implement ToF init**

In `initSensor()` (defined in Task 3), replace the placeholder lines after `// ToF path — implemented in Task 4` with:

```cpp
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  if (!tofSensor.begin()) {
    Serial.println("Sensor: TOF init FAILED — check I2C wiring (SDA=21, SCL=22, VCC=3V3, GND=GND)");
    return false;
  }
  Serial.println("Sensor: TOF (VL53L0X) on I2C SDA=" + String(I2C_SDA_PIN) + " SCL=" + String(I2C_SCL_PIN));
  return true;
```

Remove the `Serial.println("Sensor: TOF init not yet implemented");` line and `return false;` placeholder.

- [ ] **Step 2: Implement ToF raw read**

In `readSensorRaw()` (defined in Task 3), replace the placeholder lines after `// ToF path — implemented in Task 4` with:

```cpp
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
```

- [ ] **Step 3: Compile**

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 OilTankMonitor 2>&1 | tail -5
```

Expected: clean build. Sketch size grows by ~20–30 KB (VL53L0X library).

- [ ] **Step 4: Wire VL53L0X module to ESP32 (hardware step)**

Disconnect USB power. Wire:
- VL53L0X VCC → ESP32 3V3 (Pin 1)
- VL53L0X GND → ESP32 GND (Pin 38)
- VL53L0X SDA → ESP32 GPIO21 (Pin 33)
- VL53L0X SCL → ESP32 GPIO22 (Pin 36)

Reconnect USB.

- [ ] **Step 5: Temporarily force ToF mode for testing**

Without web UI yet, hardcode the sensor type for this verification only. In `setup()`, immediately after `loadSettings();`, add a temporary line:

```cpp
  cfgSensorType = SENSOR_TOF;   // TEMP — remove in Task 9 after web UI lands
```

- [ ] **Step 6: Add a debug print of the live ToF reading in `loop()`**

In `loop()`, inside the `if (now - lastSensorCheck >= SENSOR_CHECK_MS)` block (after the `SensorReading reading = readSensor();` line from Task 3), add a temporary print:

```cpp
    if (cfgSensorType == SENSOR_TOF) {
      Serial.printf("ToF: valid=%d distance=%u mm\n", reading.valid, reading.distanceMm);
    }
```

- [ ] **Step 7: Flash and verify live distance**

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 OilTankMonitor
arduino-cli upload --fqbn esp32:esp32:esp32:UploadSpeed=115200 --port /dev/ttyUSB0 OilTankMonitor
arduino-cli monitor --port /dev/ttyUSB0 --config baudrate=115200
```

Expected: Serial line every 5 s like `ToF: valid=1 distance=87 mm`. Move a hand or object in front of the sensor — distance value should change. Cover the sensor → low number (~30 mm). Aim at empty space → high number (or `valid=0` for out-of-range).

- [ ] **Step 8: Remove the temporary `cfgSensorType = SENSOR_TOF;` line and the debug print**

Restore those two temporary changes — they were verification scaffolding only.

- [ ] **Step 9: Compile clean and commit**

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 OilTankMonitor
git add OilTankMonitor/OilTankMonitor.ino
git commit -m "Add ToF (VL53L0X) init and raw read paths"
```

---

## Task 5: Add ToF median-of-3 filtering and hysteresis

**Files:**
- Modify: `OilTankMonitor/OilTankMonitor.ino` — `readSensor()` (created Task 3)

- [ ] **Step 1: Add a median-of-3 helper**

Insert immediately before `readSensor()`:

```cpp
uint16_t medianOf3(uint16_t a, uint16_t b, uint16_t c) {
  if ((a >= b && a <= c) || (a <= b && a >= c)) return a;
  if ((b >= a && b <= c) || (b <= a && b >= c)) return b;
  return c;
}
```

- [ ] **Step 2: Replace the placeholder ToF path in `readSensor()`**

In `readSensor()`, replace the trailing `// ToF filtering implemented in Task 5\n  return raw;` with:

```cpp
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
```

- [ ] **Step 3: Compile**

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 OilTankMonitor 2>&1 | tail -5
```

Expected: clean build.

- [ ] **Step 4: Commit**

```bash
git add OilTankMonitor/OilTankMonitor.ino
git commit -m "Add ToF median-of-3 filtering"
```

---

## Task 6: Add `LevelState` enum and bucketing function

**Files:**
- Modify: `OilTankMonitor/OilTankMonitor.ino` — sensor section (after Task 5 changes), main globals (~line 70)

- [ ] **Step 1: Define `LevelState` enum near the SensorType enum**

In the `// ===== SENSOR ABSTRACTION =====` block (added in Task 1), add immediately after the `SensorReading` struct:

```cpp
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
```

- [ ] **Step 2: Add a `currentState` global**

In the existing globals block (after the sensor-config globals from Task 2), add:

```cpp
LevelState currentState = LEVEL_UNKNOWN;
```

- [ ] **Step 3: Add a `bucketReading()` function**

Insert in the sensor section, immediately after `readSensor()`:

```cpp
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
```

- [ ] **Step 4: Compile**

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 OilTankMonitor 2>&1 | tail -5
```

Expected: clean build.

- [ ] **Step 5: Commit**

```bash
git add OilTankMonitor/OilTankMonitor.ino
git commit -m "Add LevelState enum and hysteresis-aware bucketReading"
```

---

## Task 7: Replace `loop()` transition logic with state-machine + transition messages

**Files:**
- Modify: `OilTankMonitor/OilTankMonitor.ino` — `loop()` (lines ~693–736 originally; will have shifted)

- [ ] **Step 1: Add a transition-message dispatch function**

Insert immediately before `setup()`:

```cpp
// Fire the right Telegram message for each transition direction.
void fireTransitionMessage(LevelState from, LevelState to) {
  // Refill direction (smaller mm / liquid restored)
  if (from == LEVEL_LOW && to >= LEVEL_BELOW_HALF) {
    sendTelegram("✅ Oil tank above low mark — refill detected.");
  }
  if (from <= LEVEL_BELOW_HALF && to >= LEVEL_ABOVE_HALF) {
    sendTelegram("🔼 Tank is half refilled.");
  }
  if (from <= LEVEL_ABOVE_HALF && to == LEVEL_HIGH) {
    sendTelegram("🔝 Tank at HIGH — refill complete.");
  }
  // Drain direction (larger mm)
  if (from == LEVEL_HIGH && to <= LEVEL_ABOVE_HALF) {
    // intentionally silent — normal use
  }
  if (from >= LEVEL_ABOVE_HALF && to <= LEVEL_BELOW_HALF && to != LEVEL_LOW) {
    sendTelegram("📉 Tank is half empty — plan a refill.");
  }
  if (from >= LEVEL_BELOW_HALF && to == LEVEL_LOW) {
    sendTelegram("⚠️ OIL TANK LOW — please refill.");
  }
}
```

Note: the conditions handle skipped buckets correctly. A jump from `LEVEL_LOW` to `LEVEL_HIGH` (rapid refill) fires all three refill messages in order because each `if` triggers independently when the from→to transition spans its trigger.

- [ ] **Step 2: Replace the loop's transition block**

In `loop()`, find this block:

```cpp
  if (now - lastSensorCheck >= SENSOR_CHECK_MS) {
    lastSensorCheck = now;

    SensorReading reading = readSensor();
    // For digital sensor: digitalState HIGH = liquid present; LOW = no liquid (oil low)
    bool currentlyLow = reading.valid && !reading.digitalState;

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
```

Replace with:

```cpp
  if (now - lastSensorCheck >= SENSOR_CHECK_MS) {
    lastSensorCheck = now;

    SensorReading reading = readSensor();
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
```

- [ ] **Step 3: Compile**

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 OilTankMonitor 2>&1 | tail -5
```

Expected: clean build.

- [ ] **Step 4: Flash and run digital regression**

With the digital sensor (XKC-Y25-V or jumper) connected to GPIO4:

```bash
arduino-cli upload --fqbn esp32:esp32:esp32:UploadSpeed=115200 --port /dev/ttyUSB0 OilTankMonitor
arduino-cli monitor --port /dev/ttyUSB0 --config baudrate=115200
```

Expected: Serial shows `Boot: initial level=HIGH` (or LOW depending on jumper). Pulling jumper to GND → `Transition: HIGH -> LOW`, Telegram `⚠️ OIL TANK LOW`. Restoring → `Transition: LOW -> HIGH`, Telegram `✅ Oil tank above low mark — refill detected.` (note: digital path goes straight LOW→HIGH, but the refill conditions still fire — see Step 5 below).

- [ ] **Step 5: Verify digital refill message**

The refill condition `from == LEVEL_LOW && to >= LEVEL_BELOW_HALF` fires for digital LOW→HIGH because `LEVEL_HIGH > LEVEL_BELOW_HALF` numerically. The `to >= LEVEL_ABOVE_HALF` and `to == LEVEL_HIGH` conditions also fire — meaning a digital LOW→HIGH transition would trigger three refill messages in rapid succession.

**This is a bug.** Fix by guarding the multi-state messages on `cfgSensorType == SENSOR_TOF`:

Edit `fireTransitionMessage` so the half-refill, high-reached, and half-empty messages only fire for ToF:

```cpp
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
```

- [ ] **Step 6: Recompile, flash, repeat digital regression**

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 OilTankMonitor
arduino-cli upload --fqbn esp32:esp32:esp32:UploadSpeed=115200 --port /dev/ttyUSB0 OilTankMonitor
```

Pull jumper LOW for 15 s → expect single `⚠️ OIL TANK LOW` Telegram. Restore → expect single `✅ Oil tank level restored` Telegram. No duplicate / extraneous refill messages.

- [ ] **Step 7: Commit**

```bash
git add OilTankMonitor/OilTankMonitor.ino
git commit -m "Replace loop transition logic with state-machine and per-sensor messages"
```

---

## Task 8: Boot-time enhanced online message

**Files:**
- Modify: `OilTankMonitor/OilTankMonitor.ino` — `setup()` (~line 651), specifically the `sendTelegram(...)` call after WiFi connects

- [ ] **Step 1: Capture initial reading after WiFi connects, before sending online message**

Find the lines in `setup()` after `connectWiFi()` succeeds (originally around line 672–675):

```cpp
    } else {
      apMode = false;
      initBot();
      sendTelegram("🛢️ Oil tank monitor is ONLINE and watching the level.\nSettings: http://" + WiFi.localIP().toString());
    }
```

Replace with:

```cpp
    } else {
      apMode = false;
      initBot();
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
```

- [ ] **Step 2: Compile, flash, verify**

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 OilTankMonitor
arduino-cli upload --fqbn esp32:esp32:esp32:UploadSpeed=115200 --port /dev/ttyUSB0 OilTankMonitor
```

Expected on boot: single Telegram message of the form:

```
🛢️ Oil tank monitor is ONLINE.
Sensor: Digital
Level: HIGH
Settings: http://192.168.x.x
```

No transition messages should fire on boot, even if the level is something other than HIGH.

- [ ] **Step 3: Commit**

```bash
git add OilTankMonitor/OilTankMonitor.ino
git commit -m "Enhance boot-online Telegram message with sensor type and current level"
```

---

## Task 9: Sensor fault detection and I2C recovery

**Files:**
- Modify: `OilTankMonitor/OilTankMonitor.ino` — `loop()` after the `readSensor()` call, plus globals declared in Task 3

- [ ] **Step 1: Add fault tracking inside the loop's sensor-check block**

Inside `loop()`, in the block `if (now - lastSensorCheck >= SENSOR_CHECK_MS)`, immediately after `SensorReading reading = readSensor();` and before `LevelState newState = ...`, insert:

```cpp
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
```

- [ ] **Step 2: Add ToF init-failure Telegram on boot**

In `setup()`, after the `initBot();` line and before the boot-online message you added in Task 8, insert:

```cpp
      bool sensorOk = initSensor();
      if (!sensorOk && cfgSensorType == SENSOR_TOF) {
        sendTelegram("⚠️ ToF init failed — check I2C wiring or switch sensor type via web UI.");
      }
```

Then **delete** the existing `initSensor();` call inside `setup()` that you added in Task 3 (was a top-level call; now we want it only inside the WiFi-connected branch so we have Telegram available for the failure message).

Wait — `initSensor()` should run regardless of WiFi state because the digital path needs `pinMode` even in AP mode. Restore the original placement and only ADD the Telegram alert. Final structure in `setup()`:

```cpp
  // Earlier in setup(), unchanged from Task 3:
  initSensor();

  // ... loadSettings, connectWiFi etc ...

    } else {
      apMode = false;
      initBot();
      // If ToF init silently failed earlier, alert now that we have Telegram
      if (cfgSensorType == SENSOR_TOF && !tofSensor.begin()) {
        sendTelegram("⚠️ ToF init failed — check I2C wiring or switch sensor type via web UI.");
      }
      delay(200);
      SensorReading r = readSensor();
      // ... rest of Task 8 changes ...
```

- [ ] **Step 3: Compile**

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 OilTankMonitor 2>&1 | tail -5
```

Expected: clean build.

- [ ] **Step 4: Flash and verify fault path (requires temporarily forcing ToF mode)**

For verification only, temporarily set `cfgSensorType = SENSOR_TOF;` in `setup()` after `loadSettings()` (this temp change will be removed after the web UI exists in Task 10):

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 OilTankMonitor
arduino-cli upload --fqbn esp32:esp32:esp32:UploadSpeed=115200 --port /dev/ttyUSB0 OilTankMonitor
```

With VL53L0X disconnected (no I2C wires): expect a single `⚠️ Sensor fault` Telegram after ~25 s. Reconnect wires: Serial shows `ToF: readings recovered`, no further messages until next state transition.

Remove the temporary `cfgSensorType = SENSOR_TOF;` line.

- [ ] **Step 5: Commit**

```bash
git add OilTankMonitor/OilTankMonitor.ino
git commit -m "Add sensor fault detection and I2C bus recovery"
```

---

## Task 10: Web UI — sensor dropdown, conditional fields, and JS toggle

**Files:**
- Modify: `OilTankMonitor/OilTankMonitor.ino` — `buildConfigPage()` (~line 185)

- [ ] **Step 1: Add the Sensor Configuration HTML block**

In `buildConfigPage()`, find the line that begins the *Network Settings* section: `page += "<h2>Network Settings</h2>";`. Insert a new block immediately before it:

```cpp
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
```

- [ ] **Step 2: Reuse the existing `.ip-fields` CSS pattern for `.tof-fields`**

In `htmlHeader()`, find this line:

```cpp
  h += ".ip-fields{display:none;}.ip-fields.show{display:block;}";
```

Replace with:

```cpp
  h += ".ip-fields,.tof-fields{display:none;}.ip-fields.show,.tof-fields.show{display:block;}";
```

- [ ] **Step 3: Add `toggleTof()` JS function**

In `buildConfigPage()`, find the existing `<script>` block at the bottom (around `function toggleStatic()`...). Add `toggleTof()` inside the same script block:

```cpp
  page += "function toggleTof(){";
  page += "  var v=document.getElementById('sensor_type').value;";
  page += "  document.getElementById('tof-fields').classList.toggle('show', v==='1');";
  page += "}";
```

- [ ] **Step 4: Update `handleSave()` to read new fields**

Find `handleSave()` (~line 466). After the existing `cfgDNS = server.arg("dns");` line, insert:

```cpp
  // Sensor configuration
  if (server.hasArg("sensor_type")) {
    int t = server.arg("sensor_type").toInt();
    cfgSensorType = (t == 1) ? SENSOR_TOF : SENSOR_DIGITAL;
  }
  if (server.hasArg("tof_low"))  cfgTofLow  = server.arg("tof_low").toInt();
  if (server.hasArg("tof_half")) cfgTofHalf = server.arg("tof_half").toInt();
  if (server.hasArg("tof_high")) cfgTofHigh = server.arg("tof_high").toInt();
```

- [ ] **Step 5: Compile, flash, manual UI test**

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 OilTankMonitor
arduino-cli upload --fqbn esp32:esp32:esp32:UploadSpeed=115200 --port /dev/ttyUSB0 OilTankMonitor
```

Open `http://<device-ip>` in a browser, log in. Expected:
- New "Sensor Configuration" section visible between Telegram and Network Settings
- Dropdown shows "Digital threshold" selected
- ToF fields hidden
- Switch dropdown to "ToF distance" → ToF threshold fields appear
- Save with sensor_type=1 → device restarts → Serial shows `Sensor type: TOF`

- [ ] **Step 6: Commit**

```bash
git add OilTankMonitor/OilTankMonitor.ino
git commit -m "Add web UI sensor dropdown and conditional ToF threshold fields"
```

---

## Task 11: Web UI — live distance polling

**Files:**
- Modify: `OilTankMonitor/OilTankMonitor.ino` — `buildConfigPage()` script block

- [ ] **Step 1: Add a JS polling block for live ToF distance**

In `buildConfigPage()`, append to the existing `<script>` block (after `toggleTof()`):

```cpp
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
```

(The `/status` endpoint will be updated to include `distance_mm` in Task 13. Until then, the JS just shows `—`.)

- [ ] **Step 2: Compile**

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 OilTankMonitor 2>&1 | tail -5
```

Expected: clean build. Visual verification of live polling deferred to Task 13.

- [ ] **Step 3: Commit**

```bash
git add OilTankMonitor/OilTankMonitor.ino
git commit -m "Add live ToF distance polling on config page"
```

---

## Task 12: `handleSave()` validation (range and ordering)

**Files:**
- Modify: `OilTankMonitor/OilTankMonitor.ino` — `handleSave()` (~line 466)

- [ ] **Step 1: Add a helper to render an inline-error page**

Insert immediately before `handleSave()`:

```cpp
void sendValidationError(const String& message) {
  String page = htmlHeader("Configuration Error");
  page += "<h1>Configuration Error</h1>";
  page += "<div class='status warn'>" + message + "</div>";
  page += "<div class='nav' style='margin-top:16px;'><a href='/'>&larr; Back to Settings</a></div>";
  page += htmlFooter();
  server.send(400, "text/html", page);
}
```

- [ ] **Step 2: Insert validation in `handleSave()` before `saveSettings();`**

In `handleSave()`, immediately after the lines that read sensor_type / tof_low / tof_half / tof_high (added in Task 10) and before `saveSettings();`, add:

```cpp
  if (cfgSensorType == SENSOR_TOF) {
    if (cfgTofHigh < TOF_MIN_MM || cfgTofHigh > TOF_MAX_MM ||
        cfgTofHalf < TOF_MIN_MM || cfgTofHalf > TOF_MAX_MM ||
        cfgTofLow  < TOF_MIN_MM || cfgTofLow  > TOF_MAX_MM) {
      sendValidationError("Each ToF threshold must be between " + String(TOF_MIN_MM) + " and " + String(TOF_MAX_MM) + " mm.");
      return;
    }
    if (!(cfgTofHigh < cfgTofHalf && cfgTofHalf < cfgTofLow)) {
      sendValidationError("ToF thresholds must satisfy HIGH &lt; HALF &lt; LOW (smaller mm = fuller tank). Got HIGH=" + String(cfgTofHigh) + " HALF=" + String(cfgTofHalf) + " LOW=" + String(cfgTofLow) + ".");
      return;
    }
  }
```

This runs only when ToF is selected. Returns `400` and an inline error page; does not call `saveSettings()` or `ESP.restart()`.

- [ ] **Step 3: Compile, flash, manual test**

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 OilTankMonitor
arduino-cli upload --fqbn esp32:esp32:esp32:UploadSpeed=115200 --port /dev/ttyUSB0 OilTankMonitor
```

In the browser:
- Switch to ToF, set high=100 half=130 low=60 → Save → expect error page about ordering
- Set high=0 → Save → expect error page about range
- Set valid values (high=60 half=130 low=200) → Save → device restarts as expected

- [ ] **Step 4: Commit**

```bash
git add OilTankMonitor/OilTankMonitor.ino
git commit -m "Add ToF threshold validation (range and ordering)"
```

---

## Task 13: `/status` JSON additions

**Files:**
- Modify: `OilTankMonitor/OilTankMonitor.ino` — `handleStatus()` (~line 507)

- [ ] **Step 1: Cache the last reading for `/status`**

Add a global near the existing sensor-state globals:

```cpp
SensorReading lastReading = { false, false, 0 };
```

In `loop()`, inside `if (now - lastSensorCheck >= SENSOR_CHECK_MS)`, immediately after `SensorReading reading = readSensor();`, add:

```cpp
    lastReading = reading;
```

- [ ] **Step 2: Update `handleStatus()`**

Replace the existing `handleStatus()` body with:

```cpp
void handleStatus() {
  if (!requireAuth()) return;
  String json = "{";
  json += "\"oil_low\":" + String(oilIsLow ? "true" : "false") + ",";
  json += "\"wifi_connected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",";
  json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"ssid\":\"" + cfgSSID + "\",";
  json += "\"uptime_sec\":" + String(millis() / 1000) + ",";
  json += "\"firmware\":\"" + String(FW_VERSION) + "\",";
  json += "\"sensor_type\":\"" + String(cfgSensorType == SENSOR_DIGITAL ? "digital" : "tof") + "\",";
  json += "\"sensor_valid\":" + String(lastReading.valid ? "true" : "false") + ",";
  json += "\"level\":\"" + String(levelStateName(currentState)) + "\"";
  if (cfgSensorType == SENSOR_TOF) {
    json += ",\"distance_mm\":" + String(lastReading.distanceMm);
    json += ",\"thresholds\":{";
    json += "\"low\":" + String(cfgTofLow) + ",";
    json += "\"half\":" + String(cfgTofHalf) + ",";
    json += "\"high\":" + String(cfgTofHigh);
    json += "}";
  }
  json += "}";
  server.send(200, "application/json", json);
}
```

- [ ] **Step 3: Compile, flash, curl test**

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 OilTankMonitor
arduino-cli upload --fqbn esp32:esp32:esp32:UploadSpeed=115200 --port /dev/ttyUSB0 OilTankMonitor
curl -s -c /tmp/c -b /tmp/c -d "password=admin" http://<device-ip>/login
curl -s -b /tmp/c http://<device-ip>/status | jq
```

Expected (digital):

```json
{
  "oil_low": false,
  ...
  "sensor_type": "digital",
  "sensor_valid": true,
  "level": "HIGH"
}
```

Expected (after switching to ToF in web UI):

```json
{
  ...
  "sensor_type": "tof",
  "sensor_valid": true,
  "level": "ABOVE_HALF",
  "distance_mm": 110,
  "thresholds": { "low": 200, "half": 130, "high": 60 }
}
```

Now visually verify Task 11's live polling works: open the config page with ToF selected and confirm the "Current Reading" updates every 2 s.

- [ ] **Step 4: Commit**

```bash
git add OilTankMonitor/OilTankMonitor.ino
git commit -m "Extend /status JSON with sensor type, level, distance, and thresholds"
```

---

## Task 14: Bump `FW_VERSION` to 2.0.0

**Files:**
- Modify: `OilTankMonitor/OilTankMonitor.ino` — line 28

- [ ] **Step 1: Update version constant**

Find:

```cpp
const char* FW_VERSION = "1.1.0";
```

Replace with:

```cpp
const char* FW_VERSION = "2.0.0";
```

- [ ] **Step 2: Compile, flash, verify**

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 OilTankMonitor
arduino-cli upload --fqbn esp32:esp32:esp32:UploadSpeed=115200 --port /dev/ttyUSB0 OilTankMonitor
curl -s -b /tmp/c http://<device-ip>/status | jq .firmware
```

Expected: `"2.0.0"`

- [ ] **Step 3: Commit**

```bash
git add OilTankMonitor/OilTankMonitor.ino
git commit -m "Bump firmware version to 2.0.0"
```

---

## Task 15: Update README.md

**Files:**
- Modify: `README.md`

- [ ] **Step 1: Add VL53L0X to the Hardware BOM table**

Find the `## Hardware` table. After the row for the XKC-Y25-V Sensor, add:

```markdown
| VL53L0X ToF Module | Time-of-Flight distance sensor (for dry mechanical float gauges) | [~$3-5](https://www.adafruit.com/product/3317) |
```

- [ ] **Step 2: Add a "Choosing a Sensor" section after the Hardware table**

Insert immediately after the BOM table:

```markdown
### Choosing a Sensor

The firmware supports two sensor types, selectable at runtime from the web interface:

- **Digital threshold** (default): single-threshold digital sensor on GPIO4. Works with the XKC-Y25-V capacitive sensor (for sight gauges containing actual liquid), an IR break-beam pair (for dry mechanical floats with an opaque puck), a reed switch + magnet, a Hall-effect sensor, or any other HIGH/LOW signal source. Single notification: tank LOW / restored.
- **ToF distance** (VL53L0X): mounted on top of the sight gauge, measures distance to the puck and reports four states: LOW, BELOW_HALF, ABOVE_HALF, HIGH. Bidirectional notifications — get a heads-up when the tank passes half empty, plus refill confirmations at half and high marks.

Most installs with a working liquid sight gauge use the XKC-Y25-V (digital). Dry mechanical-float gauges typically use either an IR break-beam (digital) or the VL53L0X (ToF).
```

- [ ] **Step 3: Add ToF wiring subsection**

Find the `### Wiring` section. After the existing wiring table, add:

```markdown
#### ToF Sensor Wiring (VL53L0X)

| VL53L0X Pin | ESP32 Pin |
|-------------|-----------|
| VCC | 3V3 (Pin 1) |
| GND | GND (Pin 38) |
| SDA | GPIO21 (Pin 33) |
| SCL | GPIO22 (Pin 36) |

Mount the VL53L0X on top of the sight gauge looking down at the puck. The sensor reads distance in mm — smaller value means the puck is near the top (fuller tank), larger value means it has dropped (emptier tank).
```

- [ ] **Step 4: Add the VL53L0X library to the install commands**

Find the `### 2. Flash the ESP32` section, in the code block listing `arduino-cli lib install` commands. Add a third line:

```bash
arduino-cli lib install "Adafruit_VL53L0X"
```

- [ ] **Step 5: Update the Configure section with sensor selection step**

Find the `### 3. Configure via Web Interface` section. Insert a new numbered step between "Set a web interface password" and "Click Save & Restart":

```markdown
7. Under **Sensor Configuration**, choose your sensor type:
   - **Digital threshold**: leave defaults — works with XKC-Y25-V, IR break-beam, etc. on GPIO4
   - **ToF distance**: enter LOW, HALF, and HIGH thresholds in mm (defaults: 200/130/60). The page shows a live distance reading once the sensor is wired — use this to determine your install-specific values
```

(Renumber the existing "Click Save & Restart" from 7 to 8.)

- [ ] **Step 6: Update the API section with the new JSON example**

Find the `## API` section. Replace the JSON example with:

```json
{
  "oil_low": false,
  "wifi_connected": true,
  "ip": "192.168.1.100",
  "ssid": "YourNetwork",
  "uptime_sec": 3600,
  "firmware": "2.0.0",
  "sensor_type": "tof",
  "sensor_valid": true,
  "level": "ABOVE_HALF",
  "distance_mm": 110,
  "thresholds": { "low": 200, "half": 130, "high": 60 }
}
```

Add a brief note below the JSON:

```markdown
For digital sensor installs: `sensor_type: "digital"`, `level: "LOW"` or `"HIGH"`, and `distance_mm` / `thresholds` are omitted. The legacy `oil_low` field is preserved across both sensor types for backward compatibility.
```

- [ ] **Step 7: Verify README renders correctly**

```bash
# If you have a markdown previewer:
grip README.md  # or open in any markdown viewer

# Or just spot-check structure:
grep -E "^#{1,3} " README.md
```

Expected: BOM table contains VL53L0X row, "Choosing a Sensor" subsection appears, ToF wiring table renders, library install command present, sensor selection step in Configure section, updated JSON in API section.

- [ ] **Step 8: Commit**

```bash
git add README.md
git commit -m "Document interchangeable sensor support (VL53L0X, ToF wiring, sensor selection)"
```

---

## Task 16: Full integration test pass

**Files:** none (testing only)

Execute every test from the spec's testing plan. Document any deviations as follow-up tasks. Each test should pass before merging.

- [ ] **Test 1 — Digital regression:** Jumper GPIO4 LOW for 30 s, then HIGH. Expected: `⚠️ OIL TANK LOW` once → `✅ Oil tank level restored` once. Hourly reminder fires at 1-hour mark if held LOW.

- [ ] **Test 2 — ToF transitions:** Switch sensor type to ToF in web UI. Move target through full range slowly. Expected sequence (refill direction, HIGH=60, HALF=130, LOW=200):
  - target at 250 mm → boot says LOW
  - moves to 180 mm → `✅ Oil tank above low mark`
  - moves to 100 mm → `🔼 Tank is half refilled`
  - moves to 50 mm → `🔝 Tank at HIGH`

- [ ] **Test 3 — ToF hysteresis:** With LOW=200 and target oscillating between 198 mm and 202 mm: zero notifications.

- [ ] **Test 4 — ToF noise rejection:** Steady target at 200 mm; introduce one rapid 50 mm reading via fast object pass. Expected: no state change (median-of-3 filter rejects spike).

- [ ] **Test 5 — Sensor type switch via web UI:** Save with sensor_type=DIGITAL, verify operation. Switch to TOF, save → device restarts → operates as ToF. Switch back to DIGITAL → operates as digital. No spurious messages.

- [ ] **Test 6 — v1.1.0 → v2.0.0 OTA upgrade:** This requires keeping the old v1.1.0 binary handy. Steps:
  1. Flash v1.1.0 (use the pre-merge commit `3ba7fcf` if needed: `git checkout 3ba7fcf -- OilTankMonitor/OilTankMonitor.ino && arduino-cli compile && upload`)
  2. Configure WiFi, Telegram, etc. Verify v1.x behavior.
  3. Compile v2.0.0 to a binary: `arduino-cli compile --fqbn esp32:esp32:esp32 --output-dir /tmp/v2 OilTankMonitor`
  4. Use the OTA web UI (`/update`) to upload `/tmp/v2/OilTankMonitor.ino.bin`.
  5. After reboot: existing settings preserved, sensor_type defaults to DIGITAL, behavior identical to v1.x.

- [ ] **Test 7 — Threshold validation:**
  - With ToF selected, save with `low=100, half=130, high=60` → expect inline error
  - Save with `high=0` or `high=3000` → expect inline error
  - Save with valid values → expect normal restart

- [ ] **Test 8 — I2C disconnected mid-run:** With ToF active and running, unplug SDA/SCL wires. Expected: single `⚠️ Sensor fault` Telegram after ~25 s. Reconnect → Serial shows `ToF: readings recovered`, no further messages.

- [ ] **Test 9 — Boot at non-LOW level:** With ToF active and target at 110 mm (ABOVE_HALF), power-cycle the device. Expected: single `🛢️ Oil tank monitor is ONLINE. Sensor: ToF Level: ABOVE_HALF (110mm).` message. Zero transition messages.

- [ ] **Test 10 — 24-hour stability:** Let device run with ToF active for 24 h with a stable target. Expected: zero spurious alerts, no resets, free heap (visible in Serial periodic logs if added) stable.

- [ ] **Step 11: Commit a marker (optional)**

If all tests pass:

```bash
git commit --allow-empty -m "All v2.0.0 integration tests passed"
```

---

## Self-review

**Spec coverage:**

| Spec section | Implementing task(s) |
|---|---|
| Sensor abstraction (enum, struct, dispatch) | 1, 3, 4 |
| Filtering (digital debounce, ToF median-of-3) | 3, 5 |
| Hysteresis (±5 mm) | 6 |
| Hardware library (Adafruit_VL53L0X) | 0, 4 |
| Sensor-fault behavior (5-cycle alert, 3-cycle recovery) | 9 |
| LevelState + bucketing | 6 |
| Transition matrix (both directions) | 7 |
| Hourly reminder | 7 |
| Boot behavior (silent init, enhanced online message) | 7, 8 |
| Sensor-type switch via web UI | 10 |
| NVS additions (4 keys) | 2 |
| Web UI dropdown + conditional fields | 10 |
| Live distance polling | 11 |
| Validation (range + ordering) | 12 |
| `/status` JSON additions | 13 |
| Error handling (ToF init failure, I2C recovery, range check) | 9, 12 |
| Versioning (2.0.0) | 14 |
| Documentation updates | 15 |
| Manual integration tests | 16 |

All 18 spec sections accounted for.

**Type consistency check:**
- `SensorType` / `SENSOR_DIGITAL` / `SENSOR_TOF` — used identically across tasks 1, 3, 4, 7, 9, 10, 12, 13, 15.
- `SensorReading` fields `valid` / `digitalState` / `distanceMm` — consistent across tasks 1, 3, 5, 9, 13.
- `LevelState` enum values `LEVEL_LOW` / `LEVEL_BELOW_HALF` / `LEVEL_ABOVE_HALF` / `LEVEL_HIGH` / `LEVEL_UNKNOWN` — consistent across tasks 6, 7, 8, 13.
- `cfgSensorType` / `cfgTofLow` / `cfgTofHalf` / `cfgTofHigh` — consistent across tasks 2, 6, 7, 10, 12, 13.
- `currentState` global — consistent across tasks 6, 7, 8, 13.

**Placeholder scan:** No "TBD" / "TODO" / "implement later" / "similar to" / "appropriate error handling" appearing in any task body. Code is shown completely in every step that modifies code.

---
