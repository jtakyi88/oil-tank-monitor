# Drop VL53L0X Library Support Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove all VL53L0X support from the firmware, freeing 5–15 KB flash and simplifying the ToF code path to a single chip.

**Architecture:** Pure deletion. Drop the `Adafruit_VL53L0X` include, global, enum value, detection-fallback branch, read-dispatch branch, status-JSON case, and all VL53L0X-mentioning user-facing strings. The `TofChip` enum stays (forward extensibility) but contains only `TOF_NONE` and `TOF_VL53L1X`. NVS-stored config requires no migration.

**Tech Stack:** Arduino C++ for ESP32, `arduino-cli`, `Adafruit_VL53L1X` (already installed v3.1.2). Removes dependency on `Adafruit_VL53L0X`.

**Spec:** `docs/superpowers/specs/2026-04-30-drop-vl53l0x-library-design.md`

**Project note:** No automated test framework. `arduino-cli compile` from `/home/juliettango/oil-monitor/OilTankMonitor/` is the gating check. End-to-end behavior is verified by the manual verification task at the end (the device should still detect VL53L1X and behave identically; failure paths get cleaner text).

---

### Task 1: Remove VL53L0X include, global, and enum value

**Files:**
- Modify: `OilTankMonitor/OilTankMonitor.ino`

- [ ] **Step 1: Remove the VL53L0X include**

In `OilTankMonitor/OilTankMonitor.ino`, find:

```cpp
#include <Adafruit_VL53L0X.h>
```

Delete that entire line. (`#include <Adafruit_VL53L1X.h>` immediately below it stays.)

- [ ] **Step 2: Remove the VL53L0X global**

Find the per-chip globals block (around line 804-806):

```cpp
Adafruit_VL53L0X tofL0x;
Adafruit_VL53L1X tofL1x;       // Constructed but not initialized until initTof() (Task 3) probes the bus.
TofChip activeTofChip = TOF_NONE;
```

Delete the `Adafruit_VL53L0X tofL0x;` line. Keep the other two lines.

- [ ] **Step 3: Trim the TofChip enum**

Find the enum (around line 32):

```cpp
enum TofChip { TOF_NONE, TOF_VL53L0X, TOF_VL53L1X };
```

Replace with:

```cpp
enum TofChip { TOF_NONE, TOF_VL53L1X };
```

- [ ] **Step 4: Compile (expected: errors — code still references removed symbols)**

```bash
cd /home/juliettango/oil-monitor/OilTankMonitor
arduino-cli compile --fqbn esp32:esp32:esp32 OilTankMonitor.ino
```

Expected: **compile failures** in `initTof()`, `readSensorRaw()`, and `handleStatus()` referring to undefined `tofL0x` and `TOF_VL53L0X`. This is intentional — Tasks 2-4 fix these sites.

Record the specific error lines for cross-reference. Do not commit yet — half-applied removal would leave the codebase non-compiling. Tasks 2-4 will fix compile, then Task 5 commits the whole sub-project as one coherent change.

---

### Task 2: Simplify `initTof()` — remove L0X fallback branch

**Files:**
- Modify: `OilTankMonitor/OilTankMonitor.ino`

- [ ] **Step 1: Replace the `initTof()` function body**

Find `initTof()` (around line 805 after Task 1 edits). It currently looks like:

```cpp
// Detect which ToF chip is on the I2C bus. Tries VL53L1X first (more capable,
// matches the user's TOF400C breakout), then falls back to VL53L0X.
// On success, configures the chip and sets activeTofChip. Returns true on success.
bool initTof() {
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  // VL53L1X first
  if (tofL1x.begin(0x29, &Wire)) {
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
  // Fallback to VL53L0X
  if (tofL0x.begin()) {
    activeTofChip = TOF_VL53L0X;
    Serial.println("Sensor: TOF (VL53L0X) on I2C SDA="
                   + String(I2C_SDA_PIN) + " SCL=" + String(I2C_SCL_PIN));
    return true;
  }
  activeTofChip = TOF_NONE;
  return false;
}
```

Replace the entire function with:

```cpp
// Initialize the VL53L1X ToF sensor. Configures long mode + 50ms timing budget
// and starts continuous ranging. Sets activeTofChip on success. Returns true on success.
bool initTof() {
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
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
```

