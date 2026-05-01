# Display Units Toggle Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a runtime-selectable display-units preference (Metric / US Customary) that drives every distance render in the firmware and accepts ToF threshold input in the selected unit, without changing internal mm-based storage.

**Architecture:** New `Units` enum + `cfgUnits` global persisted in NVS as `units` (uint8). Three file-local helpers — `unitsLabel()`, `formatDistance()`, `parseDistanceInput()` — sit next to `levelStateName()` / `sensorTypeJsonName()` / etc. and are called from the boot serial summary, Telegram messages, web UI render, web JS, `/status` JSON, and `handleSave()`. Storage stays in mm; only display and form-input layers convert.

**Tech Stack:** Arduino-ESP32 (single `.ino` sketch), `arduino-cli` for compile/upload, manual hardware verification via curl + serial. No test framework.

Spec: `docs/superpowers/specs/2026-05-01-display-units-toggle-design.md`.

---

## File Structure

| File | Responsibility | Change scope |
|------|----------------|--------------|
| `OilTankMonitor/OilTankMonitor.ino` | All firmware behavior | Task 1: enum + global + NVS load/save + 3 helpers + boot serial + Telegram suffix. Task 2: web UI dropdown + threshold inputs/labels + form processing + validation + `/status` JSON + live-reading JS. |
| `README.md` | User docs | Task 3: brief mention of the units toggle in the "Choosing a Sensor" / ToF wiring sections. |

---

## Pre-Task Setup (do once before Task 1)

- [ ] **Step 0a: Capture current metric-mode HTML and JSON for diff comparisons later**

