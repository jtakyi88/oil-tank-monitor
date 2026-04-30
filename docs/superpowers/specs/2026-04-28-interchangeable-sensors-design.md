# Interchangeable Sensor Support — Design Spec

**Date:** 2026-04-28
**Target version:** v2.0.0 (from v1.1.0)
**Status:** Approved, ready for implementation planning

## Background and motivation

The current firmware (v1.1.0) is hardcoded to the XKC-Y25-V non-contact capacitive liquid-level sensor, which detects liquid through glass via capacitance change. Field experience has revealed that many residential oil-tank sight gauges are **dry mechanical floats** — an opaque puck on a rod that rises and falls with the actual oil level inside the tank. The capacitive sensor cannot detect these because there is no liquid in the gauge tube.

Two sensor types address this gap:

1. **IR break-beam pair** — a single-threshold optical sensor; the opaque puck blocks the beam when above the mark and lets it pass when it drops below. Electrically and logically identical to the existing XKC-Y25-V (single GPIO, HIGH/LOW digital signal).
2. **Time-of-Flight (ToF) distance sensor** — mounted on top of the sight gauge, measures distance to the puck, enabling a multi-state level model (LOW / HALF / HIGH) with refill-confirmation notifications.

This spec defines a single firmware that supports either sensor, selectable at runtime via the existing web UI, with no breaking changes for v1.1.0 installs.

## Goals

- Single firmware binary supports both sensor types.
- Sensor type is selected at runtime via the web UI dropdown — no re-flash required to switch.
- Existing v1.1.0 installs OTA-upgrade to v2.0.0 with zero behavior change (default to digital sensor type).
- ToF support adds multi-state notifications: LOW, HALF (both directions), HIGH.
- Existing LOW-alert behavior preserved (immediate notification + hourly reminders + restored confirmation).
- All settings web-configurable; nothing hardcoded.

## Non-goals (out of scope)

- Teach-mode / percentage-based threshold calibration.
- Separate dropdown entries for IR break-beam vs capacitive (they share one code path).
- Auto-detection of attached sensor at boot.
- Multiple sensors on one device.
- MQTT/Home Assistant integration, battery deep sleep, history graphing — all deferred.

## Architecture decision: tagged struct + dispatch

The new sensor abstraction is a tagged struct with switch-based dispatch in two functions (`initSensor`, `readSensor`). Considered alternatives:

- **Virtual base class** — Cleaner OOP but introduces a class island in an otherwise-procedural sketch. Two sensor types does not justify the inconsistency.
- **Function-pointer ops table** — Faster dispatch but error-prone (NULL pointers) and less idiomatic for Arduino code.

**Selected:** tagged struct + dispatch. Matches the existing 737-line procedural style; adding a third sensor later means one new enum value plus one new switch case.

## File layout

**Single-file monolithic** — `OilTankMonitor/OilTankMonitor.ino`. The file grows from ~737 lines to roughly ~1,000 lines. Approved over a multi-`.ino` split for ease of newcomer reading.

## Sensor abstraction

### Types and constants

```cpp
enum SensorType { SENSOR_DIGITAL = 0, SENSOR_TOF = 1 };

struct SensorReading {
  bool valid;            // false on hardware fault (I2C timeout, ToF out-of-range)
  bool digitalState;     // HIGH = object/liquid present; meaningful when SENSOR_DIGITAL
  uint16_t distanceMm;   // mm to puck; meaningful when SENSOR_TOF
};

SensorType cfgSensorType = SENSOR_DIGITAL;   // default — protects v1.x upgraders
const int DIGITAL_SENSOR_PIN = 4;            // GPIO4 (unchanged)
const int I2C_SDA_PIN = 21;                  // ESP32 default
const int I2C_SCL_PIN = 22;                  // ESP32 default
const int TOF_HYSTERESIS_MM = 5;             // band around each threshold
```

### Dispatch functions (replace the existing `readSensorDebounced()`)

- `bool initSensor()` — for digital, sets `pinMode(DIGITAL_SENSOR_PIN, INPUT)` and returns true. For ToF, calls `Wire.begin(21,22)` then `tofSensor.begin()`; returns false on I2C/init failure.
- `SensorReading readSensor()` — switch on `cfgSensorType`. Digital path returns instant `digitalRead`. ToF path calls `Adafruit_VL53L0X::rangingTest()`; sets `valid=false` if `RangeStatus == 4` (out of range) or I2C timeout.

### Filtering

- **Digital:** existing 3-read debounce (`DEBOUNCE_COUNT = 3`, sampled every `SENSOR_CHECK_MS = 5000ms`). No change.
- **ToF:** **median of 3 consecutive readings** taken ~50ms apart, every `SENSOR_CHECK_MS`. Median rejects single-shot noise spikes; cheaper and more robust than a moving average for this signal.