- [ ] **Step 2: Compile to confirm `initTof()` is now syntactically clean**

```bash
cd /home/juliettango/oil-monitor/OilTankMonitor
arduino-cli compile --fqbn esp32:esp32:esp32 OilTankMonitor.ino
```

Expected: errors in `initTof()` are gone. Errors in `readSensorRaw()` and `handleStatus()` may still remain — those are Tasks 3 and 4. Do not commit yet.

---

### Task 3: Simplify `readSensorRaw()` — remove L0X dispatch branch

**Files:**
- Modify: `OilTankMonitor/OilTankMonitor.ino`

- [ ] **Step 1: Replace the ToF branch of `readSensorRaw()`**

Find the ToF branch in `readSensorRaw()` (currently around line 856-890). It currently looks like (after the Task 1-2 work, with both chip branches present):

```cpp
  // ToF path — dispatch on detected chip
  if (activeTofChip == TOF_VL53L0X) {
    VL53L0X_RangingMeasurementData_t data;
    tofL0x.rangingTest(&data, false);
    if (data.RangeStatus == 4) return r;          // out of range / no signal
    uint16_t mm = data.RangeMilliMeter;
    // TOF_MAX_MM is 4000 to accommodate VL53L1X long mode. VL53L0X readings
    // cap naturally at ~2000 mm and use RangeStatus 4 for out-of-signal, so
    // the widened acceptance window is safe for both chips.
    if (mm < TOF_MIN_MM || mm > TOF_MAX_MM) return r;
    r.distanceMm = mm;
    r.valid = true;
    return r;
  }
  if (activeTofChip == TOF_VL53L1X) {
    if (!tofL1x.dataReady()) return r;            // measurement not ready; debounce will skip this read
    // INVARIANT: once dataReady() is true, distance/GetRangeStatus/clearInterrupt MUST run
    // back-to-back with no early returns between them. Skipping clearInterrupt() leaves the
    // chip stuck — dataReady() will never re-assert. Validation guards live AFTER clearInterrupt.
    int16_t mm = tofL1x.distance();               // signed; -1 on error
    uint8_t status = 0xFF;
    tofL1x.VL53L1X_GetRangeStatus(&status);
    tofL1x.clearInterrupt();                      // arm next measurement (must run before any return below)
    if (status != 0 || mm < 0) return r;
    if ((uint16_t)mm < TOF_MIN_MM || (uint16_t)mm > TOF_MAX_MM) return r;
    r.distanceMm = (uint16_t)mm;
    r.valid = true;
    return r;
  }
  return r;  // TOF_NONE — should not occur in normal flow
```

Replace the entire ToF block with the simplified single-chip version:

```cpp
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
```

- [ ] **Step 2: Compile**

```bash
cd /home/juliettango/oil-monitor/OilTankMonitor
arduino-cli compile --fqbn esp32:esp32:esp32 OilTankMonitor.ino
```

Expected: `readSensorRaw()` errors gone. `handleStatus()` error remains — Task 4 fixes it. Do not commit yet.

---

### Task 4: Simplify `handleStatus()` — remove L0X case from tof_chip switch

**Files:**
- Modify: `OilTankMonitor/OilTankMonitor.ino`

- [ ] **Step 1: Update the tof_chip switch**

Find the SENSOR_TOF block in `handleStatus()` (around line 670-685). It currently contains:

```cpp
    const char* tofChipName;
    switch (activeTofChip) {
      case TOF_VL53L0X: tofChipName = "vl53l0x"; break;
      case TOF_VL53L1X: tofChipName = "vl53l1x"; break;
      default:          tofChipName = "none"; break;
    }
```

Replace with:

```cpp
    const char* tofChipName;
    switch (activeTofChip) {
      case TOF_VL53L1X: tofChipName = "vl53l1x"; break;
      default:          tofChipName = "none"; break;   // TOF_NONE
    }
```

- [ ] **Step 2: Compile**

```bash
cd /home/juliettango/oil-monitor/OilTankMonitor
arduino-cli compile --fqbn esp32:esp32:esp32 OilTankMonitor.ino
```

Expected: success. Record new flash usage; should be 5–15 KB lower than before Task 1.

---

### Task 5: Update user-facing failure strings