The device currently runs `cfgUnits == UNITS_METRIC` (because the field doesn't exist yet — every render is metric). Save these baselines so the byte-equivalence checks in Task 2's verification have something to diff against.

```
curl -s -c /tmp/cookies.txt -d "password=admin" http://192.168.0.241/login -o /dev/null
curl -sL -b /tmp/cookies.txt http://192.168.0.241/ -o /tmp/units-before.html
curl -s -b /tmp/cookies.txt http://192.168.0.241/status -o /tmp/units-before-status.json
```

Confirm:
```
wc -c /tmp/units-before.html  # should be ~8000 bytes
python3 -m json.tool /tmp/units-before-status.json  # should print formatted JSON with distance_mm / thresholds.{low,half,high}
```

If `192.168.0.241` is not reachable, ask the user for the device's current IP.

- [ ] **Step 0b: Record the pre-change sketch size**

```
arduino-cli compile --fqbn esp32:esp32:esp32 /home/juliettango/oil-monitor/OilTankMonitor 2>&1 | tail -3
```

Expected: `Sketch uses 1117048 bytes (85%) of program storage space.` (or close — within ±200 bytes from the dropdown-cleanup commit `8931ccc`). Record the exact number.

---

### Task 1: Storage + helpers + boot/Telegram surfaces

**Files:**
- Modify: `OilTankMonitor/OilTankMonitor.ino` (enum near line 32, helpers near line 90, NVS load/save lines 204/226, boot serial line 1152, Telegram suffix line 1180)

This task lands the type, NVS persistence, and helpers, and uses them in two non-UI surfaces (boot serial + Telegram). The web UI is untouched in this task — `cfgUnits` stays at its NVS default (`UNITS_METRIC`), so all rendered HTML, JSON, and validation messages remain byte-identical to before.

- [ ] **Step 1: Add `Units` enum**

Locate the existing enum block (lines 31–32):

```cpp
enum SensorType { SENSOR_DIGITAL = 0, SENSOR_TOF = 1, SENSOR_IR_BREAK = 2 };
enum TofChip { TOF_NONE, TOF_VL53L1X };
```

Insert immediately after them:

```cpp
enum Units { UNITS_METRIC = 0, UNITS_IMPERIAL = 1 };
```

- [ ] **Step 2: Add `cfgUnits` global**

Locate `cfgSensorType` declaration (line 137):

```cpp
SensorType cfgSensorType = SENSOR_DIGITAL;   // default — protects v1.x upgraders
```

Insert immediately after it:

```cpp
Units cfgUnits = UNITS_METRIC;               // default — preserves pre-toggle output
```

- [ ] **Step 3: Wire NVS load**

Locate the `prefs.getUShort("tof_high", 60);` line (line 207):

```cpp
  cfgTofHigh    = prefs.getUShort("tof_high", 60);
```

Insert the new key immediately after it (still inside the `prefs.begin("oilmon", true)` block, before `prefs.end();`):

```cpp
  cfgUnits      = (Units)prefs.getUChar("units", 0);
```

- [ ] **Step 4: Wire NVS save**

Locate the `prefs.putUShort("tof_high", cfgTofHigh);` line (line 229):

```cpp
  prefs.putUShort("tof_high", cfgTofHigh);
```

Insert the new key immediately after it (still inside the `prefs.begin("oilmon", false)` block, before `prefs.end();`):

```cpp
  prefs.putUChar("units", (uint8_t)cfgUnits);
```

- [ ] **Step 5: Add the three conversion helpers**

Locate `sensorTypeDisplayName()` (lines 79–87):

```cpp
const char* sensorTypeDisplayName(SensorType t) {
  switch (t) {
    case SENSOR_TOF:      return "ToF";
    case SENSOR_IR_BREAK: return "IR break-beam (sight gauge)";
    default:              return "Digital";
  }
}
```

Insert immediately after its closing brace:

```cpp
const char* unitsLabel(Units u) {
  return (u == UNITS_IMPERIAL) ? "in" : "mm";
}

String formatDistance(uint16_t mm) {
  if (cfgUnits == UNITS_IMPERIAL) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%.2f", mm / 25.4f);
    return String(buf);
  }
  return String(mm);
}

bool parseDistanceInput(const String& s, uint16_t& outMm) {
  if (s.length() == 0) return false;
  float v = s.toFloat();
  if (v <= 0.0f) return false;
  if (cfgUnits == UNITS_IMPERIAL) {
    long mm = lroundf(v * 25.4f);
    if (mm < 0 || mm > 65535) return false;
    outMm = (uint16_t)mm;
  } else {
    long mm = (long)v;
    if (mm < 0 || mm > 65535) return false;
    outMm = (uint16_t)mm;
  }
  return true;
}
```

Note: helpers read the *file-scope* `cfgUnits`, so callers don't need to pass it in. This matches the pattern of `sensorTypeJsonName(cfgSensorType)` etc. — explicit param where it varies, implicit globals for device state.

- [ ] **Step 6: Update boot serial summary**

Locate the `Serial.printf` call at line 1152:

```cpp
  Serial.printf("Sensor type: %s | ToF thresholds (mm): low=%u half=%u high=%u\n",
                sensorTypeBootName(cfgSensorType),
                cfgTofLow, cfgTofHalf, cfgTofHigh);
```

Replace with:

```cpp
  String tlow  = formatDistance(cfgTofLow);
  String thalf = formatDistance(cfgTofHalf);
  String thigh = formatDistance(cfgTofHigh);
  const char* u = unitsLabel(cfgUnits);
  Serial.printf("Sensor type: %s | ToF thresholds: low=%s %s half=%s %s high=%s %s\n",
                sensorTypeBootName(cfgSensorType),
                tlow.c_str(),  u,
                thalf.c_str(), u,
                thigh.c_str(), u);
```

This is one of the two intentional output-format changes per the spec (boot serial summary moves the unit token from a header `(mm)` to a per-value suffix). Default-metric output becomes:

```
Sensor type: TOF | ToF thresholds: low=200 mm half=130 mm high=60 mm
```

- [ ] **Step 7: Update Telegram distance suffix**

Locate the line that appends the mm suffix (line 1180):

```cpp
        msg += " (" + String(r.distanceMm) + "mm)";
```

Replace with:

```cpp
        msg += " (" + formatDistance(r.distanceMm) + " " + unitsLabel(cfgUnits) + ")";
```

This is the second intentional output-format change (the metric form gains a space: `200mm` → `200 mm`). Imperial form becomes `(7.87 in)`.

- [ ] **Step 8: Compile**

```
arduino-cli compile --fqbn esp32:esp32:esp32 /home/juliettango/oil-monitor/OilTankMonitor 2>&1 | tail -3
```

Expected: clean compile. Sketch size delta from the pre-task baseline: **+200 to +600 bytes** (new helpers + boot summary expansion). If it grows by more than 1000 bytes, something is off — investigate before flashing.

- [ ] **Step 9: Flash**

```
arduino-cli upload --fqbn esp32:esp32:esp32 -p /dev/ttyUSB0 /home/juliettango/oil-monitor/OilTankMonitor
```

Expected: `Hard resetting via RTS pin...` and `New upload port: /dev/ttyUSB0 (serial)`.

- [ ] **Step 10: Verify boot serial summary in metric form**

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

Expected: the boot summary line reads **exactly**:

```
Sensor type: TOF | ToF thresholds: low=200 mm half=130 mm high=60 mm
```

(or `DIGITAL` / `IR_BREAK` per saved `cfgSensorType`). If you see anything other than `low=NNN mm half=NNN mm high=NNN mm` in metric mode, the helper math or printf format is wrong — go back to Step 5/6.

- [ ] **Step 11: Verify Telegram suffix in metric form**

Trigger a state transition (cover/uncover the puck if the sensor is reachable, or temporarily move a magnet for digital mode). Confirm the resulting Telegram message ends with `(NNN mm)` not `(NNNmm)`. If the sensor is not in a state where a transition is easy, ask the user to confirm the next genuine alert renders with the space.

If the user can't trigger a transition, an alternative quick check: search live serial output for the next ALERT or LEVEL transition line — the corresponding Telegram payload format is logged.

- [ ] **Step 12: Verify metric-mode HTML/JSON unchanged**

Re-capture the page:

```
curl -s -c /tmp/cookies.txt -d "password=admin" http://192.168.0.241/login -o /dev/null
curl -sL -b /tmp/cookies.txt http://192.168.0.241/ -o /tmp/units-after-task1.html
curl -s -b /tmp/cookies.txt http://192.168.0.241/status -o /tmp/units-after-task1.json
diff /tmp/units-before.html /tmp/units-after-task1.html
diff /tmp/units-before-status.json /tmp/units-after-task1.json
```

Expected: **both diffs empty.** Task 1 doesn't touch the web UI or `/status` JSON. If either diff is non-empty, you accidentally edited a render path — find and revert.

- [ ] **Step 13: Commit**

```
git add OilTankMonitor/OilTankMonitor.ino
git commit -m "$(cat <<'EOF'
Add Units enum, cfgUnits NVS field, and conversion helpers

Plumbs the metric/imperial preference through storage and the two
non-UI render surfaces — boot serial summary and Telegram distance
suffix. Web UI is untouched in this commit; cfgUnits defaults to
UNITS_METRIC on first load so existing devices render exactly as
before.

Boot serial format changed: "ToF thresholds (mm): low=200 ..." →
"ToF thresholds: low=200 mm ...". Telegram suffix changed:
"(200mm)" → "(200 mm)". Both are documented intentional changes
per the spec.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: Web UI dropdown + threshold input/output + `/status` JSON

**Files:**
- Modify: `OilTankMonitor/OilTankMonitor.ino` (Sensor Configuration section ~line 354, threshold input render ~lines 371–382, validation/parse in `handleSave` ~lines 683–716, `/status` JSON ~line 759, inline JS ~lines 437–462)

This is the single largest task. The streaming pattern means many small `server.sendContent(...)` edits, but each is mechanical. Verification at the end is two diffs: metric-mode bytes (modulo the new `Display Units` field) and full-roundtrip imperial behavior.

- [ ] **Step 1: Add the `Display Units` dropdown to the Sensor Configuration section**

Locate the `<h2>Sensor Configuration</h2>` block (lines 353–357):

```cpp
                       // Sensor Configuration
                       "<h2>Sensor Configuration</h2>"
                       "<label for='sensor_type'>Sensor Type</label>"
                       "<select id='sensor_type' name='sensor_type' onchange='toggleTof()' style='width:100%;padding:10px;background:#16213e;color:#e0e0e0;border:1px solid #333;border-radius:6px;'>"
                       "<option value='0'"));
