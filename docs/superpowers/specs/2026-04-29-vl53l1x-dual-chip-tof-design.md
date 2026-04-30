# Dual-Chip ToF Support (VL53L0X + VL53L1X) — Design

**Date**: 2026-04-29
**File touched**: `OilTankMonitor/OilTankMonitor.ino`, `README.md`, possibly `docs/`
**Companion to**: `2026-04-28-interchangeable-sensors-design.md`, `2026-04-29-ir-break-beam-sensor-design.md`

## Background

The current firmware's ToF mode targets the **VL53L0X** chip via the `Adafruit_VL53L0X` library. The user owns a **TOF400C** breakout (Amazon B0DC6M6G7W), which carries the newer **VL53L1X** chip. The two chips share the same I2C address (0x29) and similar wiring, but use **different drivers, different APIs, and have different range**:

| Aspect | VL53L0X | VL53L1X |
|--------|---------|---------|
| Max range | ~2 m | ~4 m (long mode) |
| Driver | `Adafruit_VL53L0X` | `Adafruit_VL53L1X` |
| Read pattern | Blocking `rangingTest()` | Async: `dataReady()` → `distance()` → `clearInterrupt()` |
| Distance modes | Single | Short (1.3 m) / Medium (3 m) / Long (4 m) |
| Default I2C address | 0x29 | 0x29 |

Currently, ToF init silently fails when a VL53L1X is connected because the VL53L0X driver doesn't know how to talk to it. This forces the user into Digital or IR_BREAK mode.

## Goal

Support both chips with **runtime auto-detection** so the user never has to think about which physical sensor is on the bus. Single dropdown option ("ToF distance"); the firmware identifies the chip on init.

## Non-Goals

- Manual chip selection in the web UI (auto-detect handles it).
- Supporting multiple ToF chips simultaneously on the same bus (they share address 0x29 — would need XSHUT control).
- Caching the detection result in NVS to skip probing on subsequent boots (boot-time cost is small; YAGNI).
- Per-chip threshold profiles (thresholds are in mm and chip-agnostic).

## Detection Architecture

### New globals

```cpp
enum TofChip { TOF_NONE, TOF_VL53L0X, TOF_VL53L1X };
TofChip activeTofChip = TOF_NONE;

Adafruit_VL53L0X tofL0;
Adafruit_VL53L1X tofL1;
```

The previous single `Adafruit_VL53L0X tofSensor;` global is removed.

### Detection helper

```cpp
bool initTof() {
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  // Try VL53L1X first (matches user's hardware, more capable).
  if (tofL1.begin(0x29, &Wire)) {
    tofL1.setDistanceMode(VL53L1X::Long);   // 4 m range
    tofL1.setTimingBudget(50);              // ms; sane default
    tofL1.startRanging();                   // arm continuous measurement
    activeTofChip = TOF_VL53L1X;
    Serial.println("Sensor: TOF (VL53L1X, long mode) on I2C SDA="
                   + String(I2C_SDA_PIN) + " SCL=" + String(I2C_SCL_PIN));
    return true;
  }
  // Fallback to VL53L0X.
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

(Exact `setDistanceMode` / `setTimingBudget` API may differ from the Adafruit library's actual symbol names — implementation will use whatever the library actually exports. The plan will pin the exact calls.)

### Wiring into existing code

`initSensor()` ToF branch calls `initTof()` instead of `tofSensor.begin()`. The setup-phase post-WiFi retry (around line 963) and the `loop()` bus-recovery path (around line 1028) both call `initTof()` instead of `tofSensor.begin()`.

## Read Path

`readSensorRaw()` ToF branch dispatches on `activeTofChip`:

```cpp
SensorReading r = { false, false, 0 };
if (activeTofChip == TOF_VL53L0X) {
  VL53L0X_RangingMeasurementData_t data;
  tofL0.rangingTest(&data, false);
  if (data.RangeStatus == 4) return r;
  uint16_t mm = data.RangeMilliMeter;
  if (mm < TOF_MIN_MM || mm > TOF_MAX_MM) return r;
  r.distanceMm = mm; r.valid = true;
  return r;
}
if (activeTofChip == TOF_VL53L1X) {
  if (!tofL1.dataReady()) return r;       // valid=false; debounce will skip
  uint16_t mm = tofL1.distance();
  uint8_t status = tofL1.rangeStatus();
  tofL1.clearInterrupt();
  if (status != 0) return r;
  if (mm < TOF_MIN_MM || mm > TOF_MAX_MM) return r;
  r.distanceMm = mm; r.valid = true;
  return r;
}
return r;  // TOF_NONE — should not occur in normal flow
```

**Behavior notes**:

- **VL53L1X "not ready" responses are normal.** With a 50 ms timing budget, `dataReady()` is false for ~50 ms between measurements. The existing median-of-3 + `validCount >= 2` logic in `readSensor()` handles this — occasional sub-quorum reads are correctly downgraded to invalid.
- **Bus-recovery cadence** (`TOF_RECOVERY_CYCLES = 3` consecutive invalids) reuses the existing logic; on recovery, `initTof()` re-runs detection so a chip swap is picked up automatically.

## Constants

| Constant | Old | New |
|----------|-----|-----|
| `TOF_MAX_MM` | `2000` | `4000` |

`TOF_MIN_MM` (30) unchanged. The wider acceptance window is harmless for VL53L0X readings (which physically can't exceed ~2000 mm).

## User-Facing Strings

| Site | Old | New |
|------|-----|-----|
| Boot serial in `initSensor()` | `Sensor: TOF (VL53L0X) on I2C SDA=21 SCL=22` | `Sensor: TOF (VL53L1X, long mode) on I2C ...` or `Sensor: TOF (VL53L0X) on I2C ...` based on detection |
| Boot Serial.printf summary (~line 993) | `Sensor type: TOF \| ToF thresholds...` | unchanged — chip variant is runtime detail |
| ONLINE Telegram message | `Sensor: ToF` | unchanged |
| ToF init failure Telegram | `⚠️ ToF init failed — check I2C wiring or switch sensor type via web UI.` | `⚠️ ToF init failed — neither VL53L0X nor VL53L1X responded on I2C. Check wiring or switch sensor type via web UI.` |
| Web UI dropdown option | `ToF distance (VL53L0X)` | `ToF distance (VL53L0X / VL53L1X auto-detect)` |
| `/status` JSON | (no chip field) | new field `"tof_chip"`: `"vl53l0x"`, `"vl53l1x"`, or `"none"` |
| `README.md` + `docs/` | mentions VL53L0X only | mention both chips, note auto-detection |

## Code Touchpoints

All in `OilTankMonitor/OilTankMonitor.ino` unless noted.

| Location | Change |
|----------|--------|
| Includes | Add `#include <Adafruit_VL53L1X.h>`; keep `<Adafruit_VL53L0X.h>` |
| Globals | Remove `tofSensor`. Add `tofL0`, `tofL1`, `activeTofChip`, `TofChip` enum |
| Constants | `TOF_MAX_MM` → `4000` |
| `initSensor()` | ToF branch calls new `initTof()` helper |
| New helper `initTof()` | VL53L1X-first detection per Section "Detection helper" |
| `readSensorRaw()` ToF branch | Dispatch on `activeTofChip` per Section "Read Path" |
| `setup()` post-WiFi check (~line 963) | `tofSensor.begin()` → `initTof()` |
| `loop()` bus-recovery (~line 1028) | `tofSensor.begin()` → `initTof()` |
| `handleStatus()` | Emit `"tof_chip"` field when `cfgSensorType == SENSOR_TOF` |
| `handleRoot()` dropdown | Update option label |
| ToF init failure Telegram (in `setup()` and possibly elsewhere) | Update message text |
| `README.md` + `docs/` | Update sensor list |