**Files:**
- Modify: `OilTankMonitor/OilTankMonitor.ino`

- [ ] **Step 1: Update the boot serial failure message in `initSensor()`**

Find the ToF branch in `initSensor()` (around line 836-845). It currently reads:

```cpp
  // ToF path — auto-detect VL53L1X or VL53L0X
  if (!initTof()) {
    Serial.println("Sensor: TOF init FAILED — neither VL53L1X nor VL53L0X responded on I2C (SDA=21, SCL=22, VCC=3V3, GND=GND)");
    return false;
  }
  return true;
```

Replace with:

```cpp
  // ToF path — VL53L1X
  if (!initTof()) {
    Serial.println("Sensor: TOF init FAILED — VL53L1X did not respond on I2C (SDA=21, SCL=22, VCC=3V3, GND=GND)");
    return false;
  }
  return true;
```

- [ ] **Step 2: Update the post-WiFi failure Telegram message in `setup()`**

Find the post-WiFi block (around line 1051-1053). It currently reads:

```cpp
      if (cfgSensorType == SENSOR_TOF && activeTofChip == TOF_NONE) {
        sendTelegram("⚠️ ToF init failed — neither VL53L0X nor VL53L1X responded on I2C. Check wiring or switch sensor type via web UI.");
      }
```

Replace with:

```cpp
      if (cfgSensorType == SENSOR_TOF && activeTofChip == TOF_NONE) {
        sendTelegram("⚠️ ToF init failed — VL53L1X did not respond on I2C. Check wiring or switch sensor type via web UI.");
      }
```

- [ ] **Step 3: Update the web UI dropdown label**

Find the ToF dropdown option in `handleRoot()` (around line 306). It currently reads:

```cpp
  page += "<option value='1'" + String(cfgSensorType == SENSOR_TOF ? " selected" : "") + ">ToF distance (VL53L0X / VL53L1X auto-detect)</option>";
```

Replace with:

```cpp
  page += "<option value='1'" + String(cfgSensorType == SENSOR_TOF ? " selected" : "") + ">ToF distance (VL53L1X)</option>";
```

- [ ] **Step 4: Compile**

```bash
cd /home/juliettango/oil-monitor/OilTankMonitor
arduino-cli compile --fqbn esp32:esp32:esp32 OilTankMonitor.ino
```

Expected: success.

- [ ] **Step 5: Verify no remaining VL53L0X references in the sketch**

```bash
grep -in "VL53L0X\|tofL0x\|TOF_VL53L0X" /home/juliettango/oil-monitor/OilTankMonitor/OilTankMonitor.ino
```

Expected: no output. If any matches remain, investigate them — they may be in code paths Tasks 1-5 didn't reach. Fix and re-compile before committing.

- [ ] **Step 6: Commit Tasks 1-5 as a single coherent change**

```bash
cd /home/juliettango/oil-monitor
git add OilTankMonitor/OilTankMonitor.ino
git commit -m "Drop VL53L0X library support; simplify ToF code path to VL53L1X-only"
```