```

Insert a new `Display Units` block between the `<h2>` and the `<label for='sensor_type'>`:

```cpp
                       // Sensor Configuration
                       "<h2>Sensor Configuration</h2>"
                       "<label for='units'>Display Units</label>"
                       "<select id='units' name='units' onchange='convertUnits()' style='width:100%;padding:10px;background:#16213e;color:#e0e0e0;border:1px solid #333;border-radius:6px;'>"
                       "<option value='0'"));
  if (cfgUnits == UNITS_METRIC) server.sendContent(F(" selected"));
  server.sendContent(F(">Metric (mm)</option>"
                       "<option value='1'"));
  if (cfgUnits == UNITS_IMPERIAL) server.sendContent(F(" selected"));
  server.sendContent(F(">US Customary (inches)</option>"
                       "</select>"
                       "<label for='sensor_type'>Sensor Type</label>"
                       "<select id='sensor_type' name='sensor_type' onchange='toggleTof()' style='width:100%;padding:10px;background:#16213e;color:#e0e0e0;border:1px solid #333;border-radius:6px;'>"
                       "<option value='0'"));
```

- [ ] **Step 2: Replace the three threshold input labels and value renders to use unit-aware text**

Locate the threshold render block (lines 371–383):

```cpp
                       "<label for='tof_low'>LOW threshold (mm) — alert below this</label>"
                       "<input type='text' id='tof_low' name='tof_low' value='"));
  server.sendContent(String(cfgTofLow));
  server.sendContent(F("'>"
                       "<label for='tof_half'>HALF threshold (mm)</label>"
                       "<input type='text' id='tof_half' name='tof_half' value='"));
  server.sendContent(String(cfgTofHalf));
  server.sendContent(F("'>"
                       "<label for='tof_high'>HIGH threshold (mm) — refill complete above this</label>"
                       "<input type='text' id='tof_high' name='tof_high' value='"));
  server.sendContent(String(cfgTofHigh));
  server.sendContent(F("'>"
                       "<p style='font-size:0.85em;color:#999;'>Smaller mm = puck closer to sensor (fuller tank). Must satisfy HIGH &lt; HALF &lt; LOW. Range: 30–2000 mm.</p>"));
```

Replace with:

```cpp
                       "<label for='tof_low'>LOW threshold ("));
  server.sendContent(unitsLabel(cfgUnits));
  server.sendContent(F(") — alert below this</label>"
                       "<input type='text' id='tof_low' name='tof_low' value='"));
  server.sendContent(formatDistance(cfgTofLow));
  server.sendContent(F("'>"
                       "<label for='tof_half'>HALF threshold ("));
  server.sendContent(unitsLabel(cfgUnits));
  server.sendContent(F(")</label>"
                       "<input type='text' id='tof_half' name='tof_half' value='"));
  server.sendContent(formatDistance(cfgTofHalf));
  server.sendContent(F("'>"
                       "<label for='tof_high'>HIGH threshold ("));
  server.sendContent(unitsLabel(cfgUnits));
  server.sendContent(F(") — refill complete above this</label>"
                       "<input type='text' id='tof_high' name='tof_high' value='"));
  server.sendContent(formatDistance(cfgTofHigh));
  server.sendContent(F("'>"
                       "<p style='font-size:0.85em;color:#999;'>Smaller value = puck closer to sensor (fuller tank). Must satisfy HIGH &lt; HALF &lt; LOW. Range: 30–2000 mm (1.18–78.74 in).</p>"));
