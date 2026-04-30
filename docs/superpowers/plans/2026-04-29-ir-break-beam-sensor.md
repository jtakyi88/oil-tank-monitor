# IR Break-Beam Sensor Mode Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `SENSOR_IR_BREAK` as a third sensor type for sight-gauge puck monitoring, with inverted polarity, dedicated `OIL_OK`/`OIL_LOW` states, and tailored Telegram alert text.

**Architecture:** Single-file Arduino sketch. Add a new value to the `SensorType` enum, two new states to `LevelState`, and weave IR_BREAK branches into the eight functions that switch on sensor type. Reuse the existing 3-read digital debounce, hourly-reminder dispatcher, and NVS storage unchanged.

**Tech Stack:** Arduino C++ for ESP32, `arduino-cli` for compilation, `WebServer` for the config UI, `UniversalTelegramBot` for alerts.

**Spec:** `docs/superpowers/specs/2026-04-29-ir-break-beam-sensor-design.md`

**Project note:** This project has no automated test framework. Each task uses `arduino-cli compile` as the gating check after every change. Final runtime behavior is verified by the manual verification task at the end (matching the spec's testing plan).

---

### Task 1: Add `SENSOR_IR_BREAK` enum value and new level states

**Files:**
- Modify: `OilTankMonitor/OilTankMonitor.ino` (enums at lines 30 and 38, `levelStateName` at line 46)

- [ ] **Step 1: Add `SENSOR_IR_BREAK = 2` to `SensorType` enum**

Replace this block (around line 30):

```cpp
enum SensorType { SENSOR_DIGITAL = 0, SENSOR_TOF = 1 };
```

with:

```cpp
enum SensorType { SENSOR_DIGITAL = 0, SENSOR_TOF = 1, SENSOR_IR_BREAK = 2 };
```

- [ ] **Step 2: Add `LEVEL_OIL_OK` and `LEVEL_OIL_LOW` to `LevelState` enum**

Replace the block (around line 38):

```cpp
enum LevelState {
  LEVEL_LOW,           // distance > cfgTofLow  (or digital: no liquid)
  LEVEL_BELOW_HALF,    // cfgTofHalf < distance <= cfgTofLow
  LEVEL_ABOVE_HALF,    // cfgTofHigh < distance <= cfgTofHalf
  LEVEL_HIGH,          // distance <= cfgTofHigh (digital: liquid present)
  LEVEL_UNKNOWN        // no valid reading yet (boot)
};
```

with:

```cpp
enum LevelState {
  LEVEL_LOW,           // distance > cfgTofLow  (or digital: no liquid)
  LEVEL_BELOW_HALF,    // cfgTofHalf < distance <= cfgTofLow
  LEVEL_ABOVE_HALF,    // cfgTofHigh < distance <= cfgTofHalf
  LEVEL_HIGH,          // distance <= cfgTofHigh (digital: liquid present)
  LEVEL_OIL_OK,        // IR break-beam: beam clear (puck above low-oil mark)
  LEVEL_OIL_LOW,       // IR break-beam: beam broken (puck has reached the mark)
  LEVEL_UNKNOWN        // no valid reading yet (boot)
};
```

- [ ] **Step 3: Add cases to `levelStateName()`**

Replace the function (around line 46):

```cpp
const char* levelStateName(LevelState s) {
  switch (s) {
    case LEVEL_LOW:        return "LOW";
    case LEVEL_BELOW_HALF: return "BELOW_HALF";
    case LEVEL_ABOVE_HALF: return "ABOVE_HALF";
    case LEVEL_HIGH:       return "HIGH";
    case LEVEL_UNKNOWN:    return "UNKNOWN";
  }
  return "UNKNOWN";
}
```

with (preserving existing entries, adding two new cases):

```cpp
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
```

If the existing function uses `default:` instead of an explicit `LEVEL_UNKNOWN` case, leave the structure as-is and just add the two new cases above the default. The compiler will warn on missing cases without `default:`, which catches future enum additions.

- [ ] **Step 4: Compile to verify clean build**

Run from `/home/juliettango/oil-monitor/OilTankMonitor/`:

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 OilTankMonitor.ino
```

Expected: success, sketch uses ~1131KB (86%) of flash. No new warnings.

- [ ] **Step 5: Commit**

```bash
git add OilTankMonitor/OilTankMonitor.ino
git commit -m "Add SENSOR_IR_BREAK enum value and OIL_OK/OIL_LOW level states"
```

---

### Task 2: Wire `initSensor()` for IR_BREAK mode

**Files:**
- Modify: `OilTankMonitor/OilTankMonitor.ino` (`initSensor` around line 779)

- [ ] **Step 1: Add IR_BREAK branch to `initSensor()`**

Replace the function (around line 779):

```cpp
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
```

with:

```cpp
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
  // ToF path
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  if (!tofSensor.begin()) {
    Serial.println("Sensor: TOF init FAILED — check I2C wiring (SDA=21, SCL=22, VCC=3V3, GND=GND)");
    return false;
  }
  Serial.println("Sensor: TOF (VL53L0X) on I2C SDA=" + String(I2C_SDA_PIN) + " SCL=" + String(I2C_SCL_PIN));
  return true;
}
```

- [ ] **Step 2: Compile**

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 OilTankMonitor.ino
```