### Hysteresis (ToF only)

Each threshold has a ±5 mm dead band. Example with `Low=200`: state goes "below Low" only when reading rises above 205 mm; goes "above Low" only when reading falls below 195 mm. Prevents oscillating notifications when the puck sits exactly at a threshold.

### Hardware library and module

- **Library:** `Adafruit_VL53L0X` (Apache 2.0).
- **Chip:** VL53L0X — 30 mm to ~2 m range, more than adequate for residential sight gauges (typically 150–300 mm tall).
- **Module cost:** ~$3–5 (Adafruit, HiLetgo, generic breakouts on Amazon).
- **Wiring:** VCC → 3V3, GND → GND, SDA → GPIO21, SCL → GPIO22.

### Sensor-fault behavior

If `readSensor()` returns `valid=false` for ≥5 consecutive cycles (~25 s), send a one-shot Telegram alert: `⚠️ Sensor fault — no valid reading for 25s. Check wiring.` Do not send repeated fault alerts. Clear the fault state when readings resume.

## State machine and notifications

### Digital sensor (unchanged from v1.x)

Two states — `LEVEL_OK`, `LEVEL_LOW`. Transition logic and messages match current behavior exactly.

### ToF sensor — four states

Bucketed by the configured mm thresholds. Smaller mm = puck closer to top-mounted sensor = fuller tank, so `HIGH < HALF < LOW` in mm:

```cpp
enum LevelState {
  LEVEL_LOW,           // distance > cfgTofLow
  LEVEL_BELOW_HALF,    // cfgTofHalf < distance <= cfgTofLow
  LEVEL_ABOVE_HALF,    // cfgTofHigh < distance <= cfgTofHalf
  LEVEL_HIGH           // distance <= cfgTofHigh
};
LevelState currentState;
```

Each `SENSOR_CHECK_MS` cycle: bucket the median-of-3 reading (with hysteresis) into a `LevelState`. If different from `currentState`, fire the matching transition message and update `currentState`.

### Transition matrix (both directions)

| From → To | Direction | Notification |
|---|---|---|
| `LEVEL_LOW` → `LEVEL_BELOW_HALF` | refill | `✅ Oil tank above low mark — refill detected.` |
| `LEVEL_BELOW_HALF` → `LEVEL_ABOVE_HALF` | refill | `🔼 Tank is half refilled.` |
| `LEVEL_ABOVE_HALF` → `LEVEL_HIGH` | refill | `🔝 Tank at HIGH — refill complete.` |
| `LEVEL_HIGH` → `LEVEL_ABOVE_HALF` | drain | (silent — normal use) |
| `LEVEL_ABOVE_HALF` → `LEVEL_BELOW_HALF` | drain | `📉 Tank is half empty — plan a refill.` |
| `LEVEL_BELOW_HALF` → `LEVEL_LOW` | drain | `⚠️ OIL TANK LOW — please refill.` (starts hourly reminder loop) |

Skipped buckets between two consecutive reads (e.g., quick refill jumping from `LOW` straight to `ABOVE_HALF`) fire each intervening transition's message in order, so the user never silently misses a level event.

### Hourly reminder (both sensor types)

Only fires while `currentState == LEVEL_LOW`. Same `ALERT_INTERVAL_MS = 3600000UL` as today. Cleared automatically on first non-LOW transition.

### Boot behavior

First reading after WiFi connect *initializes* `currentState` silently — no transition notifications. Boot-online message reports the initial level inline:

> `🛢️ Oil tank monitor is ONLINE. Current level: ABOVE HALF (110mm). Settings: http://192.168.x.x`

(Example assumes default thresholds `low=200, half=130, high=60` — 110 mm falls between `high` and `half`, i.e. `LEVEL_ABOVE_HALF`.)

Avoids spamming "Half refilled / High reached" notifications on every reboot when the tank happens to be full.

### Sensor-type switch via web UI

`handleSave()` already calls `ESP.restart()` after every successful save (existing v1.x behavior). After a sensor-type change, the post-restart boot path runs `initSensor()` for the new type and re-initializes `currentState` silently from the first valid reading.

## Persistence

NVS additions in the existing `oilmon` namespace:

| Key | Type | Default | Purpose |
|---|---|---|---|
| `sensor_type` | `uint8` | `0` (digital) | Sensor selection |
| `tof_low` | `uint16` | `200` | LOW threshold in mm |
| `tof_half` | `uint16` | `130` | HALF threshold in mm |
| `tof_high` | `uint16` | `60` | HIGH threshold in mm |