```

Note the range hint always shows both unit ranges per the spec. The word "mm" inside the hint sentence ("Smaller mm = …") is replaced with "value" to be unit-neutral.

- [ ] **Step 3: Update the live-reading widget label**

Locate line 370:

```cpp
                       "<div class='status' id='tof-live' style='margin-top:12px;'>Current Reading: <span id='tof-distance'>—</span> mm</div>"
```

Replace the trailing literal ` mm</div>` with a span the JS will populate:

```cpp
                       "<div class='status' id='tof-live' style='margin-top:12px;'>Current Reading: <span id='tof-distance'>—</span> <span id='tof-unit'>mm</span></div>"
```

The JS in Step 5 will set the `tof-unit` span based on the currently selected units value, so the unit label updates without a save round-trip.

- [ ] **Step 4: Add the `convertUnits()` JS helper and update the live-poll JS**

Locate the inline `<script>` block (lines 437–462). Find `function toggleTof()`:

```cpp
                       "function toggleTof(){"
                       "  var v=document.getElementById('sensor_type').value;"
                       "  document.getElementById('tof-fields').classList.toggle('show', v==='1');"
                       "}"
```

Insert immediately after the closing `}` of `toggleTof()`:

```cpp
                       "function convertUnits(){"
                       "  var u=document.getElementById('units').value;"
                       "  var ids=['tof_low','tof_half','tof_high'];"
                       "  for(var i=0;i<ids.length;i++){"
                       "    var f=document.getElementById(ids[i]);"
                       "    var v=parseFloat(f.value);"
                       "    if(isNaN(v)) continue;"
                       "    f.value=(u==='1')?(v/25.4).toFixed(2):Math.round(v*25.4).toString();"
                       "  }"
                       "  var labels=document.querySelectorAll(\"label[for='tof_low'],label[for='tof_half'],label[for='tof_high']\");"
                       "  var symbol=(u==='1')?'in':'mm';"
                       "  labels[0].innerHTML='LOW threshold ('+symbol+') — alert below this';"
                       "  labels[1].innerHTML='HALF threshold ('+symbol+')';"
                       "  labels[2].innerHTML='HIGH threshold ('+symbol+') — refill complete above this';"
                       "  var unitSpan=document.getElementById('tof-unit');"
                       "  if(unitSpan) unitSpan.textContent=symbol;"
                       "}"
```

Then locate the live-poll body (lines 451–456):

```cpp
                       "    fetch('/status').then(r=>r.json()).then(j=>{"
                       "      var el=document.getElementById('tof-distance');"
                       "      if(el && j.distance_mm!==undefined){el.textContent=j.distance_mm;}"
                       "      else if(el){el.textContent='—';}"
                       "    }).catch(()=>{});"
```

Replace with:

```cpp
                       "    fetch('/status').then(r=>r.json()).then(j=>{"
                       "      var el=document.getElementById('tof-distance');"
                       "      if(!el) return;"
                       "      if(j.distance_mm===undefined){el.textContent='—';return;}"
                       "      var u=document.getElementById('units').value;"
                       "      el.textContent=(u==='1')?(j.distance_mm/25.4).toFixed(2):j.distance_mm;"
                       "    }).catch(()=>{});"
```

The poll reads the *current dropdown value* (not the saved `cfgUnits`), so flipping the dropdown updates the live reading immediately — matches what `convertUnits()` does to the threshold inputs and labels.

- [ ] **Step 5: Wire `units` parsing and unit-aware threshold parsing into `handleSave()`**

Locate the staging block (lines 681–696):

```cpp
  // Sensor configuration — stage into locals first, validate, then commit to globals.
  // This prevents in-memory corruption if validation fails (NVS is also untouched on failure).
  SensorType newSensorType = cfgSensorType;
  uint16_t newTofLow  = cfgTofLow;
  uint16_t newTofHalf = cfgTofHalf;
  uint16_t newTofHigh = cfgTofHigh;

  if (server.hasArg("sensor_type")) {
    int t = server.arg("sensor_type").toInt();
    if (t == 1) newSensorType = SENSOR_TOF;
    else if (t == 2) newSensorType = SENSOR_IR_BREAK;
    else newSensorType = SENSOR_DIGITAL;
  }
  if (server.hasArg("tof_low"))  newTofLow  = server.arg("tof_low").toInt();
  if (server.hasArg("tof_half")) newTofHalf = server.arg("tof_half").toInt();
  if (server.hasArg("tof_high")) newTofHigh = server.arg("tof_high").toInt();
