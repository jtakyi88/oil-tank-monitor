# Drop VL53L0X Library Support — Design

**Date**: 2026-04-30
**Files touched**: `OilTankMonitor/OilTankMonitor.ino`, `README.md`
**Sub-project of**: Production-readiness cleanup pass (A → B → C → D)

## Background

The firmware currently auto-detects between VL53L0X and VL53L1X ToF sensors at boot. This was added during the dual-chip ToF support work (2026-04-29) when "support both" seemed flexible. In practice:

- The user's only ToF hardware is a TOF400C breakout (VL53L1X chip).
- No VL53L0X hardware exists in the install.
- The `Adafruit_VL53L0X` library is linked into the firmware purely on the off-chance the user buys VL53L0X hardware later — pure YAGNI.

Linking the unused library costs flash and adds two failure-mode branches (one in detection, one in read dispatch) that can never fire on the user's actual hardware.

## Goal

Remove all VL53L0X support from the firmware. Reduce flash footprint by 5–15 KB and simplify the ToF code path to a single chip.

## Non-Goals

- Removing the `TofChip` enum infrastructure entirely (kept for forward extensibility — adding a third chip in the future stays cheap).
- Changing the user-facing `cfgSensorType` enum (`SENSOR_TOF` continues to mean "use ToF mode" — chip choice is internal).
- NVS migration (no NVS field references VL53L0X — no migration needed).

## Changes

### `OilTankMonitor/OilTankMonitor.ino`

| Item | Action |
|------|--------|
| `#include <Adafruit_VL53L0X.h>` | Remove |
| `Adafruit_VL53L0X tofL0x;` global | Remove |
| `enum TofChip { TOF_NONE, TOF_VL53L0X, TOF_VL53L1X };` | Change to `enum TofChip { TOF_NONE, TOF_VL53L1X };` |
| `initTof()` VL53L0X fallback branch | Remove. Function becomes simpler — just probe VL53L1X, return success/failure. |
| `readSensorRaw()` VL53L0X dispatch branch | Remove. Function dispatches only on `TOF_VL53L1X` (and falls through to invalid for `TOF_NONE`). |
| `handleStatus()` `tof_chip` switch | Remove `case TOF_VL53L0X:` line. Remaining cases: `TOF_VL53L1X` → `"vl53l1x"`, `default` → `"none"`. |
| Boot serial diagnostic in `initTof()` failure | Update text from "neither VL53L1X nor VL53L0X responded" to just "VL53L1X did not respond". |
| ToF init failure Telegram message in `setup()` | Same text update — drop the "neither X nor Y" phrasing in favor of "VL53L1X did not respond". |
| Web UI dropdown label | Change from `"ToF distance (VL53L0X / VL53L1X auto-detect)"` to `"ToF distance (VL53L1X)"`. |

### `README.md`

Remove every mention of VL53L0X. The previous Task 8 (2026-04-29) added dual-chip language across 6 sites in README. This sub-project undoes those edits, leaving only VL53L1X-focused documentation. The wiring section is unchanged (wiring is identical for both chips, but the chip-name labels become VL53L1X-only).

## Edge Cases

- **NVS migration**: not required. `cfgSensorType` values (0/1/2 = digital/tof/ir_break) are preserved. Anyone with a saved `SENSOR_TOF` config continues to work — they just lose the (never-exercised) VL53L0X fallback path.
- **`activeTofChip` enum widening**: `TOF_VL53L0X = 1` would no longer exist, but no NVS field stores `activeTofChip`, so this isn't a backward-compat concern. The enum can be reordered freely.
- **`TOF_MAX_MM`**: stays at 4000 (VL53L1X long mode). The Task 2 review concern about widening the VL53L0X envelope becomes moot since VL53L0X is gone.
- **`tofChipName` default arm in `handleStatus`**: still emits `"none"` for `TOF_NONE`, which is correct when ToF is selected but init failed.

## Testing

Project has no automated test framework. `arduino-cli compile` is the gating check. End-to-end verification:

1. Compile cleanly. Record flash savings (expected: 5–15 KB lower than baseline).
2. Flash to ESP32. Boot ONLINE Telegram message reads as before — `Sensor: ToF`.
3. With VL53L1X connected: `/status` reports `"sensor_type":"tof"`, `"tof_chip":"vl53l1x"`, valid `distance_mm`. Behavior unchanged from current state.
4. With VL53L1X disconnected: failure Telegram reads "VL53L1X did not respond" (new text). `/status` reports `"tof_chip":"none"`.
5. Switch to Digital and IR_BREAK modes via web UI; confirm both still work (regression check).

## Implementation Notes

This sub-project should be done first in the production-readiness pass because:

- It has the lowest risk (pure removal — no behavioral change in the chip we actually use).
- It cleanly reduces flash, freeing budget for sub-projects B–D.
- It removes a code branch that complicates the upcoming String/heap cleanup (sub-project D) by eliminating one of the two read-dispatch paths in `readSensorRaw()`.
