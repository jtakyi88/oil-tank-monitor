# Hardware Watchdog Timer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Wrap the Arduino loop task in `esp_task_wdt` with a 15 s timeout so any future infinite-loop bug auto-recovers via reboot rather than bricking the device, with a Telegram alert on the post-recovery boot.

**Architecture:** Single Arduino sketch (`OilTankMonitor/OilTankMonitor.ino`). Five feed points distributed across `setup()`, `connectWiFi()`, `sendTelegram()`, `loop()` top, and the `loop()` disconnected branch. Reboot reason captured once via `esp_reset_reason()` at setup entry; if it equals `ESP_RST_TASK_WDT`, a recovery Telegram is sent immediately after the existing ONLINE message.

**Tech Stack:** Arduino-ESP32 core, ESP-IDF `esp_task_wdt.h`, `arduino-cli` for compile/upload, manual hardware verification (no test framework).

---

## File Structure

| File | Responsibility | Change scope |
|------|----------------|--------------|
| `OilTankMonitor/OilTankMonitor.ino` | All firmware behavior including watchdog wiring | Modify (add include + 5 feed points + reboot-reason capture + recovery alert) |
| `README.md` | User-facing reliability documentation | Modify (one bullet under "Reliability") |

Spec reference: `docs/superpowers/specs/2026-04-30-hardware-watchdog-design.md`.

---

### Task 1: Arm the watchdog and wire all five feed points

**Files:**
- Modify: `OilTankMonitor/OilTankMonitor.ino`

This task is a single cohesive change. Adding `esp_task_wdt_init()` without simultaneously adding the feed points would cause the device to reboot mid-boot inside `connectWiFi()` (which can run for up to 20 s). All five sites must land together to keep the firmware booting.

- [ ] **Step 1: Add the ESP-IDF watchdog header to the include block**

In `OilTankMonitor/OilTankMonitor.ino`, locate the include block at lines 20–27. Add the new include immediately after `<Adafruit_VL53L1X.h>`:

```cpp
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <Update.h>
#include <Wire.h>
#include <Adafruit_VL53L1X.h>
#include "esp_task_wdt.h"
```

- [ ] **Step 2: Arm the watchdog at the top of `setup()`**

Locate `void setup()` at line 1011. The current body starts:

```cpp
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== Oil Tank Monitor Starting ===");

  initSensor();
```

Insert the watchdog init **immediately after the boot banner `Serial.println` and before `initSensor()`**, so:

```cpp
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== Oil Tank Monitor Starting ===");

  esp_task_wdt_init(15, true);   // 15 s timeout, panic=true → reboot on miss
  esp_task_wdt_add(NULL);        // subscribe the current (Arduino loop) task

  initSensor();
```

- [ ] **Step 3: Feed inside the `connectWiFi()` retry loop**

Locate the retry loop at line 761:

```cpp
  WiFi.begin(cfgSSID.c_str(), cfgPassword.c_str());
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
```

Add `esp_task_wdt_reset();` as the first statement inside the loop body, so:

```cpp
  WiFi.begin(cfgSSID.c_str(), cfgPassword.c_str());
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    esp_task_wdt_reset();
    delay(500);
    Serial.print(".");
    attempts++;
  }
```

- [ ] **Step 4: Feed inside `sendTelegram()` before each per-chat POST**

Locate `sendTelegram` at line 784:

```cpp
bool sendTelegram(const String& message) {
  if (!bot || WiFi.status() != WL_CONNECTED) return false;
  bool anySent = false;
  String ids[] = {cfgChatID, cfgChatID2, cfgChatID3};
  for (int i = 0; i < 3; i++) {
    if (ids[i].length() > 0) {
      bool sent = bot->sendMessage(ids[i].c_str(), message, "");
      Serial.println(sent ? ("Telegram sent to " + ids[i]) : ("Telegram FAILED for " + ids[i]));
      if (sent) anySent = true;
    }
  }
  return anySent;
}
```

Add `esp_task_wdt_reset();` as the first statement inside `if (ids[i].length() > 0)`, so:

```cpp
bool sendTelegram(const String& message) {
  if (!bot || WiFi.status() != WL_CONNECTED) return false;
  bool anySent = false;
  String ids[] = {cfgChatID, cfgChatID2, cfgChatID3};
  for (int i = 0; i < 3; i++) {
    if (ids[i].length() > 0) {
      esp_task_wdt_reset();
      bool sent = bot->sendMessage(ids[i].c_str(), message, "");
      Serial.println(sent ? ("Telegram sent to " + ids[i]) : ("Telegram FAILED for " + ids[i]));
      if (sent) anySent = true;
    }
  }
  return anySent;
}
```

- [ ] **Step 5: Feed at the top of `loop()`**