Expected: success.

- [ ] **Step 3: Commit**

```bash
git add OilTankMonitor/OilTankMonitor.ino
git commit -m "initSensor: add IR_BREAK branch with INPUT_PULLUP on SENSOR_PIN"
```

---

### Task 3: Read IR_BREAK in `readSensorRaw()` and extend debounce

**Files:**
- Modify: `OilTankMonitor/OilTankMonitor.ino` (`readSensorRaw` around line 795, `readSensor` around line 824)

- [ ] **Step 1: Add IR_BREAK branch to `readSensorRaw()`**

Replace the function (around line 795):

```cpp
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
```

with:

```cpp
SensorReading readSensorRaw() {
  SensorReading r = { false, false, 0 };
  if (cfgSensorType == SENSOR_DIGITAL || cfgSensorType == SENSOR_IR_BREAK) {
    r.digitalState = (digitalRead(SENSOR_PIN) == HIGH);
    r.valid = true;
    return r;
  }
  // ToF path
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
```

- [ ] **Step 2: Extend the digital debounce branch in `readSensor()` to cover IR_BREAK**

In `readSensor()` (around line 824), find this line:

```cpp
  if (cfgSensorType == SENSOR_DIGITAL) {
```

and replace with:

```cpp
  if (cfgSensorType == SENSOR_DIGITAL || cfgSensorType == SENSOR_IR_BREAK) {
```

The body of the branch is unchanged — both modes produce the same shape of reading (`digitalState` HIGH/LOW), so the existing debounce counter logic applies as-is.

- [ ] **Step 3: Compile**

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 OilTankMonitor.ino
```

Expected: success.

- [ ] **Step 4: Commit**

```bash
git add OilTankMonitor/OilTankMonitor.ino
git commit -m "readSensorRaw/readSensor: handle IR_BREAK via digital path"
```

---

### Task 4: Map IR_BREAK readings to OIL_OK/OIL_LOW in `bucketReading()`

**Files:**
- Modify: `OilTankMonitor/OilTankMonitor.ino` (`bucketReading` around line 865)

- [ ] **Step 1: Locate `bucketReading()` and confirm its current structure**

Read the function around line 865. Expected current shape (the digital branch, then the ToF logic):

```cpp
LevelState bucketReading(SensorReading r, LevelState fallback) {
  if (!r.valid) return fallback;
  if (cfgSensorType == SENSOR_DIGITAL) {
    return r.digitalState ? LEVEL_HIGH : LEVEL_LOW;
  }
  // ToF mapping...
}
```

(Exact ToF body may differ; do not modify it.)

- [ ] **Step 2: Add IR_BREAK branch above the ToF logic**

Insert the new branch immediately after the existing digital branch and before the ToF mapping:

```cpp
  if (cfgSensorType == SENSOR_IR_BREAK) {
    // Pin HIGH (with pullup) = beam detected = clear path = oil above low-oil mark.
    // Pin LOW = beam broken (puck has reached the mark) = oil low.
    return r.digitalState ? LEVEL_OIL_OK : LEVEL_OIL_LOW;
  }