```

Replace with:

```cpp
  // Sensor configuration — stage into locals first, validate, then commit to globals.
  // This prevents in-memory corruption if validation fails (NVS is also untouched on failure).
  SensorType newSensorType = cfgSensorType;
  Units newUnits = cfgUnits;
  uint16_t newTofLow  = cfgTofLow;
  uint16_t newTofHalf = cfgTofHalf;
  uint16_t newTofHigh = cfgTofHigh;

  if (server.hasArg("sensor_type")) {
    int t = server.arg("sensor_type").toInt();
    if (t == 1) newSensorType = SENSOR_TOF;
    else if (t == 2) newSensorType = SENSOR_IR_BREAK;
    else newSensorType = SENSOR_DIGITAL;
  }
  if (server.hasArg("units")) {
    int u = server.arg("units").toInt();
    newUnits = (u == 1) ? UNITS_IMPERIAL : UNITS_METRIC;
  }

  // Apply the new units setting to the global *temporarily* so parseDistanceInput()
  // reads the request's intended unit. We restore on parse failure to preserve the
  // staged-write invariant (no global mutation until validation passes).
  Units savedUnits = cfgUnits;
  cfgUnits = newUnits;
  bool parseOk = true;
  if (server.hasArg("tof_low")  && !parseDistanceInput(server.arg("tof_low"),  newTofLow))  parseOk = false;
  if (server.hasArg("tof_half") && !parseDistanceInput(server.arg("tof_half"), newTofHalf)) parseOk = false;
  if (server.hasArg("tof_high") && !parseDistanceInput(server.arg("tof_high"), newTofHigh)) parseOk = false;
  cfgUnits = savedUnits;

  if (!parseOk) {
    sendValidationError("Each ToF threshold must be a positive number.");
    return;
  }
```

This preserves the staged-write invariant — `cfgUnits` is only persistently mutated below the validation block, after all checks pass. The temporary swap is local to the parse loop.

- [ ] **Step 6: Update validation messages to show both units**

Locate the range-check error message (line 702):

```cpp
      sendValidationError("Each ToF threshold must be between " + String(TOF_MIN_MM) + " and " + String(TOF_MAX_MM) + " mm.");
```

Replace with:

```cpp
      char minIn[8], maxIn[8];
      snprintf(minIn, sizeof(minIn), "%.2f", TOF_MIN_MM / 25.4f);
      snprintf(maxIn, sizeof(maxIn), "%.2f", TOF_MAX_MM / 25.4f);
      sendValidationError("Each ToF threshold must be between " + String(TOF_MIN_MM) + " mm (" + String(minIn) + " in) and " + String(TOF_MAX_MM) + " mm (" + String(maxIn) + " in).");
```

Locate the ordering-check error message (line 706):

```cpp
      sendValidationError("ToF thresholds must satisfy HIGH &lt; HALF &lt; LOW (smaller mm = fuller tank). Got HIGH=" + String(newTofHigh) + " HALF=" + String(newTofHalf) + " LOW=" + String(newTofLow) + ".");
```

Replace with:

```cpp
      const char* u = unitsLabel(newUnits);
      // formatDistance reads cfgUnits, so render against newUnits via temporary swap.
      Units savedUnits2 = cfgUnits;
      cfgUnits = newUnits;
      String hi = formatDistance(newTofHigh);
      String hf = formatDistance(newTofHalf);
      String lo = formatDistance(newTofLow);
      cfgUnits = savedUnits2;
      sendValidationError("ToF thresholds must satisfy HIGH &lt; HALF &lt; LOW (smaller value = fuller tank). Got HIGH=" + hi + " " + u + " HALF=" + hf + " " + u + " LOW=" + lo + " " + u + ".");
```

- [ ] **Step 7: Commit `cfgUnits` alongside the other config in the validation-passed block**

Locate the commit block (lines 712–715):

```cpp
  // Validation passed — commit to globals
  cfgSensorType = newSensorType;
  cfgTofLow  = newTofLow;
  cfgTofHalf = newTofHalf;
  cfgTofHigh = newTofHigh;
```

Replace with:

```cpp
  // Validation passed — commit to globals
  cfgSensorType = newSensorType;
  cfgUnits      = newUnits;
  cfgTofLow  = newTofLow;
  cfgTofHalf = newTofHalf;
  cfgTofHigh = newTofHigh;
```

- [ ] **Step 8: Add `units` to the `/status` JSON contract**

Locate the JSON build block (lines 754–778). Find:

```cpp
  json += "\"firmware\":\"" + String(FW_VERSION) + "\",";
  json += "\"sensor_type\":\"";
  json += sensorTypeJsonName(cfgSensorType);
  json += "\",";
```

Insert a new line between `"firmware":` and `"sensor_type":`:

```cpp
  json += "\"firmware\":\"" + String(FW_VERSION) + "\",";
  json += "\"units\":\"";
  json += (cfgUnits == UNITS_IMPERIAL) ? "imperial" : "metric";
  json += "\",";
  json += "\"sensor_type\":\"";
  json += sensorTypeJsonName(cfgSensorType);
  json += "\",";