Locate `void loop()` at line 1083:

```cpp
void loop() {
  server.handleClient();
  checkResetButton();
```

Add `esp_task_wdt_reset();` as the first statement, so:

```cpp
void loop() {
  esp_task_wdt_reset();
  server.handleClient();
  checkResetButton();
```

- [ ] **Step 6: Feed inside the `loop()` AP/disconnected branch**

Still in `loop()`, locate the early-return block around line 1088:

```cpp
  // Only run sensor logic when connected to WiFi (not in AP mode)
  if (apMode || WiFi.status() != WL_CONNECTED) {
    if (!apMode && WiFi.status() != WL_CONNECTED) {
      // Try to reconnect; fall back to AP if it fails
      if (!connectWiFi()) {
        startAPMode();
        server.begin();
      }
    }
    delay(100);
    return;
  }
```

Add `esp_task_wdt_reset();` immediately before the `delay(100); return;`:

```cpp
  // Only run sensor logic when connected to WiFi (not in AP mode)
  if (apMode || WiFi.status() != WL_CONNECTED) {
    if (!apMode && WiFi.status() != WL_CONNECTED) {
      // Try to reconnect; fall back to AP if it fails
      if (!connectWiFi()) {
        startAPMode();
        server.begin();
      }
    }
    esp_task_wdt_reset();
    delay(100);
    return;
  }
```

- [ ] **Step 7: Compile and confirm flash delta**

Run:

```
arduino-cli compile --fqbn esp32:esp32:esp32 /home/juliettango/oil-monitor/OilTankMonitor/OilTankMonitor.ino
```

Expected: clean compile, sketch size grows by approximately 1–2 KB versus the pre-task baseline (`1118252` bytes after Sub-project A's hot-fix). Record both numbers in the commit message.

- [ ] **Step 8: Upload to the connected ESP32**

Run:

```
arduino-cli upload --fqbn esp32:esp32:esp32 -p /dev/ttyUSB0 /home/juliettango/oil-monitor/OilTankMonitor/OilTankMonitor.ino
```

Expected: `Hard resetting via RTS pin...` followed by `New upload port: /dev/ttyUSB0 (serial)`.

- [ ] **Step 9: Verify boot completes without spurious reboot**

Capture serial via:

```
python3 -c "
import serial, time
s = serial.Serial('/dev/ttyUSB0', 115200, timeout=0.05)
s.dtr = False; s.rts = True; time.sleep(0.2); s.rts = False
buf = bytearray()
end = time.time() + 30
while time.time() < end:
    chunk = s.read(8192)
    if chunk: buf.extend(chunk)
print(buf.decode('utf-8', errors='replace'))"
```

Expected output contains `=== Oil Tank Monitor Starting ===`, `Web server started.`, and **no** `rst:0x` reboot lines after the first one. The watchdog is now armed and being fed; the device must remain stable for the full 30 s capture.

- [ ] **Step 10: Confirm the web UI still responds**

Run:

```
curl -sL --max-time 5 -o /dev/null -w "status=%{http_code} time=%{time_total}\n" http://192.168.0.241/
```

Expected: `status=200` (or `302` without `-L`) within ~2 s. Repeat 3 times to be sure no false reboots are happening every 15 s.

- [ ] **Step 11: Commit**

```
git add OilTankMonitor/OilTankMonitor.ino
git commit -m "$(cat <<'EOF'
Arm esp_task_wdt with 15s timeout and wire feed points

Adds the ESP-IDF task watchdog on the Arduino loop task. Five feed
sites cover all paths where a single feed-to-feed gap could exceed
15 s: connectWiFi retry loop, sendTelegram per-chat sends, top of
loop, and the AP/disconnect early-return branch.

Any future infinite-loop bug now reboots the device within 15 s
instead of bricking the web UI.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: Capture reboot reason and emit recovery Telegram

**Files:**
- Modify: `OilTankMonitor/OilTankMonitor.ino`

- [ ] **Step 1: Capture `esp_reset_reason()` at the top of `setup()`**

In `setup()`, immediately after `Serial.println("\n=== Oil Tank Monitor Starting ===");` and **before** the `esp_task_wdt_init` line added in Task 1, insert:

```cpp
  const esp_reset_reason_t bootReason = esp_reset_reason();
  const bool recoveredFromWdt = (bootReason == ESP_RST_TASK_WDT);
  if (recoveredFromWdt) {
    Serial.println("Boot reason: ESP_RST_TASK_WDT (recovered from a hang)");
  }
```

The complete sequence at the top of `setup()` after this step:

```cpp
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== Oil Tank Monitor Starting ===");

  const esp_reset_reason_t bootReason = esp_reset_reason();
  const bool recoveredFromWdt = (bootReason == ESP_RST_TASK_WDT);
  if (recoveredFromWdt) {
    Serial.println("Boot reason: ESP_RST_TASK_WDT (recovered from a hang)");
  }

  esp_task_wdt_init(15, true);
  esp_task_wdt_add(NULL);

  initSensor();
```

- [ ] **Step 2: Send the recovery Telegram after the existing ONLINE message**

Locate the existing ONLINE-message `sendTelegram(msg);` call inside `setup()` (around line 1073, inside the `if (!apMode)` block, after the `msg += "\nSettings: ..."` line):

```cpp
      msg += "\nSettings: http://" + WiFi.localIP().toString();
      sendTelegram(msg);
    }
  }