`loadSettings()` reads each with safe defaults. Existing v1.x devices upgrading via OTA find no `sensor_type` key, default to `0` (digital), and behave identically to v1.x — zero-friction upgrade.

## Web UI changes

New section in `buildConfigPage()` between *Network Settings* and *Web Interface Password*:

```
═══ Sensor Configuration ═══
Sensor Type:  [▾ Digital threshold     ]
              [   ToF distance         ]

▼ When "ToF distance" is selected, reveal:

  Current Reading: 147 mm    [updates every 2s]

  LOW threshold (mm):   [200]   ← alert below this
  HALF threshold (mm):  [130]
  HIGH threshold (mm):  [ 60]   ← refill complete above this
```

Conditional toggle uses the same `.tof-fields { display:none } .show` JS pattern already used for the static-IP fields. **Live distance display** polls `/status` every 2 s when the dropdown shows ToF — gives the user a real-time reading to inform their threshold choices during install.

### Validation on save (`handleSave`) for ToF type

- If `tof_high >= tof_half || tof_half >= tof_low`: reject with inline error page *"ToF thresholds must satisfy HIGH < HALF < LOW (smaller mm = fuller tank)"*. Does not call `ESP.restart()`; returns to config page so user can fix.
- If any threshold is outside `30 ≤ value ≤ 2000` (VL53L0X spec range): reject with the same inline-error pattern.

## API: `/status` JSON additions

Existing endpoint, additive — never removes fields:

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

For digital installs: `sensor_type: "digital"`, `level: "LOW"` or `"OK"`, `distance_mm` and `thresholds` omitted. The legacy `oil_low` field is kept and mirrors `level == "LOW"` for any v1.x clients that depend on it.

The 2-second live-reading poll happens after the user has logged in to view the config page, so existing session-auth on `/status` is unchanged.

## Error handling

1. **ToF init failure at boot** — Log to Serial, continue boot to web-server mode, send Telegram (if WiFi up): `⚠️ ToF init failed — check I2C wiring or switch sensor type via web UI.` Device stays accessible so user can recover without USB.
2. **I2C bus recovery** — After 3 consecutive `valid=false` ToF reads, call `Wire.end()` + `Wire.begin(21,22)` + `tofSensor.begin()` once before declaring the persistent 5-cycle fault. Recovers from transient bus glitches.
3. **Threshold range check** — Each ToF threshold must be `30 ≤ value ≤ 2000` mm. `handleSave` rejects out-of-range values with the same inline-error pattern as the ordering check.
4. **Sensor fault** — Per Section 2: 5 consecutive invalid reads → one-shot Telegram alert; cleared on first valid read.

## Testing plan

Manual integration testing — no unit-test framework in this codebase.

| # | Test | Expected |
|---|---|---|
| 1 | Digital regression: jumper GPIO4 LOW/HIGH | LOW alert, restored, hourly reminder all match v1.1.0 behavior |
| 2 | ToF transitions: target swept through range | Each transition message fires once, in correct direction |
| 3 | ToF hysteresis: oscillate target ±2 mm at a threshold | Zero notifications |
| 4 | ToF noise rejection: single 50 mm spike during otherwise-stable 200 mm reading | No state change (median-of-3 filters it) |
| 5 | Sensor type switch via web UI | Save triggers restart, new sensor active, silent boot init |
| 6 | v1.1.0 → v2.0.0 OTA upgrade | Settings preserved; defaults to digital; behaves identically to v1.1.0 |
| 7 | Threshold validation | `low=100, half=130, high=60` rejected; `high=0` or `3000` rejected |
| 8 | I2C disconnected mid-run | Single fault Telegram after ~25 s; reconnect resumes silently |
| 9 | Boot at non-LOW level | Online message reports current state; no transition spam |
| 10 | 24 h stability run | No memory leaks, no spurious alerts |

## Documentation updates (`README.md`)

- Hardware BOM gains a row: `VL53L0X module — Adafruit/HiLetgo/etc — ~$3-5`.
- Wiring section gains a *ToF wiring* subsection (VCC → 3V3, GND → GND, SDA → GPIO21, SCL → GPIO22).
- Library install: add `arduino-cli lib install "Adafruit_VL53L0X"`.
- Setup section gains step *"Select sensor type and enter thresholds (ToF only)"*.
- API section: updated `/status` JSON example reflecting new fields.
- New section *"Choosing a Sensor"* explaining digital (single threshold, simple) vs ToF (multi-state, refill confirmation, dry-gauge floats).

## Versioning and migration

- Bump `FW_VERSION` from `1.1.0` → `2.0.0`.
- NVS schema is additive (no breaking key changes), so OTA from v1.x is seamless.
- No migration code needed — `loadSettings()` reads new keys with defaults if absent.