## Flash Budget

Current build: **1,132,828 / 1,310,720 bytes (86%)**. Adding `Adafruit_VL53L1X` is estimated at +15–25 KB → ~88–90% flash.

**Risk**: hitting the 1.31 MB default app partition. Mitigation if exceeded:

1. Switch to a larger partition scheme via `arduino-cli compile --build-property "build.partitions=min_spiffs"` (or equivalent) — gives ~1.9 MB app partition with reduced SPIFFS.
2. If still too tight, drop the VL53L0X library entirely and revisit the "Drop VL53L0X" option.

The implementation plan's first task is to measure actual flash usage so this risk is quantified before further work.

## Edge Cases

- **Both chips on bus simultaneously**: physically impossible without external XSHUT manipulation (shared address 0x29). Not a concern.
- **Hot-swap during runtime**: the existing bus-recovery (3 consecutive invalids → `initTof()`) re-runs detection. Swap is picked up.
- **VL53L1X stuck not-ready**: existing fault detector triggers bus recovery after `TOF_RECOVERY_CYCLES` invalids. Same as VL53L0X stuck-on-error path.
- **NVS-stored thresholds**: stored in mm, chip-agnostic. No migration. User-set thresholds tuned for VL53L0X (≤ 2000 mm) remain valid.
- **VL53L1X library exists in arduino-cli registry**: must be verified during implementation. If not present, `arduino-cli lib install "Adafruit VL53L1X"` is the install step.

## Testing

Project has no automated test framework. Manual verification plan:

1. `arduino-cli lib install "Adafruit VL53L1X"` (if not already installed).
2. `arduino-cli compile --fqbn esp32:esp32:esp32 OilTankMonitor.ino`. Record flash usage. If above ~95%, escalate (partition change or library drop).
3. Flash to ESP32. Connect VL53L1X (TOF400C breakout) on I2C (SDA=21, SCL=22, 3V3, GND).
4. Switch sensor type to "ToF distance" via web UI. Save.
5. Confirm boot serial reads `Sensor: TOF (VL53L1X, long mode) on I2C ...`.
6. `/status` JSON contains `"sensor_type":"tof"`, `"tof_chip":"vl53l1x"`, `"distance_mm":<value>`.
7. Vary distance to the puck/surface; confirm `level` transitions through LOW/BELOW_HALF/ABOVE_HALF/HIGH per configured thresholds.
8. Telegram alerts fire on transitions; hourly LOW reminder cadence works.
9. (If a VL53L0X breakout is ever obtained) Repeat steps 3–7 with VL53L0X swapped in. Confirm boot reads `Sensor: TOF (VL53L0X) on I2C ...`, `/status` reports `"tof_chip":"vl53l0x"`, behavior otherwise identical.
10. Disconnect ToF entirely while running; confirm fault detector fires and "ToF init failed" Telegram with new wording arrives.
11. Switch back to Digital and IR_BREAK modes via web UI; confirm both still work (regression check).