```

- [ ] **Step 3: Compile**

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 OilTankMonitor.ino
```

Expected: success.

- [ ] **Step 4: Commit**

```bash
git add OilTankMonitor/OilTankMonitor.ino
git commit -m "bucketReading: map IR_BREAK pin state to OIL_OK/OIL_LOW"
```

---

### Task 5: Add IR_BREAK transition messages and extend hourly reminder

**Files:**
- Modify: `OilTankMonitor/OilTankMonitor.ino` (`fireTransitionMessage` around line 908; `loop()` around lines 1055, 1059, 1063)

- [ ] **Step 1: Add an IR_BREAK branch to `fireTransitionMessage()`**

In `fireTransitionMessage()` (around line 908), find the existing structure:

```cpp
void fireTransitionMessage(LevelState from, LevelState to) {
  if (cfgSensorType == SENSOR_DIGITAL) {
    // ... digital messages ...
    return;
  }
  // ToF: full multi-state transition matrix
  // ...
}
```

Insert a new branch immediately after the digital `return;` and before the ToF block:

```cpp
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
```

- [ ] **Step 2: Extend `lastAlertTime` tracking to cover OIL_LOW transitions**

In `loop()` (around line 1055), find:

```cpp
      if (newState == LEVEL_LOW) lastAlertTime = now;
```

Replace with:

```cpp
      if (newState == LEVEL_LOW || newState == LEVEL_OIL_LOW) lastAlertTime = now;
```

- [ ] **Step 3: Extend the legacy `oilIsLow` flag to cover OIL_LOW**

In `loop()` (around line 1059), find:

```cpp
    oilIsLow = (currentState == LEVEL_LOW);
```

Replace with:

```cpp
    oilIsLow = (currentState == LEVEL_LOW || currentState == LEVEL_OIL_LOW);
```

This preserves `/status` backward compatibility for any external client that polled the legacy `oil_low` boolean.

- [ ] **Step 4: Extend the hourly reminder to fire in IR mode**

In `loop()` (around line 1063), find:

```cpp
  // Hourly LOW reminder — same behavior as v1.x, now keyed on currentState
  if (currentState == LEVEL_LOW && (now - lastAlertTime >= ALERT_INTERVAL_MS)) {
    Serial.println("Sending hourly low-oil reminder.");
    sendTelegram("⚠️ REMINDER: Oil tank is still LOW. Please refill.");
    lastAlertTime = now;
  }
```

Replace with:

```cpp
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
```

- [ ] **Step 5: Compile**

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 OilTankMonitor.ino
```

Expected: success.

- [ ] **Step 6: Commit**

```bash
git add OilTankMonitor/OilTankMonitor.ino
git commit -m "Alert dispatcher: handle OIL_OK/OIL_LOW transitions and hourly reminder"
```

---

### Task 6: Add IR_BREAK option to web UI dropdown and hide ToF fields

**Files:**
- Modify: `OilTankMonitor/OilTankMonitor.ino` (`handleRoot` around line 297)

- [ ] **Step 1: Add the dropdown option**

In `handleRoot()` (around line 297), find the line that builds the dropdown:

```cpp
  page += "<option value='1'" + String(cfgSensorType == SENSOR_TOF ? " selected" : "") + ">ToF distance (VL53L0X)</option>";
```

Add immediately after it:

```cpp
  page += "<option value='2'" + String(cfgSensorType == SENSOR_IR_BREAK ? " selected" : "") + ">IR break-beam (sight gauge puck)</option>";
