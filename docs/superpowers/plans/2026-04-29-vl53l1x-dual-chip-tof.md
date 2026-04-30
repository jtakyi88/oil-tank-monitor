# Dual-Chip ToF Support (VL53L0X + VL53L1X) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add VL53L1X support to ToF mode via runtime auto-detection — single dropdown option, firmware probes I2C and uses whichever chip responds.

**Architecture:** Keep existing `Adafruit_VL53L0X` library; add `Adafruit_VL53L1X` library. Replace single global `tofSensor` with per-chip globals + an `activeTofChip` enum. New `initTof()` helper does VL53L1X-first detection with VL53L0X fallback. `readSensorRaw()` ToF branch dispatches on `activeTofChip`. Bump `TOF_MAX_MM` from 2000 → 4000.

**Tech Stack:** Arduino C++ for ESP32, `arduino-cli`, `Adafruit_VL53L0X` (already installed v1.2.5), `Adafruit_VL53L1X` (already installed v3.1.2).

**Spec:** `docs/superpowers/specs/2026-04-29-vl53l1x-dual-chip-tof-design.md`

**Project note:** This project has no automated test framework. Each task uses `arduino-cli compile` from `/home/juliettango/oil-monitor/OilTankMonitor/` as the gating check. Final runtime behavior is verified by the manual verification task at the end.

**Library API pins** (do not deviate — these were verified against the installed library headers at plan time):

- `Adafruit_VL53L1X::Adafruit_VL53L1X(int8_t shutdown_pin = -1, int8_t irq_pin = -1)` — default both to -1.
- `bool begin(uint8_t i2c_addr = 0x29, TwoWire *theWire = &Wire, bool debug = false)` — returns true on success.
- `tofL1.VL53L1X_SetDistanceMode(2)` — inherited from `VL53L1X` parent class. `2` = Long mode (4 m). Returns `VL53L1X_ERROR` (0 = OK).
- `tofL1.setTimingBudget(uint16_t ms)` — Adafruit subclass exposes this directly.
- `tofL1.startRanging()` — call once after init/config.
- `tofL1.dataReady()` — non-blocking poll, returns `bool`.
- `tofL1.distance()` — returns `int16_t` mm; **-1 on error**.
- `tofL1.VL53L1X_GetRangeStatus(uint8_t *out)` — inherited; **0 = valid range**, non-zero = various error codes.
- `tofL1.clearInterrupt()` — must be called after each successful `distance()` to arm next measurement.

---

### Task 1: Measure flash budget impact

**Files:** `OilTankMonitor/OilTankMonitor.ino` (temporary include, will be reverted)

**Goal:** Confirm adding the VL53L1X library doesn't push the firmware over the partition limit before doing real work. Spec flagged this as the single biggest risk.

- [ ] **Step 1: Record baseline flash usage**

```bash
cd /home/juliettango/oil-monitor/OilTankMonitor
arduino-cli compile --fqbn esp32:esp32:esp32 OilTankMonitor.ino 2>&1 | grep "Sketch uses"
```

Note the byte count and percentage. Expected: ~1,132,896 bytes (86%).

- [ ] **Step 2: Add a test include for the VL53L1X library**

In `OilTankMonitor/OilTankMonitor.ino`, immediately after the existing line:

```cpp
#include <Adafruit_VL53L0X.h>
```

add a new line:

```cpp
#include <Adafruit_VL53L1X.h>
Adafruit_VL53L1X _flashBudgetProbe;  // temporary: forces linker to pull in the library
```

- [ ] **Step 3: Compile and record new flash usage**

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 OilTankMonitor.ino 2>&1 | grep "Sketch uses"
```

Compare to Step 1. Compute delta in KB and percentage.

- [ ] **Step 4: Decision gate**

- If new flash usage is **< 95%** (under ~1,245,000 bytes): proceed. Continue to Step 5.
- If new flash usage is **95-99%**: still proceed but flag for the user — there's no headroom for future features.
- If new flash usage is **≥ 100%** (compile fails with "section overflow"): STOP and escalate. The user will need to choose between (a) switching to a larger partition scheme via `--build-property "build.partitions=min_spiffs"`, or (b) dropping the VL53L0X library entirely. Do not proceed without user input.

- [ ] **Step 5: Revert the test include**

Remove the two lines added in Step 2 (`#include <Adafruit_VL53L1X.h>` and `Adafruit_VL53L1X _flashBudgetProbe;`).

