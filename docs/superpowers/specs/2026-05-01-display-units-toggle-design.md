# Display Units Toggle (Metric ↔ US Customary) — Design

**Date**: 2026-05-01
**File touched**: `OilTankMonitor/OilTankMonitor.ino` (+ README)
**Sub-project of**: Post-v2 UX improvements

## Background

The firmware currently expresses every distance in millimeters: ToF threshold inputs and labels in the web UI, the live distance reading widget, the `/status` JSON contract (`distance_mm`, `thresholds.{low,half,high}`), Telegram messages (`(200mm)`), and the boot serial summary. US-based installers thinking in inches must mentally convert on every screen.

Only ToF mode renders distances. Digital and IR break-beam modes have no user-visible measurements, so the toggle has no immediate effect in those modes — but it persists across mode changes.

## Goal

Add a runtime-selectable display-units preference (`Metric (mm)` / `US Customary (inches)`) that drives every distance render and accepts ToF threshold input in the selected unit. Internal storage stays in millimeters — no NVS migration, no conversion drift in the persisted state.

## Non-Goals

- No support for other measurement systems (no centimeters, feet, mils).
- No change to digital or IR break-beam modes (no measurements to display).
- No timezone, language, or currency localization. Strictly distance.
- No per-user preference. The toggle is a device-wide setting.
- No backward-incompatible JSON change. `distance_mm` and `thresholds.{low,half,high}` stay millimeter-valued and always present.

## Architecture

### Storage

```cpp
enum Units {
  UNITS_METRIC = 0,
  UNITS_IMPERIAL = 1
};
Units cfgUnits = UNITS_METRIC;
```

NVS key: `units` (uint8). Default on first boot or missing key: `UNITS_METRIC` — preserves current behavior on existing devices flashed with this firmware.

ToF thresholds (`cfgTofLow/Half/High`) remain stored in millimeters. Conversion happens only at the display and input layers.

### Conversion helpers

Three file-local helpers live near `levelStateName()` and the existing `sensorTypeJsonName()` family:

```cpp
const char* unitsLabel(Units u);           // "mm" or "in"
String formatDistance(uint16_t mm);        // "200" or "7.87" per cfgUnits
bool parseDistanceInput(const String& s,   // accepts mm or inches per cfgUnits
                        uint16_t& outMm);  // returns false on parse failure
```

**Conversion math**:
- mm → inches: `mm / 25.4`, rendered with 2 decimal places (`%.2f`)
- inches → mm: `round(inches * 25.4)`, stored as uint16_t

**Round-trip stability**: 2-decimal inches (`0.01"` ≈ `0.254 mm`) is finer than 1 mm storage resolution, so any 2-decimal inch input maps to exactly one stored mm value, and that mm value re-renders to the same 2-decimal inch string. No drift across re-edits.

### Web UI

A new `<select>` field is inserted at the top of the **Sensor Configuration** section, immediately above the Sensor Type dropdown:

```
Display Units
[ Metric (mm) ▼ ]
   Metric (mm)
   US Customary (inches)
```

POST field name: `units`. Saved by `handleSave()` alongside other config.

When `cfgUnits == UNITS_IMPERIAL`:
- Threshold input labels read `"LOW threshold (in) — alert below this"`, `"HALF threshold (in)"`, `"HIGH threshold (in) — refill complete above this"`
- Threshold input values are pre-filled with the stored mm value converted to 2-decimal inches
- Range hint reads: `Range: 30–2000 mm (1.18–78.74 in). Smaller value = puck closer to sensor (fuller tank). Must satisfy HIGH < HALF < LOW.`
- Live reading widget renders `Current Reading: 7.87 in`

When `cfgUnits == UNITS_METRIC` (default), all of the above retain their current mm-based wording.

The range hint always shows both unit ranges so the user has a sanity reference regardless of mode.

### Client-side dropdown handler

To avoid a footgun where the user flips the units dropdown but the threshold input boxes still show their old values (`200`), a small JS handler converts the three input values in place when the dropdown changes:

```js
function convertUnits() {
  var u = document.getElementById('units').value;          // '0' = metric, '1' = imperial
  ['tof_low','tof_half','tof_high'].forEach(function(id){
    var f = document.getElementById(id);
    var v = parseFloat(f.value);
    if (isNaN(v)) return;
    f.value = (u === '1') ? (v / 25.4).toFixed(2) : Math.round(v * 25.4).toString();
  });
  // Update labels and range hint visibility (toggle '.metric-only' / '.imperial-only' classes).
}
```

