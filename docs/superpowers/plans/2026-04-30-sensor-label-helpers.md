# Sensor-Type Label Helper Consolidation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace three inline `switch (cfgSensorType)` blocks with three named helper functions returning `const char*`, keeping every user-visible string byte-for-byte identical.

**Architecture:** Three file-local helpers added immediately after the existing `levelStateName()` helper (around line 61 of `OilTankMonitor/OilTankMonitor.ino`), matching its style. Three call sites updated to use the helpers. No `PROGMEM`/`F()` migration — that belongs to sub-project D.

**Tech Stack:** Arduino-ESP32, single `.ino` sketch, `arduino-cli` for compile/upload, manual hardware verification (no test framework).

---

## File Structure

| File | Responsibility | Change scope |
|------|----------------|--------------|
| `OilTankMonitor/OilTankMonitor.ino` | All firmware behavior | Add 3 helper functions after line 61; update 3 call sites |

Spec reference: `docs/superpowers/specs/2026-04-30-sensor-label-helpers-design.md`.

This plan uses **one task** because adding helpers without using them produces unused-function warnings, and using them without defining them is a build error. Both halves must land in the same commit.

---

### Task 1: Add helpers, replace call sites, verify behavior unchanged

**Files:**
- Modify: `OilTankMonitor/OilTankMonitor.ino`

- [ ] **Step 1: Add the three helper functions after `levelStateName()`**

Locate `levelStateName()` (currently lines 50–61):

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

Insert immediately after its closing brace (line 61), before the I2C pin constants:

```cpp
const char* sensorTypeJsonName(SensorType t) {
  switch (t) {
    case SENSOR_TOF:      return "tof";
    case SENSOR_IR_BREAK: return "ir_break";
    default:              return "digital";
  }
}

const char* sensorTypeBootName(SensorType t) {
  switch (t) {
    case SENSOR_TOF:      return "TOF";
    case SENSOR_IR_BREAK: return "IR_BREAK";
    default:              return "DIGITAL";
  }
}

const char* sensorTypeDisplayName(SensorType t) {
  switch (t) {
    case SENSOR_TOF:      return "ToF";
    case SENSOR_IR_BREAK: return "IR break-beam (sight gauge)";
    default:              return "Digital";
  }
}
```

Note: matching the existing `levelStateName()` style — no `static` qualifier. The single-TU Arduino sketch makes the qualifier unnecessary.

- [ ] **Step 2: Replace the `handleStatus()` JSON switch (currently lines 661–667)**

Locate:

```cpp
  const char* sensorTypeName;
  switch (cfgSensorType) {
    case SENSOR_TOF:      sensorTypeName = "tof"; break;
    case SENSOR_IR_BREAK: sensorTypeName = "ir_break"; break;
    default:              sensorTypeName = "digital"; break;
  }
  json += "\"sensor_type\":\"" + String(sensorTypeName) + "\",";
```

Replace with:

```cpp
  json += "\"sensor_type\":\"";
  json += sensorTypeJsonName(cfgSensorType);
  json += "\",";
```

The local `sensorTypeName` variable and its switch are removed.

- [ ] **Step 3: Replace the boot serial summary switch (currently lines 1053–1061)**

Locate:

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

Replace with:

```cpp
  Serial.printf("Sensor type: %s | ToF thresholds (mm): low=%u half=%u high=%u\n",
                sensorTypeBootName(cfgSensorType),
                cfgTofLow, cfgTofHalf, cfgTofHigh);
```

The local `bootSensorTypeName` variable and its switch are removed.

- [ ] **Step 4: Replace the ONLINE Telegram switch (currently lines 1083–1088)**

Locate:

```cpp
      String msg = "🛢️ Oil tank monitor is ONLINE.\nSensor: ";
      switch (cfgSensorType) {
        case SENSOR_TOF:      msg += "ToF"; break;
        case SENSOR_IR_BREAK: msg += "IR break-beam (sight gauge)"; break;
        default:              msg += "Digital"; break;
      }
```

Replace with:

```cpp
      String msg = "🛢️ Oil tank monitor is ONLINE.\nSensor: ";
      msg += sensorTypeDisplayName(cfgSensorType);
```