Verify revert with:
```bash
arduino-cli compile --fqbn esp32:esp32:esp32 OilTankMonitor.ino 2>&1 | grep "Sketch uses"
```
Expected: byte count returns to the Step 1 baseline.

- [ ] **Step 6: No commit**

This task produces no code change. Just record the flash delta in your task report so the user knows the budget impact before Task 2 lands the include for real.

---

### Task 2: Add `Adafruit_VL53L1X` include + `TofChip` enum + per-chip globals

**Files:** Modify `OilTankMonitor/OilTankMonitor.ino`

- [ ] **Step 1: Add the new include**

In `OilTankMonitor/OilTankMonitor.ino`, immediately after this existing line:

```cpp
#include <Adafruit_VL53L0X.h>
```

add:

```cpp
#include <Adafruit_VL53L1X.h>
```

- [ ] **Step 2: Add the `TofChip` enum near the existing `SensorType` enum**

The existing `SensorType` enum is around line 30. Immediately after it, add:

```cpp
enum TofChip { TOF_NONE, TOF_VL53L0X, TOF_VL53L1X };
```

- [ ] **Step 3: Replace the single ToF global with per-chip globals**

Find the existing line (currently around line 780):

```cpp
Adafruit_VL53L0X tofSensor;     // unused until Task 4 wires the ToF path
```

(Note: the trailing comment text from earlier task work may differ slightly. Match on `Adafruit_VL53L0X tofSensor;`.)

Replace with:

```cpp
Adafruit_VL53L0X tofL0;
Adafruit_VL53L1X tofL1;
TofChip activeTofChip = TOF_NONE;
```

- [ ] **Step 4: Update all existing references from `tofSensor` to `tofL0`**

Do not rewrite the ToF read logic yet — that's Task 4. For now, the goal is just to keep the file compiling under the rename. Run:

```bash
grep -n "tofSensor" /home/juliettango/oil-monitor/OilTankMonitor/OilTankMonitor.ino
```

For each occurrence, change `tofSensor` to `tofL0`. Expected sites (line numbers approximate):
- `initSensor()` ToF branch: `if (!tofSensor.begin())` → `if (!tofL0.begin())`
- `readSensorRaw()` ToF branch: `tofSensor.rangingTest(...)` → `tofL0.rangingTest(...)`
- `setup()` post-WiFi check: `if (cfgSensorType == SENSOR_TOF && !tofSensor.begin())` → `if (cfgSensorType == SENSOR_TOF && !tofL0.begin())`
- `loop()` bus-recovery: `tofSensor.begin();` → `tofL0.begin();`

After the rename, re-grep to confirm zero remaining occurrences:

```bash
grep -n "tofSensor" /home/juliettango/oil-monitor/OilTankMonitor/OilTankMonitor.ino
```

Expected: no output.

- [ ] **Step 5: Bump `TOF_MAX_MM`**

Find the line:

```cpp
const int TOF_MAX_MM = 2000;           // VL53L0X spec upper bound (mm we accept)
```

Replace with:

```cpp
const int TOF_MAX_MM = 4000;           // VL53L1X long-mode upper bound (VL53L0X readings cap naturally at ~2000)
```

- [ ] **Step 6: Compile**

```bash
cd /home/juliettango/oil-monitor/OilTankMonitor
arduino-cli compile --fqbn esp32:esp32:esp32 OilTankMonitor.ino
```

Expected: success. Flash usage will jump (now we're really linking the second library — should match Task 1's measurement).

- [ ] **Step 7: Commit**

```bash
cd /home/juliettango/oil-monitor
git add OilTankMonitor/OilTankMonitor.ino
git commit -m "Add VL53L1X include, per-chip ToF globals, bump TOF_MAX_MM to 4000"
```

---

### Task 3: Add `initTof()` detection helper

**Files:** Modify `OilTankMonitor/OilTankMonitor.ino`

- [ ] **Step 1: Add the new function above `initSensor()`**