```

- [ ] **Step 2: Locate the ToF threshold field hiding logic**

Search for where ToF threshold inputs are conditionally rendered or styled hidden. Run:

```bash
grep -n "cfgTofLow\|cfgTofHalf\|cfgTofHigh\|tof.*display\|display.*tof" OilTankMonitor/OilTankMonitor.ino
```

Identify the JavaScript or server-side conditional that hides the ToF threshold block when `cfgSensorType == SENSOR_DIGITAL`.

- [ ] **Step 3: Extend the hide condition to also hide in IR_BREAK mode**

If the hide is server-side (a `if (cfgSensorType == SENSOR_DIGITAL)` guard around the `page += ...` lines for ToF fields), replace the guard with:

```cpp
if (cfgSensorType == SENSOR_DIGITAL || cfgSensorType == SENSOR_IR_BREAK) {
```

If the hide is client-side JavaScript that reads the dropdown value, extend the JS condition similarly. Example pattern — if the JS looks like:

```javascript
function toggleTofFields() {
  var v = document.getElementById('sensorType').value;
  document.getElementById('tofFields').style.display = (v === '1') ? 'block' : 'none';
}
```

then no change is needed — value `'2'` already evaluates to "hide" because it's not `'1'`. Verify this is the case by tracing the actual code; if the existing condition is `v !== '0'` or similar (showing for anything but digital), then change it to `v === '1'` so IR_BREAK also hides ToF fields.

- [ ] **Step 4: Compile**

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 OilTankMonitor.ino
```

Expected: success.

- [ ] **Step 5: Commit**

```bash
git add OilTankMonitor/OilTankMonitor.ino
git commit -m "Web UI: add IR break-beam dropdown option, hide ToF fields in IR mode"
```

---

### Task 7: Accept IR_BREAK type in `handleSave()`

**Files:**
- Modify: `OilTankMonitor/OilTankMonitor.ino` (`handleSave` around line 589)

- [ ] **Step 1: Update the sensor-type parsing**

In `handleSave()` (around line 589), find the line:

```cpp
    newSensorType = (t == 1) ? SENSOR_TOF : SENSOR_DIGITAL;
```

Replace with:

```cpp
    if (t == 1) {
      newSensorType = SENSOR_TOF;
    } else if (t == 2) {
      newSensorType = SENSOR_IR_BREAK;
    } else {
      newSensorType = SENSOR_DIGITAL;
    }
```

- [ ] **Step 2: Compile**

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 OilTankMonitor.ino
```

Expected: success.

- [ ] **Step 3: Commit**

```bash
git add OilTankMonitor/OilTankMonitor.ino
git commit -m "handleSave: accept sensor type 2 (IR_BREAK)"
```

---

### Task 8: Emit IR_BREAK info in `/status` JSON

**Files:**
- Modify: `OilTankMonitor/OilTankMonitor.ino` (`handleStatus` around line 650)

- [ ] **Step 1: Update the `sensor_type` field**

In `handleStatus()` (around line 650), find:

```cpp
  json += "\"sensor_type\":\"" + String(cfgSensorType == SENSOR_DIGITAL ? "digital" : "tof") + "\",";
```

Replace with:

```cpp
  const char* sensorTypeName;
  switch (cfgSensorType) {
    case SENSOR_TOF:      sensorTypeName = "tof"; break;
    case SENSOR_IR_BREAK: sensorTypeName = "ir_break"; break;
    default:              sensorTypeName = "digital"; break;
  }
  json += "\"sensor_type\":\"" + String(sensorTypeName) + "\",";
```

- [ ] **Step 2: Add `beam_state` field for IR_BREAK mode**

Find the closing brace of the JSON build (look for the line that ends the JSON object with `"}"`). Just before that closing line, insert:

```cpp
  if (cfgSensorType == SENSOR_IR_BREAK) {
    bool clear = (digitalRead(SENSOR_PIN) == HIGH);
    json += "\"beam_state\":\"" + String(clear ? "clear" : "broken") + "\",";
  }
```

Note: this performs a fresh pin read, not a debounced one — `/status` is expected to give an instantaneous snapshot. If there is a pre-computed cached reading available in the function scope, use that instead; the snapshot semantics are what matter.

If the JSON build appends a trailing comma after each field (as the snippet above does), the existing closing logic likely strips it before the `}`. If not (i.e., if the existing code carefully omits trailing commas), insert the `beam_state` field with no trailing comma and adjust placement accordingly.

- [ ] **Step 3: Compile**

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 OilTankMonitor.ino
```