(All five code tasks land as a single commit because intermediate states don't compile.)

---

### Task 6: Update README and docs to remove VL53L0X mentions

**Files:**
- Modify: `README.md`
- Possibly modify: any file under `docs/` outside `specs/` and `plans/`

- [ ] **Step 1: Find existing VL53L0X mentions**

```bash
cd /home/juliettango/oil-monitor
grep -rn "VL53L0X" README.md docs/ 2>&1 | grep -v "/specs/\|/plans/"
```

(Excluding spec/plan files — those are historical and shouldn't be edited.)

- [ ] **Step 2: Edit each match to be VL53L1X-only**

For each match, edit the line to remove VL53L0X-specific text:
- `"ToF Module (VL53L0X or VL53L1X)"` → `"ToF Module (VL53L1X)"`
- `"VL53L0X or VL53L1X, auto-detected"` → `"VL53L1X"`
- `"VL53L0X or VL53L1X (auto-detected)"` → `"VL53L1X"`
- `"ToF Sensor Wiring (VL53L0X or VL53L1X)"` → `"ToF Sensor Wiring (VL53L1X)"`
- `"ToF Pin (VL53L0X/VL53L1X)"` → `"ToF Pin (VL53L1X)"`
- The line `arduino-cli lib install "Adafruit_VL53L0X"` should be deleted entirely (the VL53L1X line below it stays).

If any match is in a section that's specifically about distinguishing the two chips (e.g., "Both sensors share identical wiring"), rewrite the surrounding sentence to remove the comparison since there's only one ToF chip now.

- [ ] **Step 3: Verify cleanup**

```bash
cd /home/juliettango/oil-monitor
grep -rn "VL53L0X" README.md docs/ 2>&1 | grep -v "/specs/\|/plans/"
```

Expected: no output.

- [ ] **Step 4: Commit**

```bash
cd /home/juliettango/oil-monitor
git add README.md
# If docs/ outside specs/plans had matches, also `git add docs/`
git commit -m "Remove VL53L0X mentions from README/docs after dropping driver"
```

---

### Task 7: Manual end-to-end verification

**Files:** None (runtime test on hardware).

- [ ] **Step 1: Final compile and record flash savings**

```bash
cd /home/juliettango/oil-monitor/OilTankMonitor
arduino-cli compile --fqbn esp32:esp32:esp32 OilTankMonitor.ino
```

Note the new `Sketch uses N bytes` value. Compute delta from the pre-removal baseline (1,138,380 bytes per the last build). Expect 5–15 KB lower. Report the actual figure.

- [ ] **Step 2: Flash to ESP32**

```bash
arduino-cli upload --fqbn esp32:esp32:esp32 -p /dev/ttyUSB0 OilTankMonitor.ino
```

Expected: upload succeeds, board hard-resets.

- [ ] **Step 3: Verify normal operation (digital and IR_BREAK modes — regression check)**

In the web UI, switch sensor type to "Digital threshold" and save. Confirm digital sensor mode still works (LOW/HIGH transitions if the sensor is connected).

Switch to "IR break-beam (sight gauge puck)" and save. Confirm IR mode still works.

- [ ] **Step 4: Verify ToF mode user-facing strings**

Switch sensor type to "ToF distance (VL53L1X)". The dropdown text should now say `"ToF distance (VL53L1X)"` (no slash, no "auto-detect"). Save.

If a VL53L1X is connected and working: `/status` should report `"sensor_type":"tof"`, `"tof_chip":"vl53l1x"`, valid `distance_mm`. Behavior unchanged from prior dual-chip build.

If no ToF chip is connected: failure Telegram message should now read `⚠️ ToF init failed — VL53L1X did not respond on I2C. Check wiring or switch sensor type via web UI.` (single-chip text, no "neither X nor Y" phrasing).

- [ ] **Step 5: No commit**

Verification produces no code change.

---

## Self-Review

**Spec coverage check** — every requirement in the spec maps to a task:

| Spec requirement | Task |
|------------------|------|
| Remove `#include <Adafruit_VL53L0X.h>` | Task 1 Step 1 |
| Remove `Adafruit_VL53L0X tofL0x;` global | Task 1 Step 2 |
| Trim `TofChip` enum to drop `TOF_VL53L0X` | Task 1 Step 3 |
| Remove L0X branch in `initTof()` | Task 2 |
| Remove L0X branch in `readSensorRaw()` | Task 3 |
| Remove `case TOF_VL53L0X:` from `handleStatus()` switch | Task 4 |
| Update boot serial failure text to single-chip | Task 5 Step 1 |
| Update Telegram failure text to single-chip | Task 5 Step 2 |
| Update web UI dropdown label | Task 5 Step 3 |
| README cleanup (remove dual-chip language) | Task 6 |
| Verify normal operation post-removal | Task 7 |
| Record flash savings | Task 7 Step 1 |

All spec sections covered.

**Placeholder scan** — no TBDs, no "implement later", no missing code blocks. Tasks 1-4 are intentionally non-compiling intermediate states; this is called out clearly and the commit happens only at Task 5 once the code compiles cleanly.

**Type consistency** — `TofChip` enum values (`TOF_NONE`, `TOF_VL53L1X`), `tofL1x`, `activeTofChip`, `TOF_MIN_MM`, `TOF_MAX_MM`, `VL53L1X_SetDistanceMode`, `VL53L1X_GetRangeStatus`, `setTimingBudget`, `startRanging`, `dataReady`, `distance`, `clearInterrupt` — all names used consistently across tasks.

Plan is complete.