```

`distance_mm` and `thresholds.{low,half,high}` are deliberately left untouched — they stay mm-valued always, per the spec.

- [ ] **Step 9: Compile**

```
arduino-cli compile --fqbn esp32:esp32:esp32 /home/juliettango/oil-monitor/OilTankMonitor 2>&1 | tail -3
```

Expected: clean compile. Sketch size delta from end of Task 1: **+400 to +1000 bytes** (web UI dropdown, JS function, validation expansion).

- [ ] **Step 10: Flash**

```
arduino-cli upload --fqbn esp32:esp32:esp32 -p /dev/ttyUSB0 /home/juliettango/oil-monitor/OilTankMonitor
```

- [ ] **Step 11: Verify metric-mode HTML diff is bounded to expected additions**

```
sleep 8  # wait for boot + WiFi
curl -s -c /tmp/cookies.txt -d "password=admin" http://192.168.0.241/login -o /dev/null
curl -sL -b /tmp/cookies.txt http://192.168.0.241/ -o /tmp/units-after-task2.html
diff /tmp/units-before.html /tmp/units-after-task2.html
```

Expected diff additions only:
- New `<label for='units'>Display Units</label>` block + `<select>` with the two options (between `<h2>Sensor Configuration</h2>` and `<label for='sensor_type'>`)
- Range hint changed: `Smaller mm = puck closer to sensor (fuller tank). … Range: 30–2000 mm.` → `Smaller value = puck closer to sensor (fuller tank). … Range: 30–2000 mm (1.18–78.74 in).`
- Live reading widget gained `<span id='tof-unit'>mm</span>` wrapping
- `<script>` gained `convertUnits()` function
- Live-poll JS body restructured but produces identical metric output

Expected diff *removals*: none of the existing labels' "(mm)" text — those are now interpolated via `unitsLabel()` and produce literal `mm` in metric mode. So the HTML strings `LOW threshold (mm) — alert below this`, `HALF threshold (mm)`, `HIGH threshold (mm) — refill complete above this` must still appear in the output — verify with `grep`:

```
grep -E "(LOW|HALF|HIGH) threshold \(mm\)" /tmp/units-after-task2.html
```

Expected: 3 matches. If 0, the labels are wrong.

- [ ] **Step 12: Verify `/status` JSON gained `units` and nothing else changed semantically**

```
curl -s -b /tmp/cookies.txt http://192.168.0.241/status -o /tmp/units-after-task2.json
diff /tmp/units-before-status.json /tmp/units-after-task2.json
```

Expected diff: only the addition of `,"units":"metric"` after `"firmware":...`. `distance_mm` and `thresholds.{low,half,high}` values match before (modulo any live distance reading drift, which is normal and not unit-related). If the json key order differs in a way that adds noise, instead spot-check:

```
python3 -c "import json; d=json.load(open('/tmp/units-after-task2.json')); print('units=',d.get('units')); print('distance_mm=',d.get('distance_mm')); print('thresholds=',d.get('thresholds'))"
```

Expected: `units= metric`, `distance_mm=` an integer (mm), `thresholds= {'low': 200, 'half': 130, 'high': 60}` (or whatever's saved).

- [ ] **Step 13: End-to-end UI walkthrough — switch to imperial, save, observe, switch back**

Open `http://192.168.0.241/` in a browser. Verify:

1. The Sensor Configuration section now has a `Display Units` dropdown above the Sensor Type dropdown, currently set to `Metric (mm)`.
2. ToF threshold input fields show `200`, `130`, `60` with `(mm)` labels and the live reading shows `NNN mm`.
3. Change the Display Units dropdown to `US Customary (inches)`. Threshold input fields immediately convert to `7.87`, `5.12`, `2.36`. Labels switch to `(in)`. Live reading changes to two-decimal inches on next poll (≤2 s).
4. Click Save & Restart. Wait for reboot (~10 s). Reload the page.
5. Display Units dropdown is `US Customary (inches)`. Threshold inputs still `7.87`, `5.12`, `2.36`. Labels still `(in)`.
6. `curl -s http://192.168.0.241/status | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['units'], d['distance_mm'], d['thresholds'])"` → `imperial 200 {'low': 200, 'half': 130, 'high': 60}` (mm values unchanged).
7. Switch back to `Metric (mm)`. Inputs revert to `200`, `130`, `60`. Save. Reload. Confirm.

If any of steps 3–7 fails, identify whether the bug is JS (no save needed to reproduce) or server-side (only after save), and go back to the corresponding step.

- [ ] **Step 14: Verify boot serial summary in imperial mode**

After step 13, `cfgUnits` is back to `UNITS_METRIC` (you switched back). Switch the dropdown to imperial and click save. After reboot, capture serial:

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
print(buf.decode('utf-8', errors='replace'))" | grep "Sensor type"
```

Expected: `Sensor type: TOF | ToF thresholds: low=7.87 in half=5.12 in high=2.36 in` (or DIGITAL/IR_BREAK per saved sensor type).

Then via UI, switch back to metric for clean state going into Task 3.

- [ ] **Step 15: Verify validation paths — both error messages render with both-unit text**

Submit a known-bad imperial value via curl (or via UI):

```
# In imperial mode, send 100 in (= 2540 mm > 2000 mm display limit but < 4000 TOF_MAX_MM, so the *ordering* check trips first)
curl -sL -b /tmp/cookies.txt -X POST http://192.168.0.241/save \
  --data-urlencode "ssid=YourSSID" --data-urlencode "password=YourWiFiPassword" \
  --data-urlencode "bot_token=000000000:REPLACE_WITH_REAL_TOKEN" \
  --data-urlencode "chat_id=000000000" \
  --data-urlencode "units=1" --data-urlencode "sensor_type=1" \
  --data-urlencode "tof_low=2.0" --data-urlencode "tof_half=5.0" --data-urlencode "tof_high=8.0" \
  --data-urlencode "web_pass=admin" -o /tmp/validation-resp.html