Expected: success.

- [ ] **Step 4: Commit**

```bash
git add OilTankMonitor/OilTankMonitor.ino
git commit -m "handleStatus: emit ir_break sensor_type and beam_state field"
```

---

### Task 9: Update boot ONLINE Telegram message for IR_BREAK

**Files:**
- Modify: `OilTankMonitor/OilTankMonitor.ino` (`setup()` around line 972)

- [ ] **Step 1: Extend the ONLINE message construction**

In `setup()` (around line 972), find:

```cpp
      String msg = "🛢️ Oil tank monitor is ONLINE.\nSensor: ";
      msg += (cfgSensorType == SENSOR_DIGITAL ? "Digital" : "ToF");
      msg += "\nLevel: " + String(levelStateName(currentState));
      if (cfgSensorType == SENSOR_TOF && r.valid) {
        msg += " (" + String(r.distanceMm) + "mm)";
      }
      msg += "\nSettings: http://" + WiFi.localIP().toString();
      sendTelegram(msg);
```

Replace with:

```cpp
      String msg = "🛢️ Oil tank monitor is ONLINE.\nSensor: ";
      switch (cfgSensorType) {
        case SENSOR_TOF:      msg += "ToF"; break;
        case SENSOR_IR_BREAK: msg += "IR break-beam (sight gauge)"; break;
        default:              msg += "Digital"; break;
      }
      msg += "\nLevel: " + String(levelStateName(currentState));
      if (cfgSensorType == SENSOR_TOF && r.valid) {
        msg += " (" + String(r.distanceMm) + "mm)";
      }
      msg += "\nSettings: http://" + WiFi.localIP().toString();
      sendTelegram(msg);
```

- [ ] **Step 2: Verify the post-WiFi ToF init alert is IR-mode-safe**

Check `setup()` around line 963:

```cpp
      if (cfgSensorType == SENSOR_TOF && !tofSensor.begin()) {
        sendTelegram("⚠️ ToF init failed — check I2C wiring or switch sensor type via web UI.");
      }
```

Confirm it explicitly checks `cfgSensorType == SENSOR_TOF` so it does not fire spuriously in IR_BREAK mode. No change needed if it does — just verify.

- [ ] **Step 3: Compile**

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 OilTankMonitor.ino
```

Expected: success. Sketch size will be slightly larger but well under 90% of flash.

- [ ] **Step 4: Commit**

```bash
git add OilTankMonitor/OilTankMonitor.ino
git commit -m "setup ONLINE message: include IR break-beam sensor label"
```

---

### Task 10: Manual end-to-end verification

**Files:** None (runtime test on hardware).

This task verifies the runtime behavior described in the spec's testing plan. Do not skip.

- [ ] **Step 1: Flash the firmware**

From `/home/juliettango/oil-monitor/OilTankMonitor/`:

```bash
arduino-cli upload --fqbn esp32:esp32:esp32 -p /dev/ttyUSB0 OilTankMonitor.ino
```

Expected: upload succeeds, board hard-resets.

If `/dev/ttyUSB0` is busy, close any open serial monitor and retry.

- [ ] **Step 2: Verify the new dropdown option is visible**

Open the device's web UI (the IP shown in the boot ONLINE Telegram message). Confirm the sensor-type dropdown shows three options:
- Digital threshold (XKC-Y25-V, IR break-beam, reed switch, etc.)
- ToF distance (VL53L0X)
- **IR break-beam (sight gauge puck)** ← new

- [ ] **Step 3: Switch to IR_BREAK mode and save**

Select "IR break-beam (sight gauge puck)" in the dropdown. Verify the ToF threshold input fields are hidden. Save.

- [ ] **Step 4: Verify `/status` reports IR mode correctly**

Open `http://<ip>/status`. Expected JSON contains:

```json
"sensor_type":"ir_break",
"beam_state":"clear",
"level":"OIL_OK",
```

