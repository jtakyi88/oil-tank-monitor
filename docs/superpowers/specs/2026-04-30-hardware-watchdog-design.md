# Hardware Watchdog Timer — Design

**Date**: 2026-04-30
**Files touched**: `OilTankMonitor/OilTankMonitor.ino`, `README.md`
**Sub-project of**: Production-readiness cleanup pass (A → **B** → C → D)

## Background

Sub-project A surfaced a real-world hang: with a missing/loose ToF sensor, `Adafruit_VL53L1X::InitSensor()` enters an unbounded `while(tmp==0)` poll, freezing the entire Arduino loop. We fixed that specific trap, but the firmware has no general defense — any future bug (third-party library quirk, network deadlock, runaway state) that traps the main task will produce the same symptom: TCP keeps accepting on port 80, but every HTTP handler hangs forever, and the device must be power-cycled.

ESP32 ships with `esp_task_wdt` — a hardware-backed task watchdog. Arming it on the Arduino loop task converts every "main loop hang" failure mode into a 15 s wait followed by an automatic reboot.

## Goal

Wrap the Arduino main task in `esp_task_wdt` with a 15 s timeout. Any feed gap longer than 15 s reboots the device. On the next boot, a Telegram alert tells the user a hang was auto-recovered.

## Non-Goals

- Tuning ESP-IDF's interrupt watchdog (defaults are fine for our workload).
- Adding watchdogs to other FreeRTOS tasks (WiFi / lwIP / `WiFiClientSecure` already have their own).
- Persistent reboot counters or "Nth reset within X hours" alerting (YAGNI — single-event telemetry is enough to surface a problem).
- Detecting brownout resets (`ESP_RST_BROWNOUT`). Brownouts are hardware/PSU faults, distinct from software hangs.

## Configuration

| Setting | Value |
|---------|-------|
| Timeout | **15 seconds** |
| Panic on trigger | **true** (forces a reboot, not just an alert) |
| Subscribed task | The Arduino loop task (subscribed via `esp_task_wdt_add(NULL)` inside `setup()`) |

15 s comfortably accommodates one Telegram HTTPS POST (typical 1–5 s, worst-case ~10 s on a flaky network) while still catching genuine infinite loops within an acceptable user-visible window.

## Feed Points

The watchdog is reset (fed) at five sites. These are the only places where a single feed-to-feed gap could legitimately exceed 15 s without instrumentation.

| # | Location | Why this site |
|---|----------|---------------|
| 1 | Top of `setup()` — `esp_task_wdt_init(15, true)` + `esp_task_wdt_add(NULL)` | Arms the watchdog before any boot work, including `connectWiFi()` and the first `sendTelegram()` |
| 2 | Inside `connectWiFi()`'s retry `while` loop body | WiFi association can take 10–30 s; the loop's own `delay(500)` ticks become the natural feed cadence |
| 3 | Inside `sendTelegram()` — once per `cfgChatIDx` send, before each POST | Up to 3 chats × ~5 s = 15 s in the worst case; feeding between sends keeps the per-feed gap to one POST |
| 4 | Top of `loop()` — `esp_task_wdt_reset()` | Main steady-state feed; covers all sensor reads, web handlers, and bus-recovery |
| 5 | Inside the `if (apMode \|\| WiFi.status() != WL_CONNECTED)` branch in `loop()` (right before its `delay(100); return;`) | That early-return path bypasses the top-of-loop feed during reconnects; a prolonged disconnection would otherwise self-trigger the WDT |

`delay()` does not auto-feed `esp_task_wdt`. Every `delay()` longer than 15 s would self-trigger; none in the current code exceed 1 s, but the feed sites above bracket every loop in the firmware.

## Reboot-Reason Telemetry

ESP-IDF's `esp_reset_reason()` returns an enum identifying why the chip last reset. We capture it once at the top of `setup()`:

```cpp
const esp_reset_reason_t bootReason = esp_reset_reason();
const bool recoveredFromWdt = (bootReason == ESP_RST_TASK_WDT);
```

Later, in the existing post-WiFi block where the ONLINE message is sent, if `recoveredFromWdt` is true we send a **second** Telegram immediately after the ONLINE message:

> ⚠️ Recovered from a hang (watchdog reset). The device auto-rebooted to restore service.

Two messages (ONLINE + recovery) are deliberately separate so the user sees both signals: "device is up" and "the previous boot ended in a hang." If the recovery alert ever becomes spammy, future work can fold it into the ONLINE message; for now, separation is clearer.

`recoveredFromWdt` is captured into a local variable in `setup()` — no new global state.

## Code Touchpoints

All in `OilTankMonitor/OilTankMonitor.ino` unless noted.

| Location | Change |
|----------|--------|
| Includes | Add `#include "esp_task_wdt.h"` |
| `setup()` start (before `Serial.begin`) | Capture `esp_reset_reason()` into local `bootReason` |
| `setup()` after `Serial.begin` + boot banner | `esp_task_wdt_init(15, true);` + `esp_task_wdt_add(NULL);` |
| `setup()` ONLINE-message block | After existing `sendTelegram(msg)`, conditionally send recovery alert if `bootReason == ESP_RST_TASK_WDT` |
| `connectWiFi()` retry loop | Add `esp_task_wdt_reset();` inside the `while(WiFi.status() != WL_CONNECTED && ...)` body |
| `sendTelegram()` per-chat block | Add `esp_task_wdt_reset();` before each `sendToChat()` (or equivalent per-chat call) |
| `loop()` top | Add `esp_task_wdt_reset();` as the first statement |
| `loop()` AP/disconnect branch | Add `esp_task_wdt_reset();` immediately before its `delay(100); return;` |
| `README.md` | One line under "Reliability features" mentioning the 15 s watchdog with auto-reboot + recovery Telegram |

## Edge Cases

- **Watchdog fires during the first-boot WiFi connect**: feed point #2 (inside the connect retry loop) prevents this. If `connectWiFi()` itself returns false and we fall into AP mode, feed point #5 keeps feeding while we wait for user reconfiguration.
- **Watchdog fires during the recovery Telegram itself**: covered by feed point #3 (per-chat feeds in `sendTelegram`).
- **First-ever boot after flashing**: `esp_reset_reason()` returns `ESP_RST_POWERON`. No recovery alert sent. Correct.
- **Manual reset via the front-panel button or RTS-line reflash**: returns `ESP_RST_EXT` or `ESP_RST_SW`. No recovery alert. Correct.
- **AP-mode boot (no WiFi configured)**: watchdog runs, but `recoveredFromWdt`'s alert path requires Telegram, which requires WiFi. The recovery message is silently skipped in AP mode — acceptable, since the user is physically present and reconfiguring.
- **OTA update**: triggers `ESP_RST_SW` on reboot, not `ESP_RST_TASK_WDT`. No false recovery alert.
- **Brownout reset**: returns `ESP_RST_BROWNOUT`. Non-WDT, no alert. Out of scope here.

## Testing

Project has no automated test framework. Manual verification:

1. `arduino-cli compile` — succeeds; record flash delta (expected +1 to +2 KB).
2. Flash to ESP32. Confirm normal boot completes within ~5 s, "Web server started." prints, no spurious reboot.
3. Verify ONLINE Telegram arrives as before, with **no** recovery alert (boot was POWERON, not WDT).
4. Inject a deliberate hang: temporarily add `while (true) {}` at the end of `setup()` after `server.begin()`. Reflash.
5. Confirm: `Serial` goes silent after "Web server started.", device reboots within ~15 s, and after the next boot's WiFi connect the user sees **two** Telegram messages — the ONLINE message followed by the recovery alert.
6. Remove the injected hang, reflash, confirm normal operation again.
7. Regression: confirm web UI loads and Digital / IR_BREAK / ToF modes still work as before. Confirm no false reboots over a 5-minute idle period.

## Implementation Notes

This is the second sub-project in the production-readiness pass and should follow A. Each subsequent sub-project (C: label-helper consolidation, D: String/heap cleanup) will inherit the watchdog automatically — every new code path is already covered by feed point #4 (top of `loop()`) without further wiring.