The threshold labels and range hint use class-based show/hide (`.metric-only` and `.imperial-only`) toggled by the same handler, so the user sees `(in)` labels immediately on dropdown change, not after submit.

### Form processing

`handleSave()` reads `cfgUnits` first (POSTed as `"0"` or `"1"`). Then for each ToF threshold field, it uses `parseDistanceInput()` against the *new* (just-set) `cfgUnits` value — i.e., a request that flips `units` from metric to imperial and submits inch-valued thresholds in the same POST is parsed correctly. With the client-side handler above, the typical user flow is: change dropdown → input values auto-convert → save with already-correct values.

Validation order:
1. Parse `units` and stage to `newUnits`.
2. Parse each threshold using `newUnits` to determine input mode.
3. Validate the resulting mm values against the existing 30–2000 mm range and HIGH < HALF < LOW ordering. Validation messages show both units (e.g., `"Each ToF threshold must be between 30 mm (1.18 in) and 2000 mm (78.74 in)."`).
4. Commit `cfgUnits` and the three thresholds together (preserving the existing staged-write pattern that prevents in-memory corruption on validation failure).

### `/status` JSON contract

Adds one new field, no removals:

```json
{
  "...existing fields...": "...",
  "units": "metric",          // or "imperial"
  "sensor_type": "tof",
  "distance_mm": 200,         // unchanged: always millimeters
  "thresholds": {             // unchanged: always millimeters
    "low": 200,
    "half": 130,
    "high": 60
  },
  "tof_chip": "vl53l1x"
}
```

Rationale: the only consumer of `/status` is the inline JS at line 452 of the firmware (the live distance widget). We control both ends. Adding a parallel `*_in` set of fields would double the contract for one consumer; instead the JS reads `units` and converts. External monitoring scripts (if any are added later) can do the same one-line conversion.

The web UI's poll JS becomes:

```js
fetch('/status').then(r=>r.json()).then(j=>{
  var el = document.getElementById('tof-distance');
  if (!el || j.distance_mm === undefined) return;
  if (j.units === 'imperial') {
    el.textContent = (j.distance_mm / 25.4).toFixed(2) + ' in';
  } else {
    el.textContent = j.distance_mm + ' mm';
  }
});
```

### Telegram messages

The trailing distance suffix (currently `" (" + String(r.distanceMm) + "mm)"`) becomes:

```cpp
msg += " (" + formatDistance(r.distanceMm) + " " + unitsLabel(cfgUnits) + ")";
```

Renders as `(200 mm)` or `(7.87 in)`. Note the added space before the unit suffix — matches conventional formatting (`200 mm`, not `200mm`). This is a minor cosmetic change to the metric output too; documented in the PR.

### Boot serial summary

Currently:
```
Sensor type: TOF | ToF thresholds (mm): low=200 half=130 high=60
```

Becomes (metric):
```
Sensor type: TOF | ToF thresholds: low=200 mm half=130 mm high=60 mm
```

Or (imperial):
```
Sensor type: TOF | ToF thresholds: low=7.87 in half=5.12 in high=2.36 in
```

Implementation uses `formatDistance()` + `unitsLabel()` per threshold, replacing the current static `(mm)` header.

### README

The "Choosing a Sensor" section gets a brief mention of the units toggle. The ToF wiring/operation paragraph notes that the firmware accepts threshold input in either mm or inches per the runtime preference.

## Files

One firmware file: `OilTankMonitor/OilTankMonitor.ino`. One docs file: `README.md`. No new files, no new libraries.

Expected flash delta: +400 to +800 bytes (added enum, NVS load/save, three helpers, conversion math, two extra UI labels and JS branch).

## Output Equivalence

When `cfgUnits == UNITS_METRIC`, output should be byte-identical to current behavior except:
- Telegram suffix gains a space (`200mm` → `200 mm`).
- Boot serial summary moves the unit token from a header `(mm)` to a per-value suffix.

These are intentional changes documented above. All other metric-mode renders (UI labels, JSON, validation messages, range hint) must match the current strings exactly.

When `cfgUnits == UNITS_IMPERIAL`, all unit-bearing renders use inches per the rules above.

