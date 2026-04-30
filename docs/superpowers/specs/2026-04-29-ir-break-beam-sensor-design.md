# IR Break-Beam Sensor Mode — Design

**Date**: 2026-04-29
**File touched**: `OilTankMonitor/OilTankMonitor.ino` (single-file project)
**Companion to**: `2026-04-28-interchangeable-sensors-design.md`

## Background

The firmware currently supports two sensor modes:

- `SENSOR_DIGITAL` (XKC-Y25-V capacitive level sensor): pin HIGH = liquid present
- `SENSOR_TOF` (VL53L0X): distance reading bucketed into LOW/BELOW_HALF/ABOVE_HALF/HIGH

Users now want to use an inexpensive IR break-beam emitter/receiver pair mounted across a sight-gauge tube. A floating puck inside the gauge descends as oil drops; when the puck reaches the beam height, it blocks the beam → triggers low-oil alert.

`SENSOR_DIGITAL` *can* technically read the receiver, but with two semantic problems:

1. Polarity is inverted (IR receivers typically output HIGH when beam is detected; firmware treats HIGH as "liquid present").
2. State names and Telegram alert text are framed around "liquid detected / not detected", which makes alerts confusing for sight-gauge use.

## Goal

Add a third sensor type, `SENSOR_IR_BREAK`, with correct polarity for sight-gauge puck use and dedicated state names + Telegram messages that read sensibly to a non-engineer reading the alert at 2 AM.

## Non-Goals

- Heartbeat / "sensor still alive" alerting (separate feature).
- Multi-sensor fusion (e.g. ToF + IR break-beam combined).
- Configurable polarity inversion as a generic toggle (rejected as Option 1 during brainstorming — not enough semantic improvement to justify the UI noise).
- Per-sensor "alert direction" generalization (rejected as Option 3 during brainstorming — over-scoped).

## Behavior

### Wiring

- IR emitter (red+black wires): red → 3V3 (or VIN if module needs 5V), black → GND. No signal line.
- IR receiver (red+black+white wires): red → 3V3 (or VIN), black → GND, white → **GPIO 4** (`SENSOR_PIN`, shared with the existing digital sensor — only one is connected at a time).
- Pin mode: `INPUT_PULLUP`.

### Polarity

After the pull-up:
- Pin HIGH → beam clear → `LEVEL_OIL_OK`
- Pin LOW → beam broken (puck at mark) → `LEVEL_OIL_LOW`

### Debounce

Reuses the existing digital debounce path used by `SENSOR_DIGITAL`: three (`DEBOUNCE_COUNT`) consecutive matching raw reads across successive `readSensor()` calls before the accepted state changes. Until stable, the previously-accepted reading is returned. This filters single-sample noise from puck wobble or beam jitter.

### Validity

`readSensorRaw()` always returns `valid=true` for IR_BREAK. A digital pin has no "out-of-range" failure mode equivalent to ToF range errors. (See "Known limitation" below.)

## State Model

Add two values to `LevelState`:

```cpp
enum LevelState {
  LEVEL_LOW,           // existing — digital low / ToF distance > tofLow
  LEVEL_BELOW_HALF,    // existing — ToF only
  LEVEL_ABOVE_HALF,    // existing — ToF only
  LEVEL_HIGH,          // existing — digital high / ToF distance <= tofHigh
  LEVEL_OIL_OK,        // NEW — IR break-beam: beam clear (puck above mark)
  LEVEL_OIL_LOW,       // NEW — IR break-beam: beam broken (puck at mark)
  LEVEL_UNKNOWN        // existing
};
```

`levelStateName()` extends with cases returning `"OIL_OK"` and `"OIL_LOW"`.

Each sensor mode uses its own subset of states; no mode mixes them. A `SENSOR_IR_BREAK` reading will never produce `LEVEL_LOW` or `LEVEL_HIGH`, and a `SENSOR_DIGITAL` reading will never produce `LEVEL_OIL_*`.

## Configuration

### `SensorType` enum

```cpp
enum SensorType {
  SENSOR_DIGITAL  = 0,
  SENSOR_TOF      = 1,
  SENSOR_IR_BREAK = 2,   // NEW
};
```

### NVS storage

`cfgSensorType` is already persisted as a `uint8_t`. Adding value `2` is forward-compatible:
- Existing configs (0 or 1) continue to load correctly.
- Configs written by the new firmware and then booted on old firmware would fall back to digital — acceptable since downgrade is not a supported flow.

No migration step required.

### Web UI (`handleRoot`, `handleSave`)

`handleRoot`: dropdown gains a third `<option value='2'>IR break-beam (sight gauge puck)</option>`. ToF threshold input fields stay hidden when `cfgSensorType == SENSOR_IR_BREAK`, mirroring the existing hide-when-digital behavior.

`handleSave`: parses `t == 2` and stores `SENSOR_IR_BREAK`. Re-runs `initSensor()` so the new pin mode takes effect without a reboot.

### `/status` JSON (`handleStatus`)

- `"sensor_type"` gains `"ir_break"` as a possible value.
- New optional field `"beam_state"`: `"clear"` or `"broken"`. Present only when `cfgSensorType == SENSOR_IR_BREAK`. Other fields (`distance_mm`, ToF thresholds) are omitted in IR mode.

## Telegram Messages

In `SENSOR_IR_BREAK` mode:

| Trigger | Message |
|---------|---------|
| Boot, after WiFi connects | `🛢️ Oil tank monitor is ONLINE.\nSensor: IR break-beam (sight gauge)\nState: <OIL_OK\|OIL_LOW>\nSettings: http://<ip>` |
| Transition to `LEVEL_OIL_LOW` | `⚠️ Low oil — sight-gauge puck has reached the low-oil mark. Please refill.\nSettings: http://<ip>` |
| Repeated alert while in `LEVEL_OIL_LOW` | Same message, sent **once per hour** while the condition persists (matches existing digital cadence). |
| Transition back to `LEVEL_OIL_OK` | `✅ Oil level restored — puck is above the low-oil mark.` |

The existing alert dispatcher in `loop()` keys off level-state transitions and tracks "last alert time" for cadence. New IR_BREAK message strings plug into the same dispatcher; no parallel state machine.

## Code Touchpoints

All changes in `OilTankMonitor/OilTankMonitor.ino`. Line numbers are pre-edit; they will shift.

| Location | Change |
|----------|--------|
| `enum SensorType` (~line 30) | Add `SENSOR_IR_BREAK = 2` |
| `enum LevelState` (~line 38) | Add `LEVEL_OIL_OK`, `LEVEL_OIL_LOW` |
| `levelStateName()` (~line 46) | Two new cases returning `"OIL_OK"`, `"OIL_LOW"` |
| `initSensor()` (~line 779) | IR_BREAK branch: `pinMode(SENSOR_PIN, INPUT_PULLUP)` + serial log |
| `readSensorRaw()` (~line 795) | IR_BREAK branch: read pin, set `digitalState`, `valid=true` |
| `readSensor()` debounce (~line 829) | Extend the existing digital debounce check to also cover IR_BREAK |
| `bucketReading()` (~line 865) | IR_BREAK: HIGH → `LEVEL_OIL_OK`, LOW → `LEVEL_OIL_LOW` |
| `fireTransitionMessage()` (~line 908) | New IR_BREAK branch handling `LEVEL_OIL_OK` ↔ `LEVEL_OIL_LOW` transitions with the alert/recovery messages from the Telegram Messages section |
| `loop()` transition tracker (~line 1055) | Extend `if (newState == LEVEL_LOW) lastAlertTime = now;` to also set `lastAlertTime` when `newState == LEVEL_OIL_LOW` |
| `loop()` hourly reminder (~line 1063) | Extend condition from `currentState == LEVEL_LOW` to also cover `LEVEL_OIL_LOW`, with the IR-mode reminder text |
| `loop()` legacy `oilIsLow` (~line 1059) | Set true when `currentState == LEVEL_OIL_LOW` as well, preserving `/status` backward compat |
| `handleRoot()` (~line 297) | Third dropdown option; hide ToF fields when IR mode active |
| `handleSave()` (~line 589) | Accept `t == 2` |
| `handleStatus()` (~line 650) | Emit `"ir_break"`, optional `beam_state` field |
| `setup()` ONLINE message (~line 972) | IR_BREAK message variant |

## Edge Cases

- **Live sensor type change**: `handleSave()` re-runs `initSensor()`. IR_BREAK init is just a `pinMode` call — safe to re-run.
- **Boot with puck already at the mark**: `setup()` takes an initial reading before sending the ONLINE message, so the boot Telegram correctly reports `OIL_LOW`. The alert dispatcher then sends the low-oil alert on the first state-transition tick.
- **Slow puck oscillation across the beam**: 3-read debounce filters single-sample flips. If the puck genuinely oscillates over many seconds, repeated alert/recovery pairs are acceptable — the oil really is at threshold.
- **Sensor type changed while in IR mode**: existing transition logic resets `currentState` to `LEVEL_UNKNOWN` and a new initial reading is taken; safe.

## Known Limitations

- **Disconnected receiver wire** reads HIGH (pulled up) → looks like `LEVEL_OIL_OK`. A broken sensor wire is indistinguishable from a healthy "everything fine" state. The existing `SENSOR_DIGITAL` mode has the same limitation. Adding heartbeat / liveness alerting is explicitly out of scope.
- **5V emitters with 3V3 receivers**: documented in wiring guidance, but not enforced or detected by firmware.

## Testing

Project has no automated tests. Manual verification plan:

1. `arduino-cli compile --fqbn esp32:esp32:esp32 OilTankMonitor.ino` — compiles cleanly.
2. Flash to the ESP32. Web UI at `http://<ip>` shows the new "IR break-beam (sight gauge puck)" option in the sensor type dropdown.
3. Wire up the IR pair, select IR mode in the UI, save.
4. With beam clear: `/status` reports `"sensor_type":"ir_break"`, `"beam_state":"clear"`, `"level":"OIL_OK"`. Boot Telegram ONLINE message matches.
5. Block beam by hand: within ~3 seconds, `/status` flips to `"beam_state":"broken"`, `"level":"OIL_LOW"`. Telegram low-oil alert arrives.
6. Hold beam blocked for >60 minutes: second alert fires, confirming hourly cadence.
7. Unblock beam: recovery message arrives; alert state clears.
8. Switch back to digital mode via web UI; confirm digital sensor behavior is unchanged.
9. Reboot with beam blocked: ONLINE message reports `OIL_LOW` directly; alert dispatcher fires.