```

Add a conditional second call **after** `sendTelegram(msg);`, so:

```cpp
      msg += "\nSettings: http://" + WiFi.localIP().toString();
      sendTelegram(msg);
      if (recoveredFromWdt) {
        sendTelegram("⚠️ Recovered from a hang (watchdog reset). The device auto-rebooted to restore service.");
      }
    }
  }
```

- [ ] **Step 3: Compile**

```
arduino-cli compile --fqbn esp32:esp32:esp32 /home/juliettango/oil-monitor/OilTankMonitor/OilTankMonitor.ino
```

Expected: clean compile. Flash size growth from Task 1's baseline should be under ~200 bytes.

- [ ] **Step 4: Upload**

```
arduino-cli upload --fqbn esp32:esp32:esp32 -p /dev/ttyUSB0 /home/juliettango/oil-monitor/OilTankMonitor/OilTankMonitor.ino
```

- [ ] **Step 5: Verify normal boot does NOT send recovery alert**

Capture serial for ~20 s as in Task 1 Step 9. Expected:

- Boot banner appears.
- Serial **does not** contain `Boot reason: ESP_RST_TASK_WDT` (because this boot was a fresh upload, i.e. `ESP_RST_SW`).
- ONLINE Telegram arrives in the user's chat.
- **No** "Recovered from a hang" Telegram follows.

This confirms the recovery alert is correctly gated on the actual reset reason and isn't firing on every boot.

- [ ] **Step 6: Commit**

```
git add OilTankMonitor/OilTankMonitor.ino
git commit -m "$(cat <<'EOF'
Capture esp_reset_reason and send recovery Telegram on WDT reboot

When the watchdog fires and reboots the device, the next boot's
esp_reset_reason() returns ESP_RST_TASK_WDT. We capture that once
at setup entry and, after the normal ONLINE message has been sent,
emit a second Telegram that tells the user a hang was auto-recovered.

Single-event telemetry only — no persistent counters.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 3: Document the watchdog in the README

**Files:**
- Modify: `README.md`

- [ ] **Step 1: Locate the existing reliability/feature section in `README.md`**

Run:

```
grep -n "## Features\|## Reliability\|watchdog\|^- " /home/juliettango/oil-monitor/README.md | head -40
```

Use the output to identify the bullet list under "Features" or the closest equivalent reliability section in the current README.

- [ ] **Step 2: Add a single bullet describing the watchdog**

Add the following bullet to the Features list (or under a "Reliability" subheading if one already exists):

```markdown
- **15-second hardware watchdog**: any infinite loop in the firmware auto-reboots the device. After recovery, a Telegram alert is sent so hangs do not go unnoticed.
```

Place it adjacent to existing reliability-themed bullets (e.g., next to AP-mode fallback or settings persistence) rather than mixed in with feature bullets like web UI or sensor selection.

- [ ] **Step 3: Commit**

```
git add README.md
git commit -m "$(cat <<'EOF'
Document the 15s hardware watchdog and recovery Telegram in README

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 4: Hardware verification with an injected hang

**Files:**
- Modify (temporary, reverted before end of task): `OilTankMonitor/OilTankMonitor.ino`

This task **does not produce a commit**. It deliberately injects an infinite loop to prove the watchdog actually fires, captures the recovery Telegram, then reverts the injection.

- [ ] **Step 1: Inject a deliberate hang at the end of `setup()`**

In `OilTankMonitor/OilTankMonitor.ino`, locate the end of `setup()` (the closing brace at line ~1081, immediately after `Serial.println("Web server started.");`). Add a temporary infinite loop:

```cpp
  Serial.println("Web server started.");
  Serial.println("TEST: injecting infinite loop to verify watchdog");
  while (true) { /* deliberately hang to prove WDT fires */ }
}
```

- [ ] **Step 2: Compile and upload**

```
arduino-cli compile --fqbn esp32:esp32:esp32 /home/juliettango/oil-monitor/OilTankMonitor/OilTankMonitor.ino && \
arduino-cli upload --fqbn esp32:esp32:esp32 -p /dev/ttyUSB0 /home/juliettango/oil-monitor/OilTankMonitor/OilTankMonitor.ino
```

- [ ] **Step 3: Capture 60 s of serial spanning the first hang and the post-WDT reboot**

```
python3 -c "
import serial, time
s = serial.Serial('/dev/ttyUSB0', 115200, timeout=0.05)
s.dtr = False; s.rts = True; time.sleep(0.2); s.rts = False
buf = bytearray()
end = time.time() + 60
while time.time() < end:
    chunk = s.read(8192)
    if chunk: buf.extend(chunk)