(Assuming the receiver is wired and beam is clear. If beam is broken at this moment, expect `"broken"` and `"OIL_LOW"`.)

- [ ] **Step 5: Verify the boot ONLINE Telegram message**

Power-cycle the board. Within ~30 seconds expect a Telegram message of the form:

```
🛢️ Oil tank monitor is ONLINE.
Sensor: IR break-beam (sight gauge)
Level: OIL_OK
Settings: http://<ip>
```

(Or `Level: OIL_LOW` if the beam is broken at boot.)

- [ ] **Step 6: Trigger the low-oil alert**

Block the IR beam (place an object between emitter and receiver, or descend the sight-gauge puck into the beam). Within ~3 seconds expect:
- `/status` shows `"beam_state":"broken"`, `"level":"OIL_LOW"`
- Telegram message: `⚠️ Low oil — sight-gauge puck has reached the low-oil mark. Please refill.\nSettings: http://<ip>`

- [ ] **Step 7: Verify recovery**

Unblock the beam. Within ~3 seconds expect:
- `/status` flips back to `"beam_state":"clear"`, `"level":"OIL_OK"`
- Telegram message: `✅ Oil level restored — puck is above the low-oil mark.`

- [ ] **Step 8: (Optional, time-permitting) Verify hourly reminder cadence**

Hold the beam blocked for >60 minutes. Expect a second Telegram message:
`⚠️ REMINDER: Sight-gauge puck still at the low-oil mark. Please refill.`

If you can't wait 60 minutes, temporarily lower `ALERT_INTERVAL_MS` to something like `60000UL` (1 minute) for the test, verify, then restore to `3600000UL` and re-flash.

- [ ] **Step 9: Verify backward compat — switch back to Digital mode**

In the web UI, switch sensor type back to "Digital threshold". Save. Confirm the digital sensor mode still works as before (the existing XKC sensor or a digital level sensor produces correct LEVEL_HIGH/LEVEL_LOW transitions).

- [ ] **Step 10: Final commit (only if any debug tweaks were made during verification)**

If any code changes were made during verification (e.g., temporarily tweaking `ALERT_INTERVAL_MS`), revert them and commit a "no-op" verification note only if needed. Otherwise, no commit — verification is observational.

---

## Self-Review

**Spec coverage check** — every requirement in the spec maps to a task:
- New `SENSOR_IR_BREAK` enum value → Task 1
- New `LEVEL_OIL_OK`/`LEVEL_OIL_LOW` states → Task 1
- `levelStateName` cases → Task 1
- Pin mode `INPUT_PULLUP` on GPIO 4 in IR mode → Task 2
- Polarity mapping (HIGH=clear=OK, LOW=broken=LOW) → Task 4
- Reuse 3-read digital debounce → Task 3
- NVS storage forward compatibility (no migration) → handled implicitly; new value coexists with existing 0/1
- Web UI dropdown third option → Task 6
- Hide ToF fields in IR mode → Task 6
- `handleSave` accepts `t == 2` → Task 7
- `/status` JSON `sensor_type:"ir_break"` and `beam_state` field → Task 8
- Telegram ONLINE message variant → Task 9
- Telegram low-oil alert + recovery + hourly reminder → Task 5
- `oilIsLow` legacy field covers `LEVEL_OIL_LOW` → Task 5
- Manual verification of all behavior → Task 10

All spec sections covered.

**Placeholder scan** — no TBDs, no "implement later", no missing code blocks. Task 6 Step 3 contains conditional guidance ("if existing code is X, do Y; if Z, do W") but each branch is fully specified. Task 8 Step 2 has similar conditional guidance for the JSON closing-comma handling, also fully specified per branch.

**Type consistency** — `SENSOR_IR_BREAK`, `LEVEL_OIL_OK`, `LEVEL_OIL_LOW`, `digitalState`, `valid`, `cfgSensorType`, `currentState`, `lastAlertTime`, `oilIsLow`, `ALERT_INTERVAL_MS`, `SENSOR_PIN` — all names used consistently across tasks.

Plan is complete.
