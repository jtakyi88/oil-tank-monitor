# Sensor-Type Label Helper Consolidation — Design

**Date**: 2026-04-30
**Files touched**: `OilTankMonitor/OilTankMonitor.ino`
**Sub-project of**: Production-readiness cleanup pass (A → B → **C** → D)

## Background

Three separate `switch (cfgSensorType)` blocks in the firmware map the `SensorType` enum to display strings, each using a different vocabulary:

| Site (current line) | Vocabulary | Examples |
|---------------------|------------|----------|
| `handleStatus()` JSON (line 662) | machine-readable, lowercase | `"digital"`, `"tof"`, `"ir_break"` |
| Boot serial summary (line 1054) | enum-style, uppercase | `"DIGITAL"`, `"TOF"`, `"IR_BREAK"` |
| ONLINE Telegram message (line 1084) | human-readable, mixed case + qualifier | `"Digital"`, `"ToF"`, `"IR break-beam (sight gauge)"` |

The duplication isn't large in absolute terms but matches a pattern that will repeat as new sensor types are added. The `handleStatus` switch also wraps its result in `String(...)`, which causes one heap allocation per `/status` request — small per call but unnecessary.

## Goal

Replace the three inline switches with three named helper functions that return `const char*` (flash-resident strings, no heap). Each helper owns one vocabulary and is called from one site.

## Non-Goals

- No change to any user-visible string. JSON values, boot logs, and Telegram wording all stay byte-for-byte identical.
- No introduction of `PROGMEM` or `F()` macros — that belongs to sub-project D's broader heap/string cleanup.
- No README change. The refactor is internal only.
- No collapsing the three vocabularies into one. Each serves a distinct audience (machines, boot logs, humans) and they should not share storage.

## The Helpers

Three file-local `static const char*` helpers, defined once near the existing `SensorType` enum (around line 30 of `OilTankMonitor/OilTankMonitor.ino`):

```cpp
static const char* sensorTypeJsonName(SensorType t) {
  switch (t) {
    case SENSOR_TOF:      return "tof";
    case SENSOR_IR_BREAK: return "ir_break";
    default:              return "digital";
  }
}

static const char* sensorTypeBootName(SensorType t) {
  switch (t) {
    case SENSOR_TOF:      return "TOF";
    case SENSOR_IR_BREAK: return "IR_BREAK";
    default:              return "DIGITAL";
  }
}

static const char* sensorTypeDisplayName(SensorType t) {
  switch (t) {
    case SENSOR_TOF:      return "ToF";
    case SENSOR_IR_BREAK: return "IR break-beam (sight gauge)";
    default:              return "Digital";
  }
}
```

The `default` arm preserves the existing fallback to digital — the current code uses the same fallback at every site, so behavior is unchanged.

## Call-Site Changes

### `handleStatus()` JSON (currently lines 661–667)

**Before:**
```cpp
const char* sensorTypeName;
switch (cfgSensorType) {
  case SENSOR_TOF:      sensorTypeName = "tof"; break;
  case SENSOR_IR_BREAK: sensorTypeName = "ir_break"; break;
  default:              sensorTypeName = "digital"; break;
}
json += "\"sensor_type\":\"" + String(sensorTypeName) + "\",";
```

**After:**
```cpp
json += "\"sensor_type\":\"";
json += sensorTypeJsonName(cfgSensorType);
json += "\",";
```

The `String(sensorTypeName)` allocation goes away. Three `+=` calls replace one chained `+`; the `String` class handles `const char*` operands directly without intermediate temporaries.

### Boot serial summary (currently lines 1053–1061)

**Before:**
```cpp
const char* bootSensorTypeName;
switch (cfgSensorType) {
  case SENSOR_TOF:      bootSensorTypeName = "TOF"; break;
  case SENSOR_IR_BREAK: bootSensorTypeName = "IR_BREAK"; break;
  default:              bootSensorTypeName = "DIGITAL"; break;
}
Serial.printf("Sensor type: %s | ToF thresholds (mm): low=%u half=%u high=%u\n",
              bootSensorTypeName,
              cfgTofLow, cfgTofHalf, cfgTofHigh);
```

**After:**
```cpp
Serial.printf("Sensor type: %s | ToF thresholds (mm): low=%u half=%u high=%u\n",
              sensorTypeBootName(cfgSensorType),
              cfgTofLow, cfgTofHalf, cfgTofHigh);
```

The local `bootSensorTypeName` variable is removed entirely.

### ONLINE Telegram message (currently lines 1084–1088)

**Before:**
```cpp
String msg = "🛢️ Oil tank monitor is ONLINE.\nSensor: ";
switch (cfgSensorType) {
  case SENSOR_TOF:      msg += "ToF"; break;
  case SENSOR_IR_BREAK: msg += "IR break-beam (sight gauge)"; break;
  default:              msg += "Digital"; break;
}
```

**After:**
```cpp
String msg = "🛢️ Oil tank monitor is ONLINE.\nSensor: ";
msg += sensorTypeDisplayName(cfgSensorType);
```

## Edge Cases

- **Adding a new `SensorType` value in the future**: each helper's `default` arm covers any unknown enum value with the digital vocabulary. A future contributor extending the enum needs to add three case arms (one per helper) — but they only need to look in one place per vocabulary, instead of grepping the file for every switch.
- **Compiler inlining**: with three call sites and small bodies, the compiler may inline these. That's fine — it's an implementation detail. The point of the refactor is the source-level clarity and the dropped String allocation, not micro-optimization.

## Flash & Heap Impact

Each removed inline switch is replaced with one helper definition; net code volume is roughly the same. The string literals themselves are deduplicated by the toolchain in either form. The expected flash delta is small (−200 to −500 bytes from the eliminated locals and `String` wrapper). Per-`/status`-request heap pressure drops by one `String` allocation.

## Testing

Project has no automated tests. Manual verification:

1. `arduino-cli compile --fqbn esp32:esp32:esp32 OilTankMonitor.ino` — clean compile, sketch size ≤ pre-task baseline.
2. Flash to ESP32. Capture 20 s of serial. Confirm the boot summary line still reads exactly `Sensor type: TOF | ToF thresholds (mm): low=200 half=130 high=60` (or `DIGITAL` / `IR_BREAK` per saved config).
3. `curl http://192.168.0.241/status` and confirm the JSON still contains `"sensor_type":"tof"` (or matching).
4. Confirm the Telegram ONLINE message still includes `Sensor: ToF` (or matching wording for digital / IR break-beam).
5. Switch sensor type via web UI to each of the three options and verify all three vocabularies remain correct.

## Implementation Notes

This is the third sub-project in the production-readiness pass. Sub-project D will revisit the broader String/heap question; this sub-project removes a small, surgical instance of the same pattern without touching anything else.