grep -E "ToF thresholds must satisfy" /tmp/validation-resp.html
```

Expected: an HTML error page contains `ToF thresholds must satisfy HIGH &lt; HALF &lt; LOW (smaller value = fuller tank). Got HIGH=8.00 in HALF=5.00 in LOW=2.00 in.` (Note imperial-formatted values.)

For the range error, send a number out of range:

```
# 200 in = 5080 mm > TOF_MAX_MM (4000)
curl -sL -b /tmp/cookies.txt -X POST http://192.168.0.241/save \
  --data-urlencode "ssid=YourSSID" --data-urlencode "password=YourWiFiPassword" \
  --data-urlencode "bot_token=000000000:REPLACE_WITH_REAL_TOKEN" \
  --data-urlencode "chat_id=000000000" \
  --data-urlencode "units=1" --data-urlencode "sensor_type=1" \
  --data-urlencode "tof_low=200" --data-urlencode "tof_half=5.12" --data-urlencode "tof_high=2.36" \
  --data-urlencode "web_pass=admin" -o /tmp/validation-range.html
grep -E "Each ToF threshold must be between" /tmp/validation-range.html
```

Expected: the page contains `Each ToF threshold must be between 30 mm (1.18 in) and 4000 mm (157.48 in).`

If either grep finds nothing, the validation message wiring in Step 6 is wrong.

NOTE: The auth cookie may have expired between the UI session and these curl calls. If you get a redirect to `/login`, re-run the login curl from Step 11.

NOTE 2: The submitted `bot_token` and `chat_id` values must match what's currently saved on the device (those are placeholders shown here from the existing config). Adjust if the device has different values, or pull them from `/status` first if needed. Better: do these validation checks via the browser UI rather than curl to avoid the auth+token round-trip pain.

- [ ] **Step 16: Switch back to metric for clean state, then commit**

Via UI: set Display Units to Metric (mm), save, wait for reboot. Confirm `/status` returns `units=metric`.

```
git add OilTankMonitor/OilTankMonitor.ino
git commit -m "$(cat <<'EOF'
Wire units toggle through web UI, JSON, and validation

Adds the Display Units dropdown to the Sensor Configuration section
(Metric/Imperial), with a client-side handler that converts the
three ToF threshold input values and updates labels in place when
the dropdown changes — no submit required.

handleSave parses the new units setting first, then parses each
threshold against it via parseDistanceInput, then validates the
resulting mm values against the existing 30–4000 mm range. The
staged-write invariant is preserved by temporarily swapping cfgUnits
during the parse loop and restoring on failure.

/status JSON gains a "units" field. distance_mm and thresholds.*
remain mm-valued (the only consumer is the inline poll JS, which
now converts client-side based on the current dropdown value).

Validation messages render in both units. Live reading widget
follows the dropdown immediately (no save required to see the
display change).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 3: README documentation + final hardware verification

**Files:**
- Modify: `README.md`

- [ ] **Step 1: Update the "Choosing a Sensor" section to mention the units toggle**

Locate the closing paragraph of the section (around line 45 of README.md):

```markdown
Most installs with a working liquid sight gauge use the XKC-Y25-V. Dry mechanical-float gauges typically use either an IR break-beam or a ToF sensor like VL53L1X.
```

Insert immediately after it:

```markdown
The web interface has a **Display Units** toggle (Metric / US Customary) that controls how distances are shown and accepted across the UI, the `/status` JSON poll, Telegram alerts, and the boot serial log. Internal storage is always in millimeters; the toggle only affects display and form-input layers, so switching units never changes the underlying configuration.
```

- [ ] **Step 2: Note imperial input acceptance in the ToF wiring section**

Locate the paragraph after the ToF wiring table (around line 99 of README.md):

```markdown
Mount the VL53L1X ToF sensor on top of the sight gauge looking down at the puck. The sensor reads distance in mm — smaller value means the puck is near the top (fuller tank), larger value means it has dropped (emptier tank). VL53L1X provides ranging up to ~4 m.
```

Replace with:

```markdown
Mount the VL53L1X ToF sensor on top of the sight gauge looking down at the puck. The sensor reads distance — smaller value means the puck is near the top (fuller tank), larger value means it has dropped (emptier tank). VL53L1X provides ranging up to ~4 m. Threshold values can be entered in millimeters or in inches per the **Display Units** preference; storage is always in millimeters.
```

- [ ] **Step 3: Run the full hardware-verification soak test**

Confirm each of these on the live device. No code change required.