print(buf.decode('utf-8', errors='replace'))"
```

Expected sequence in the captured output:

1. `=== Oil Tank Monitor Starting ===`
2. `Connecting to ...` then `Connected! IP: ...`
3. `Web server started.`
4. `TEST: injecting infinite loop to verify watchdog`
5. **Silence for ~15 seconds** (the hang).
6. A panic/reboot trace from ESP-IDF mentioning `Task watchdog got triggered` (typical text: `Task watchdog got triggered. The following tasks/users did not reset the watchdog in time:` followed by a CPU register dump).
7. The bootloader header (`ets Jul 29 2019 ...`).
8. A second `=== Oil Tank Monitor Starting ===`.
9. `Boot reason: ESP_RST_TASK_WDT (recovered from a hang)`.
10. WiFi reconnect, ONLINE Telegram, then **the recovery Telegram message**.
11. The hang fires again (this is expected — the test code is still in `setup()`).

Confirm with the user that they received both Telegram messages on their phone (ONLINE + recovery alert) on the second boot.

- [ ] **Step 4: Remove the injected hang**

Edit `OilTankMonitor/OilTankMonitor.ino` and revert `setup()`'s closing block to its pre-task state:

```cpp
  Serial.println("Web server started.");
}
```

- [ ] **Step 5: Confirm the diff cleanly reverts the test code**

Run:

```
git diff OilTankMonitor/OilTankMonitor.ino
```

Expected: empty diff (or only line-ending whitespace). If anything else differs from the committed Task 2 state, fix it before continuing.

- [ ] **Step 6: Compile and upload the clean firmware**

```
arduino-cli compile --fqbn esp32:esp32:esp32 /home/juliettango/oil-monitor/OilTankMonitor/OilTankMonitor.ino && \
arduino-cli upload --fqbn esp32:esp32:esp32 -p /dev/ttyUSB0 /home/juliettango/oil-monitor/OilTankMonitor/OilTankMonitor.ino
```

- [ ] **Step 7: Verify normal operation post-test**

Run two checks:

```
sleep 8
curl -sL --max-time 5 -o /dev/null -w "status=%{http_code} time=%{time_total}\n" http://192.168.0.241/
```

Expected: `status=200` within ~2 s.

Then capture 20 s of serial (same Python snippet as Task 1 Step 9). Expected: boot completes, ONLINE Telegram arrives, **no** recovery Telegram on this boot (because the prior boot ended in `ESP_RST_SW` from the reflash, not WDT).

- [ ] **Step 8: No commit**

This task is verification-only. The only file touched (`OilTankMonitor.ino`) was reverted in Step 4. Do not commit. Mark the task complete.

---

## Self-Review

**Spec coverage**
- "Configuration: 15 s timeout, panic=true, subscribed task" → Task 1 Step 2 (`esp_task_wdt_init(15, true); esp_task_wdt_add(NULL);`).
- "5 feed points" → Task 1 Steps 2, 3, 4, 5, 6.
- "Reboot-reason capture and recovery Telegram" → Task 2 Steps 1, 2.
- "Recovery message text" → Task 2 Step 2 uses the exact wording from the spec.
- "Recovery message sent as a separate Telegram after ONLINE" → Task 2 Step 2 places the conditional `sendTelegram(...)` immediately after the existing `sendTelegram(msg);`.
- "README update" → Task 3.
- "Testing: deliberate-hang verification" → Task 4.
- "First-ever boot returns POWERON, no alert" → covered by Task 2 Step 5 (post-flash boot is `ESP_RST_SW`, also non-WDT).
- "Recovery message silently skipped in AP mode" → already correct because `sendTelegram()` returns false when `bot` is null or WiFi is not connected; AP mode meets both conditions.

**Placeholder scan**: no TBDs, no "implement later," every code change shows the actual code. The grep in Task 3 Step 1 is exploratory but bounded to a single search — the plan tells the engineer what to do with the result (Step 2 specifies the exact bullet).

**Type consistency**: the local variable name `recoveredFromWdt` is defined in Task 2 Step 1 and used by Task 2 Step 2 — same name in both places. `bootReason` is also consistent. The constant `ESP_RST_TASK_WDT` matches the spec.

No issues found.