Locate `initSensor()` (currently around line 783 after Task 2's edits). Immediately ABOVE it, insert this new function:

```cpp
// Detect which ToF chip is on the I2C bus. Tries VL53L1X first (more capable,
// matches the user's TOF400C breakout), then falls back to VL53L0X.
// On success, configures the chip and sets activeTofChip. Returns true on success.
bool initTof() {
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  // VL53L1X first
  if (tofL1.begin(0x29, &Wire)) {
    tofL1.VL53L1X_SetDistanceMode(2);   // 2 = Long mode (~4 m range)
    tofL1.setTimingBudget(50);          // 50 ms per measurement
    tofL1.startRanging();
    activeTofChip = TOF_VL53L1X;
    Serial.println("Sensor: TOF (VL53L1X, long mode) on I2C SDA="
                   + String(I2C_SDA_PIN) + " SCL=" + String(I2C_SCL_PIN));
    return true;
  }
  // Fallback to VL53L0X
  if (tofL0.begin()) {
    activeTofChip = TOF_VL53L0X;
    Serial.println("Sensor: TOF (VL53L0X) on I2C SDA="
                   + String(I2C_SDA_PIN) + " SCL=" + String(I2C_SCL_PIN));
    return true;
  }
  activeTofChip = TOF_NONE;
  return false;
}
```

- [ ] **Step 2: Compile (function exists but is unused — confirm no syntax error)**

```bash
cd /home/juliettango/oil-monitor/OilTankMonitor
arduino-cli compile --fqbn esp32:esp32:esp32 OilTankMonitor.ino
```

Expected: success. May produce a `-Wunused-function` warning since nothing calls `initTof()` yet — that's fine, Task 4 wires it up.

- [ ] **Step 3: Commit**

```bash
cd /home/juliettango/oil-monitor
git add OilTankMonitor/OilTankMonitor.ino
git commit -m "Add initTof() helper with VL53L1X-first auto-detection"
```

---

### Task 4: Wire `initTof()` into `initSensor()`, setup retry, and bus-recovery

**Files:** Modify `OilTankMonitor/OilTankMonitor.ino`

- [ ] **Step 1: Replace the ToF branch of `initSensor()`**

Locate `initSensor()` and find its ToF branch. After Task 2's rename, it currently looks like (approximately):

```cpp
  // ToF path
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  if (!tofL0.begin()) {
    Serial.println("Sensor: TOF init FAILED — check I2C wiring (SDA=21, SCL=22, VCC=3V3, GND=GND)");
    return false;
  }
  Serial.println("Sensor: TOF (VL53L0X) on I2C SDA=" + String(I2C_SDA_PIN) + " SCL=" + String(I2C_SCL_PIN));
  return true;
```

Replace that entire block with:

```cpp
  // ToF path — auto-detect VL53L1X or VL53L0X
  if (!initTof()) {
    Serial.println("Sensor: TOF init FAILED — neither VL53L1X nor VL53L0X responded on I2C (SDA=21, SCL=22, VCC=3V3, GND=GND)");
    return false;
  }
  return true;
```

(The success-case `Serial.println` lives inside `initTof()` itself, so it's removed from `initSensor()`.)

- [ ] **Step 2: Update the post-WiFi retry in `setup()`**

Locate (currently around line 963 in pre-edit numbering, may have shifted):

```cpp
      if (cfgSensorType == SENSOR_TOF && !tofL0.begin()) {
        sendTelegram("⚠️ ToF init failed — check I2C wiring or switch sensor type via web UI.");
      }
```

Replace with:

```cpp
      if (cfgSensorType == SENSOR_TOF && !initTof()) {
        sendTelegram("⚠️ ToF init failed — neither VL53L0X nor VL53L1X responded on I2C. Check wiring or switch sensor type via web UI.");
      }
```

- [ ] **Step 3: Update the bus-recovery path in `loop()`**

Locate (currently around line 1028 in pre-edit numbering):

```cpp
          Serial.println("ToF: attempting I2C bus recovery");
          Wire.end();
          Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
          tofL0.begin();
```

Replace with:

```cpp
          Serial.println("ToF: attempting I2C bus recovery");
          Wire.end();
          initTof();   // re-runs detection and reconfigures whichever chip responds
```

(`initTof()` calls `Wire.begin()` itself, so the explicit `Wire.begin()` line is removed.)

- [ ] **Step 4: Compile**

```bash
cd /home/juliettango/oil-monitor/OilTankMonitor
arduino-cli compile --fqbn esp32:esp32:esp32 OilTankMonitor.ino
```

Expected: success. The `-Wunused-function` warning from Task 3 should now be gone.

- [ ] **Step 5: Commit**

```bash
cd /home/juliettango/oil-monitor
git add OilTankMonitor/OilTankMonitor.ino
git commit -m "Wire initTof() into initSensor, setup retry, and bus-recovery path"
```

---

### Task 5: Dispatch ToF reads on `activeTofChip` in `readSensorRaw()`

**Files:** Modify `OilTankMonitor/OilTankMonitor.ino`

- [ ] **Step 1: Replace the ToF branch of `readSensorRaw()`**

Locate `readSensorRaw()` and find its ToF branch. After Task 2's rename, it currently looks like (approximately):

```cpp
  // ToF path
  VL53L0X_RangingMeasurementData_t data;
  tofL0.rangingTest(&data, false);
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

Replace that entire block with:

```cpp
  // ToF path — dispatch on detected chip
  if (activeTofChip == TOF_VL53L0X) {
    VL53L0X_RangingMeasurementData_t data;
    tofL0.rangingTest(&data, false);
    if (data.RangeStatus == 4) return r;          // out of range / no signal
    uint16_t mm = data.RangeMilliMeter;
    if (mm < TOF_MIN_MM || mm > TOF_MAX_MM) return r;
    r.distanceMm = mm;
    r.valid = true;
    return r;
  }
  if (activeTofChip == TOF_VL53L1X) {
    if (!tofL1.dataReady()) return r;             // measurement not ready; debounce will skip
    int16_t mm = tofL1.distance();                // signed; -1 on error
    uint8_t status = 0xFF;
    tofL1.VL53L1X_GetRangeStatus(&status);
    tofL1.clearInterrupt();                       // arm next measurement
    if (status != 0 || mm < 0) return r;
    if ((uint16_t)mm < TOF_MIN_MM || (uint16_t)mm > TOF_MAX_MM) return r;
    r.distanceMm = (uint16_t)mm;
    r.valid = true;
    return r;
  }
  return r;  // TOF_NONE — should not occur in normal flow
```

- [ ] **Step 2: Compile**

```bash
cd /home/juliettango/oil-monitor/OilTankMonitor
arduino-cli compile --fqbn esp32:esp32:esp32 OilTankMonitor.ino
```

Expected: success.

- [ ] **Step 3: Commit**

```bash
cd /home/juliettango/oil-monitor
git add OilTankMonitor/OilTankMonitor.ino
git commit -m "readSensorRaw: dispatch ToF reads on activeTofChip (VL53L0X or VL53L1X)"
```

---

### Task 6: Emit `tof_chip` in `/status` JSON

**Files:** Modify `OilTankMonitor/OilTankMonitor.ino`

- [ ] **Step 1: Locate `handleStatus()`**

`handleStatus()` is around line 645. Read it to find where the IR_BREAK `beam_state` field is conditionally appended (added by the previous IR break-beam plan). The new `tof_chip` field follows the same pattern.

- [ ] **Step 2: Add the `tof_chip` field, gated on SENSOR_TOF**

Find the existing IR_BREAK conditional block in `handleStatus()`, which looks like (approximately):

```cpp
  if (cfgSensorType == SENSOR_IR_BREAK) {
    // Fresh pin read — beam_state is an instantaneous snapshot, not the debounced level.
    bool clear = (digitalRead(SENSOR_PIN) == HIGH);
    json += "\"beam_state\":\"" + String(clear ? "clear" : "broken") + "\",";
  }
```

Immediately AFTER that block (still inside `handleStatus()`, before the closing `}`), insert:

```cpp
  if (cfgSensorType == SENSOR_TOF) {
    const char* tofChipName;
    switch (activeTofChip) {
      case TOF_VL53L0X: tofChipName = "vl53l0x"; break;
      case TOF_VL53L1X: tofChipName = "vl53l1x"; break;
      default:          tofChipName = "none"; break;
    }
    json += "\"tof_chip\":\"" + String(tofChipName) + "\",";
  }
```

- [ ] **Step 3: Compile**

```bash
cd /home/juliettango/oil-monitor/OilTankMonitor
arduino-cli compile --fqbn esp32:esp32:esp32 OilTankMonitor.ino
```

Expected: success.

- [ ] **Step 4: Commit**

```bash
cd /home/juliettango/oil-monitor
git add OilTankMonitor/OilTankMonitor.ino
git commit -m "handleStatus: emit tof_chip field for ToF mode"
```

---

### Task 7: Update web UI dropdown label

**Files:** Modify `OilTankMonitor/OilTankMonitor.ino`

- [ ] **Step 1: Update the ToF dropdown option label**

In `handleRoot()` (around line 297-298), find:

```cpp
  page += "<option value='1'" + String(cfgSensorType == SENSOR_TOF ? " selected" : "") + ">ToF distance (VL53L0X)</option>";
```

Replace with:

```cpp
  page += "<option value='1'" + String(cfgSensorType == SENSOR_TOF ? " selected" : "") + ">ToF distance (VL53L0X / VL53L1X auto-detect)</option>";
```

- [ ] **Step 2: Compile**

```bash
cd /home/juliettango/oil-monitor/OilTankMonitor
arduino-cli compile --fqbn esp32:esp32:esp32 OilTankMonitor.ino
```

Expected: success.

- [ ] **Step 3: Commit**

```bash
cd /home/juliettango/oil-monitor
git add OilTankMonitor/OilTankMonitor.ino
git commit -m "Web UI: label ToF dropdown as auto-detect VL53L0X / VL53L1X"
```

---

### Task 8: Update README and docs

**Files:** Modify `README.md` and any file under `docs/` that mentions VL53L0X exclusively

- [ ] **Step 1: Find existing VL53L0X mentions in docs**

```bash
cd /home/juliettango/oil-monitor
grep -rn "VL53L0X\|VL53L1X" README.md docs/ 2>&1 | grep -v "/specs/\|/plans/"
```

(Excluding spec and plan files — those are historical and shouldn't be edited.)

For each match outside `docs/superpowers/specs/` and `docs/superpowers/plans/`, decide whether the line should mention both chips or only one.

- [ ] **Step 2: Update each match**

For sentences that name VL53L0X as the supported ToF chip, change them to "VL53L0X or VL53L1X (auto-detected)" or equivalent — preserve surrounding sentence structure.

For wiring tables, no change needed — the wiring is the same for both chips (3V3, GND, SDA→21, SCL→22).

If `docs/` references the ~2 m range as a sensor limit, add a note that VL53L1X can reach ~4 m in long mode, and that `TOF_MAX_MM` accepts up to 4000 mm.

- [ ] **Step 3: Verify no remaining "VL53L0X only" claims**

Re-run the grep from Step 1 and read each match in context. Confirm none of them still imply VL53L0X is the only supported chip.

- [ ] **Step 4: Commit**

```bash
cd /home/juliettango/oil-monitor
git add README.md docs/
git commit -m "Document VL53L1X support and auto-detection in README and docs"
```

(If only README.md was edited and `docs/` had no matches, omit `docs/` from the `git add`.)

---

### Task 9: Manual end-to-end verification

**Files:** None (runtime test on hardware).

- [ ] **Step 1: Final compile**

```bash
cd /home/juliettango/oil-monitor/OilTankMonitor
arduino-cli compile --fqbn esp32:esp32:esp32 OilTankMonitor.ino
```

Confirm success and record final flash usage.

- [ ] **Step 2: Flash to ESP32**

```bash
arduino-cli upload --fqbn esp32:esp32:esp32 -p /dev/ttyUSB0 OilTankMonitor.ino
```

Expected: upload succeeds. If `/dev/ttyUSB0` is busy, close any open serial monitor and retry.

- [ ] **Step 3: Wire the VL53L1X (TOF400C breakout)**

Connect to ESP32:
- VL53L1X VIN → ESP32 3V3
- VL53L1X GND → ESP32 GND
- VL53L1X SDA → ESP32 GPIO 21
- VL53L1X SCL → ESP32 GPIO 22

- [ ] **Step 4: Switch sensor type to ToF in web UI**

Open the device's web UI (IP shown in boot ONLINE Telegram message). Select "ToF distance (VL53L0X / VL53L1X auto-detect)" in the dropdown. Save.

- [ ] **Step 5: Verify chip detection**

Open `<ip>/status`. Expected JSON contains:

```json
"sensor_type":"tof",
"tof_chip":"vl53l1x",
"distance_mm":<some non-zero value>,
```

If `tof_chip` is `"none"`, the sensor isn't responding — recheck wiring. If it's `"vl53l0x"`, the user has actually wired a VL53L0X (which would also be a valid result, just unexpected for this hardware).

If you have access to the serial console, the boot log should also include:
```
Sensor: TOF (VL53L1X, long mode) on I2C SDA=21 SCL=22
```

- [ ] **Step 6: Verify distance readings track physical distance**

Move a hand or object in front of the sensor at varying distances. `distance_mm` in `/status` should change accordingly. At ~10 cm you should read ~100; at 1 m you should read ~1000; at 2-3 m you should read approximately the actual distance.

- [ ] **Step 7: Verify level state transitions**

If the configured ToF thresholds (cfgTofLow, cfgTofHalf, cfgTofHigh) are within reach with hand-waving, verify the `level` field transitions through LOW / BELOW_HALF / ABOVE_HALF / HIGH as you change distance. Telegram alerts should fire on transitions per the existing dispatcher.

- [ ] **Step 8: (Optional) Trigger fault and recovery**

Disconnect the VL53L1X SDA wire. Within ~3 read cycles (~9 seconds with 50 ms timing budget + 50 ms inter-read delay), `/status` should show invalid readings and the fault detector should attempt I2C bus recovery. Telegram should NOT spam — the fault detector has its own debounce.

Reconnect SDA. Within a few seconds, `/status` should report valid readings again.

- [ ] **Step 9: Verify backward compatibility — switch to other sensor types**

In the web UI, switch sensor type to "Digital threshold". Save. Confirm the digital sensor mode still works (LEVEL_HIGH / LEVEL_LOW transitions).

Switch to "IR break-beam (sight gauge puck)". Save. Confirm IR mode still works (OIL_OK / OIL_LOW transitions).

Switch back to "ToF distance". Save. Confirm ToF mode still detects and reads correctly.

- [ ] **Step 10: No commit unless tests required code tweaks**

If verification was clean, no commit needed — verification is observational. If you had to tweak anything (e.g., timing budget didn't suit your environment), commit the tweak with a descriptive message.

---

## Self-Review

**Spec coverage check** — every requirement in the spec maps to a task:

- New `Adafruit_VL53L1X` include → Task 2
- `TofChip` enum → Task 2
- Per-chip globals (`tofL0`, `tofL1`, `activeTofChip`) → Task 2
- Removal of single `tofSensor` global → Task 2 (rename + replace)
- `TOF_MAX_MM` bumped 2000 → 4000 → Task 2
- `initTof()` detection helper with VL53L1X-first → Task 3
- Long mode + 50 ms timing budget for VL53L1X → Task 3
- `startRanging()` after VL53L1X init → Task 3
- `initSensor()` ToF branch calls `initTof()` → Task 4
- Setup post-WiFi retry uses `initTof()` → Task 4
- `loop()` bus-recovery uses `initTof()` → Task 4
- Updated ToF init failure Telegram text → Task 4
- `readSensorRaw()` dispatches on `activeTofChip` → Task 5
- VL53L1X `dataReady()` / `distance()` / `clearInterrupt()` / `VL53L1X_GetRangeStatus()` API used → Task 5
- `/status` JSON `tof_chip` field → Task 6
- Web UI dropdown label updated → Task 7
- README and docs updated → Task 8
- Flash budget measurement before any real work → Task 1
- Manual end-to-end verification including chip detection, fault recovery, and backward compat → Task 9

All spec sections covered.

**Placeholder scan** — no TBDs, no "implement later", no missing code blocks. Library API symbol names are pinned in the plan header (verified against installed library headers at plan time). Task 8 (docs update) intentionally describes the search/edit pattern rather than enumerating every line, because the exact set of doc lines depends on what `grep` finds — but each step is fully actionable.

**Type consistency** — `TofChip` enum (`TOF_NONE`/`TOF_VL53L0X`/`TOF_VL53L1X`), `activeTofChip`, `tofL0`, `tofL1`, `initTof()`, `TOF_MAX_MM`, `TOF_MIN_MM`, `VL53L1X_SetDistanceMode`, `VL53L1X_GetRangeStatus`, `setTimingBudget`, `startRanging`, `dataReady`, `distance`, `clearInterrupt` — all names used consistently across tasks.

Plan is complete.