1. **Default state preserved**: a fresh boot with `cfgUnits` defaulting to metric reads `low=NNN mm half=NNN mm high=NNN mm` in the boot serial. `/status` returns `"units":"metric"`. UI shows mm labels and integer threshold values.

2. **Switch to imperial via UI**:
   - Open `http://192.168.0.241/`, change Display Units to `US Customary (inches)`.
   - Threshold input values immediately become 2-decimal inches (`7.87`, `5.12`, `2.36`). Labels read `(in)`.
   - Live reading widget shows distance to 2 decimals + `in`.
   - Click Save & Restart. After reboot, page re-renders with the same imperial values. `/status` returns `"units":"imperial"`. Boot serial reads `low=7.87 in half=5.12 in high=2.36 in`.

3. **Trigger a Telegram state change** (cover or move the puck). Confirm Telegram message ends with `(7.87 in)` or similar, not `(NNN mm)`.

4. **Switch back to metric via UI**, save. After reboot:
   - Threshold values back to `200`, `130`, `60`. Labels read `(mm)`.
   - `/status` returns `"units":"metric"`. Boot serial returns to `low=200 mm half=130 mm high=60 mm`.
   - Telegram suffix back to `(NNN mm)` (with the new space — pre-existing intentional change from Task 1).

5. **Cross-mode persistence**: switch to digital sensor, save, then switch back to ToF. `cfgUnits` should still be in effect.

6. **Validation paths** — covered in Task 2 Step 15; re-confirm by submitting an out-of-range and an out-of-order value via the UI in imperial mode and confirming both error messages mention both units.

7. **Soak**: leave the device idle for 10+ minutes after the final unit switch. Confirm no new heap leak, no watchdog reboot, no spurious Telegrams. Sub-D's heap measurement (if you re-add the temporary `Serial.printf("free heap before /: %u\n", ESP.getFreeHeap())` from sub-project D's plan) should show the same per-render delta as before — the units toggle adds tens of bytes per render, not kilobytes.

If any of items 1–7 fails, identify the failing step and go back to the corresponding Task 1 or Task 2 step.

- [ ] **Step 4: Commit README changes**

```
git add README.md
git commit -m "$(cat <<'EOF'
Document the metric ↔ US customary display units toggle

Brief mention in the Choosing a Sensor section and a note on the
ToF wiring section that threshold values can be entered in either
unit per the Display Units preference. Internal storage stays in
millimeters regardless.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Self-Review

**Spec coverage**

| Spec section | Plan task |
|--------------|-----------|
| Storage (`Units` enum, `cfgUnits` global, NVS key) | Task 1 Steps 1–4 |
| Conversion helpers (`unitsLabel`, `formatDistance`, `parseDistanceInput`) | Task 1 Step 5 |
| Web UI: Display Units dropdown placement | Task 2 Step 1 |
| Web UI: threshold labels + value pre-fill + range hint | Task 2 Step 2 |
| Web UI: live-reading widget unit | Task 2 Step 3 |
| Client-side dropdown handler (`convertUnits()`) | Task 2 Step 4 |
| Form processing (parse `units`, then thresholds vs. new units) | Task 2 Step 5 |
| Form processing (commit cfgUnits with other config) | Task 2 Step 7 |
| Validation messages with both-unit text | Task 2 Step 6 |
| `/status` JSON adds `units`, keeps `distance_mm`/`thresholds.*` in mm | Task 2 Step 8 |
| Telegram suffix per cfgUnits | Task 1 Step 7 |
| Boot serial summary per cfgUnits | Task 1 Step 6 |
| README mention | Task 3 Steps 1–2 |
| Output equivalence (metric byte-for-byte modulo documented changes) | Task 1 Step 12, Task 2 Step 11 |
| Edge case: existing devices default to metric | Task 1 Step 3 default arg `0` |
| Edge case: POST flips units + sends inch-valued thresholds | Task 2 Step 5 (parse against newUnits via temp swap) |
| Edge case: invalid input → false from parseDistanceInput | Task 1 Step 5 + Task 2 Step 5 |
| Edge case: cross-mode persistence | Task 3 Step 3 item 5 |
| Hardware verification | Task 3 Step 3 |

No spec gaps found.

**Placeholder scan**

No `TBD`, `TODO`, `FIXME`, or vague-instruction patterns. Every code-touching step shows the actual code. Verification steps name expected exact strings. No "verify normal operation" hand-waves.

**Type consistency**

- `Units` enum used identically in Steps 1, 5, 7, plus Task 2 Steps 5, 6, 7, 8.
- `cfgUnits` global referenced in Task 1 Steps 5/6/7 and Task 2 Steps 1/2/4/5/8 — all reads.
- `unitsLabel()`, `formatDistance()`, `parseDistanceInput()` signatures defined in Task 1 Step 5 and called identically in Task 1 Steps 6/7 and Task 2 Steps 2/3/5/6.
- POST field name `units` consistent in Task 2 Steps 1 (`name='units'`), 5 (`server.hasArg("units")`), and Step 4 (`document.getElementById('units')`).
- JSON field name `units` consistent in Task 2 Step 8 server emit and Step 4 client read.

No issues found.