- [ ] **Step 5: Compile**

```
arduino-cli compile --fqbn esp32:esp32:esp32 /home/juliettango/oil-monitor/OilTankMonitor/OilTankMonitor.ino
```

Expected: clean compile with no warnings about the helpers. Sketch size should drop slightly versus the pre-task baseline of `1119464` bytes (post-Sub-project B). Expect a delta of −100 to −500 bytes. Record the new value.

- [ ] **Step 6: Upload**

```
arduino-cli upload --fqbn esp32:esp32:esp32 -p /dev/ttyUSB0 /home/juliettango/oil-monitor/OilTankMonitor/OilTankMonitor.ino
```

Expected: `Hard resetting via RTS pin...` and `New upload port: /dev/ttyUSB0 (serial)`.

- [ ] **Step 7: Verify boot serial summary is byte-identical**

Capture 20 s of boot output:

```
python3 -c "
import serial, time
s = serial.Serial('/dev/ttyUSB0', 115200, timeout=0.05)
s.dtr = False; s.rts = True; time.sleep(0.2); s.rts = False
buf = bytearray()
end = time.time() + 20
while time.time() < end:
    chunk = s.read(8192)
    if chunk: buf.extend(chunk)
print(buf.decode('utf-8', errors='replace'))"
```

Expected: the boot summary line must read **exactly**:

```
Sensor type: TOF | ToF thresholds (mm): low=200 half=130 high=60
```

(Or `DIGITAL` / `IR_BREAK` as the first token, depending on the saved `cfgSensorType`. The current device has it on TOF.) If the line reads `Sensor type: tof` (lowercase) or `Sensor type: ToF` (mixed case), the wrong helper was wired into the boot path — go back to Step 3.

- [ ] **Step 8: Verify `/status` JSON is byte-identical**

```
curl -s http://192.168.0.241/status | python3 -m json.tool
```

Expected: the response contains `"sensor_type": "tof"` (lowercase). If it reads `"TOF"` or `"ToF"`, the wrong helper was wired into `handleStatus()` — go back to Step 2.

- [ ] **Step 9: Verify ONLINE Telegram message is byte-identical**

The boot in Step 7 already triggered an ONLINE Telegram. Ask the user to confirm their Telegram chat shows a message starting:

```
🛢️ Oil tank monitor is ONLINE.
Sensor: ToF
Level: ...
```

(Or `Sensor: Digital` / `Sensor: IR break-beam (sight gauge)` per saved config.) If it reads `Sensor: TOF` (uppercase) or `Sensor: tof` (lowercase), the wrong helper was wired into the ONLINE block — go back to Step 4.

- [ ] **Step 10: Commit**

```
git add OilTankMonitor/OilTankMonitor.ino
git commit -m "$(cat <<'EOF'
Consolidate sensor-type label switches into three helpers

Three inline switch blocks mapped cfgSensorType to display strings,
each with a different vocabulary. Replaces them with file-local
helpers — sensorTypeJsonName, sensorTypeBootName, sensorTypeDisplayName
— modeled on the existing levelStateName helper.

Every user-visible string is byte-for-byte unchanged. handleStatus
no longer wraps its result in String(...), dropping one heap
allocation per /status request.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Self-Review

**Spec coverage**
- "Three helpers, one per vocabulary" → Step 1.
- "Each takes `SensorType`, returns `const char*`, lives near the enum" → Step 1, placed after `levelStateName()` per the design rationale (matching the existing helper pattern).
- "Default arm preserves digital fallback" → Step 1, all three helpers `default: return "digital"/"DIGITAL"/"Digital"`.
- "handleStatus call site rewrite" → Step 2.
- "Boot serial call site rewrite" → Step 3.
- "ONLINE Telegram call site rewrite" → Step 4.
- "No user-visible string changes" → Steps 7, 8, 9 verify each vocabulary's call site preserves exact wording.
- "No README change" → not in plan; correct.

**Placeholder scan**: every code change shows the actual code. Verification steps name the exact expected strings. No "verify normal operation" hand-waves.

**Type consistency**: helper names appear identically in their definitions (Step 1) and in their three call sites (Steps 2, 3, 4). All three return `const char*`, take a single `SensorType` parameter.

No issues found.