## Edge Cases

- **Existing devices flashed with this firmware**: `cfgUnits` key absent from NVS → defaults to `UNITS_METRIC` → all surfaces render exactly as before until the user opts in.
- **POST with `units=` flipped + inch-valued thresholds in the same request**: validation parses thresholds against the *new* unit setting (see "Form processing"). Single round-trip switch works.
- **POST with `units=` flipped to imperial but threshold values still in mm digits**: e.g., user changes the dropdown but forgets the inputs are mm. Their `200` parses as `200 in = 5080 mm`, fails the 30–2000 mm range check, validation error renders with both-unit messaging. No silent corruption.
- **Imperial input with > 2 decimals**: `parseDistanceInput` accepts any float, rounds to nearest mm. The displayed value on next render is the rounded value's 2-decimal inches representation — may differ slightly from what the user typed (`7.875 → 200 mm → 7.87 in` displayed). Acceptable: the stored value is exactly what the firmware uses.
- **Negative or non-numeric input**: `parseDistanceInput` returns false → validation message "Each ToF threshold must be between 30 mm (1.18 in) and 2000 mm (78.74 in)." Same error path as today's range check.
- **Digital / IR break-beam modes**: `cfgUnits` has no rendered effect (no distances exist). The dropdown is still visible and saveable. If the user later switches to ToF, the preference is in effect.
- **Telegram boot ONLINE message**: doesn't currently include distance, so no change needed there.

## Testing

Manual hardware verification (no automated tests exist):

1. **Pre-change capture (metric mode)**:
   - `curl -sL http://192.168.0.241/ -o /tmp/before.html` (with auth cookie)
   - `curl -s http://192.168.0.241/status -o /tmp/before-status.json`
2. **Compile and flash**.
3. **Default-state verification (metric)**:
   - `curl ... > /tmp/after-metric.html`; `diff /tmp/before.html /tmp/after-metric.html` should show ONLY: the new `Display Units` field added at top of Sensor Configuration.
   - `/status` JSON gains `units: "metric"`; all other fields byte-identical.
4. **Switch to imperial via UI**:
   - Confirm threshold inputs re-render with inch values (`200 → 7.87`, `130 → 5.12`, `60 → 2.36`).
   - Labels read `(in)`. Range hint shows both units.
   - Save with values unchanged. Confirm the saved page displays, then config page re-renders with the same inch values (round-trip stable).
   - Live reading widget shows distance in inches with 2 decimals.
   - `/status` JSON: `units: "imperial"`, `distance_mm` and `thresholds.{}` still in mm.
5. **Trigger a state change** (cover the puck or move the sensor) so the firmware emits a Telegram. Confirm the suffix is `(7.87 in)` not `(200mm)`.
6. **Boot reset** and confirm serial summary reads `ToF thresholds: low=7.87 in half=5.12 in high=2.36 in`.
7. **Switch back to metric**, confirm everything reverts including Telegram suffix.
8. **Validation paths**:
   - Submit imperial input out of range (e.g., `100 in` → 2540 mm > 2000 mm). Confirm error message shows both units.
   - Submit imperial input violating ordering. Confirm message shows the imperial-form values.
9. **Cross-mode persistence**: switch to digital sensor, save, switch back to ToF. `cfgUnits` should still be in effect.

## Implementation Notes

The plan should split this into roughly three tasks:

1. **U-1: Storage + helpers + boot/Telegram surfaces.** Add `Units` enum, `cfgUnits` global, NVS load/save, three conversion helpers, and update the boot serial summary + Telegram distance suffix. No web UI changes yet. Verifiable by switching units via NVS preload (or just compile-test) and observing serial output. Compile-only commit acceptable since the toggle isn't wired through the UI yet — but the metric-default path must produce the documented serial change.

2. **U-2: Web UI dropdown + threshold input/output.** Add the `Display Units` dropdown to the Sensor Configuration section. Update threshold input labels, range hint, and value pre-fill to switch on `cfgUnits`. Wire `parseDistanceInput()` into `handleSave()` and update validation messages. Update the live reading JS to convert client-side. Update `/status` JSON to include `units`. Verifiable end-to-end via UI walkthrough + curl diffs.

3. **U-3: README documentation + hardware verification.** Brief note in README about the units toggle. Run the full test plan above. No firmware change.
