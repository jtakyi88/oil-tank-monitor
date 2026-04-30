# Streaming HTML Renders Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Eliminate the multi-KB transient `String` allocation during web-handler renders by streaming HTML chunks directly to the WebServer client. Output bytes must remain byte-for-byte identical, verified by `diff` against captures taken before each refactor.

**Architecture:** Add a parallel set of `streamX()` functions that write to `server.sendContent(...)` directly. Migrate handlers one or two at a time, keeping the old `String`-returning functions in place during the transition. Once every caller has migrated, delete the old functions. Each task ends with a compilable, working firmware so we can flash and verify incrementally.

**Tech Stack:** Arduino-ESP32 v3.x, single `.ino` sketch, `arduino-cli` for compile/upload, `curl` for byte-equivalence diff testing, manual browser verification.

---

## File Structure

| File | Responsibility | Change scope |
|------|----------------|--------------|
| `OilTankMonitor/OilTankMonitor.ino` | All firmware behavior including render functions | Add `streamX()` functions; migrate handlers one task at a time; delete old `String`-returning functions in the final task |

Spec reference: `docs/superpowers/specs/2026-04-30-streaming-html-renders-design.md`.

### Render functions in scope

| Existing | Replacement | Used by |
|----------|-------------|---------|
| `htmlHeader(const String& title)` | `streamHtmlHeader(const String& title)` | every render function |
| `htmlFooter()` | `streamHtmlFooter()` | every render function |
| `buildLoginPage(bool failed)` | `streamLoginPage(bool failed)` | `handleLogin` |
| `buildSavedPage()` | `streamSavedPage()` | `handleSave` |
| `buildUpdatePage()` | `streamUpdatePage()` | `handleUpdatePage` |
| `buildUpdateResultPage(bool, String)` | `streamUpdateResultPage(bool, String)` | `handleUpdateResult` |
| `buildConfigPage()` | `streamConfigPage()` | `handleRoot` |
| `sendValidationError(const String& message)` | unchanged signature; body rewritten to stream | `handleSave` validation paths |
| `handleFactoryReset()` inline `htmlHeader/Footer` | uses `streamHtmlHeader/Footer` | self |

Total: 8 render functions migrated in 4 tasks.

---

## Pre-Refactor Capture (one-time setup before Task 1)

Before any code changes, capture the current rendered output of `/login` and `/` so we can diff against the post-refactor output. This is the safety net.

- [ ] **Setup Step 1: Capture `/login` (no auth needed)**

```
curl -s http://192.168.0.241/login -o /tmp/before-login.html
wc -c /tmp/before-login.html
```

Expected: ~1.5–2.5 KB. Record the byte count.

- [ ] **Setup Step 2: Log in and capture `/` (auth required)**

The web password defaults to `admin` unless changed. If the user has changed it, substitute the correct password below.

```
curl -s -c /tmp/cookies.txt -d "password=admin" http://192.168.0.241/login -o /dev/null
curl -s -b /tmp/cookies.txt http://192.168.0.241/ -o /tmp/before-config.html
wc -c /tmp/before-config.html
```

Expected: ~3.5–5 KB. Record the byte count. The file must contain `<form method='POST' action='/save'>` — if it instead contains `Login Required` or similar, the cookie didn't take and the password is wrong; ask the user for the correct password.

---

### Task 1: Add streaming helpers, migrate `buildLoginPage`

**Files:**
- Modify: `OilTankMonitor/OilTankMonitor.ino`

This task introduces the two streaming primitives (`streamHtmlHeader`, `streamHtmlFooter`) and migrates the smallest, easiest-to-verify caller (`/login`). The old `htmlHeader`/`htmlFooter` are kept; everything else still uses them. Compile result is fully working with both code paths coexisting.

- [ ] **Step 1: Add `streamHtmlHeader` and `streamHtmlFooter` immediately after the existing `htmlFooter()` function (around line 267)**

Insert after the closing `}` of `htmlFooter()`:

```cpp
void streamHtmlHeader(const String& title) {
  server.sendContent(F("<!DOCTYPE html><html><head><meta charset='UTF-8'>"
                       "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                       "<title>"));
  server.sendContent(title);
  server.sendContent(F("</title>"
                       "<style>"
                       "body{font-family:system-ui,-apple-system,sans-serif;margin:0;padding:20px;background:#1a1a2e;color:#e0e0e0;}"
                       ".container{max-width:480px;margin:0 auto;}"
                       "h1{color:#e94560;font-size:1.5em;border-bottom:2px solid #e94560;padding-bottom:8px;}"
                       "h2{color:#0f3460;font-size:1.1em;margin-top:24px;color:#16c79a;}"
                       "label{display:block;margin:12px 0 4px;font-weight:600;font-size:0.9em;}"
                       "input[type=text],input[type=password]{width:100%;padding:10px;border:1px solid #333;border-radius:6px;"
                       "  box-sizing:border-box;font-size:1em;background:#16213e;color:#e0e0e0;}"
                       "input:focus{outline:none;border-color:#e94560;}"
                       "button{background:#e94560;color:#fff;border:none;padding:12px 24px;border-radius:6px;"
                       "  font-size:1em;cursor:pointer;margin-top:20px;width:100%;}"
                       "button:hover{background:#c81e45;}"
                       ".toggle{display:flex;align-items:center;gap:10px;margin:12px 0;}"
                       ".toggle input{width:auto;}"
                       ".status{background:#16213e;padding:16px;border-radius:8px;margin:16px 0;border-left:4px solid #16c79a;}"
                       ".status.warn{border-left-color:#e94560;}"
                       ".ip-fields,.tof-fields{display:none;}.ip-fields.show,.tof-fields.show{display:block;}"
                       "a{color:#16c79a;}"
                       ".nav{margin:16px 0;font-size:0.9em;}"
                       ".eye-btn{background:none;border:none;color:#e0e0e0;cursor:pointer;font-size:1.2em;padding:8px;margin-top:0;width:auto;}"
                       "</style></head><body><div class='container'>"));
}

void streamHtmlFooter() {
  server.sendContent(F("</div></body></html>"));
}
```

The C++ adjacent-string-literal rule concatenates these into a single `F()` blob at compile time, so each call to `sendContent(F(...))` writes one big chunk in one go.

- [ ] **Step 2: Read the current `buildLoginPage(bool failed)` body so the new version transcribes character-for-character**

```
sed -n '540,556p' /home/juliettango/oil-monitor/OilTankMonitor/OilTankMonitor.ino
```

Use that output as the source of truth for Step 3.

- [ ] **Step 3: Add `streamLoginPage(bool failed)` immediately after `buildLoginPage(bool failed)`**

(Adding the streaming version next to the existing function — the old one stays for one more step.)

```cpp
void streamLoginPage(bool failed) {
  streamHtmlHeader("Oil Tank Monitor - Login");
  server.sendContent(F("<h1>Oil Tank Monitor</h1>"
                       "<h2>Login Required</h2>"));
  if (failed) {
    server.sendContent(F("<div class='status warn'>Incorrect password.</div>"));
  }
  server.sendContent(F("<form method='POST' action='/login'>"
                       "<label for='password'>Password</label>"
                       "<input type='password' id='password' name='password' required autofocus>"
                       "<button type='submit'>Log In</button>"
                       "</form>"));
  streamHtmlFooter();
}
```

**Important**: the F() chunks above must match the existing `buildLoginPage` output character-for-character. Compare against the `sed` output from Step 2. If anything differs, fix the F() literal, not the original.

- [ ] **Step 4: Update `handleLogin()` to call `streamLoginPage` instead of `buildLoginPage`**

Locate `handleLogin()` (around line 561). The two `server.send(200, "text/html", buildLoginPage(...))` lines:

```cpp
    server.send(200, "text/html", buildLoginPage(true));
    return;
  }
  server.send(200, "text/html", buildLoginPage(false));
```

Replace with:

```cpp
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html", "");
    streamLoginPage(true);
    return;
  }
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  streamLoginPage(false);
```

- [ ] **Step 5: Compile**

```
arduino-cli compile --fqbn esp32:esp32:esp32 /home/juliettango/oil-monitor/OilTankMonitor/OilTankMonitor.ino
```

Expected: clean compile. Sketch size delta vs Sub-project C baseline (1119460 bytes): probably ±200 bytes.

- [ ] **Step 6: Flash**

```
arduino-cli upload --fqbn esp32:esp32:esp32 -p /dev/ttyUSB0 /home/juliettango/oil-monitor/OilTankMonitor/OilTankMonitor.ino
```

- [ ] **Step 7: Capture post-refactor `/login`**

Wait ~5 s after flash for boot to complete, then:

```
sleep 5
curl -s http://192.168.0.241/login -o /tmp/after-login.html
wc -c /tmp/after-login.html
```

- [ ] **Step 8: Diff `/login` output**

```
diff /tmp/before-login.html /tmp/after-login.html
```

Expected: **no output** (files identical). If there is any diff, fix the F() literal in `streamLoginPage` or `streamHtmlHeader` until the diff is empty. Do not proceed to the next step until diff is empty.

- [ ] **Step 9: Commit**

```
git add OilTankMonitor/OilTankMonitor.ino
git commit -m "$(cat <<'EOF'
Stream /login HTML directly via sendContent (sub-D task 1)

Adds streamHtmlHeader/streamHtmlFooter primitives and streamLoginPage,
all writing directly to server.sendContent(F(...)). Migrates
handleLogin to use them. The old htmlHeader/htmlFooter/buildLoginPage
remain in place — other handlers still call them and will migrate
in subsequent tasks.

Output bytes verified identical via curl + diff against the
pre-refactor capture.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: Migrate `buildSavedPage`, `buildUpdatePage`, `buildUpdateResultPage`, `sendValidationError`, and `handleFactoryReset` inline render

**Files:**
- Modify: `OilTankMonitor/OilTankMonitor.ino`

These are post-action pages — the user reaches them only after submitting a form. Each is small (<2 KB output). Diff-checking each in isolation is cumbersome; the verification strategy here is "compile + visit each page in the browser + visually confirm correct rendering."

- [ ] **Step 1: Read current `buildSavedPage()` body for transcription**

```
sed -n '436,454p' /home/juliettango/oil-monitor/OilTankMonitor/OilTankMonitor.ino
```

- [ ] **Step 2: Add `streamSavedPage()` immediately after `buildSavedPage()`**

```cpp
void streamSavedPage() {
  String targetIP = cfgStaticIP && cfgIP.length() > 0 ? cfgIP : WiFi.localIP().toString();
  String targetURL = "http://" + targetIP;

  streamHtmlHeader("Settings Saved");
  server.sendContent(F("<h1>Settings Saved</h1>"
                       "<div class='status'>"
                       "Configuration saved. The device is restarting and connecting to <strong>"));
  server.sendContent(cfgSSID);
  server.sendContent(F("</strong>.<br><br>"
                       "Redirecting to <strong>"));
  server.sendContent(targetURL);
  server.sendContent(F("</strong> in <span id='countdown'>15</span> seconds..."
                       "</div>"
                       "<script>"
                       "var sec=15;var el=document.getElementById('countdown');"
                       "var t=setInterval(function(){sec--;el.textContent=sec;"
                       "if(sec<=0){clearInterval(t);window.location='"));
  server.sendContent(targetURL);
  server.sendContent(F("';}"
                       "},1000);"
                       "</script>"));
  streamHtmlFooter();
}
```

- [ ] **Step 3: Read current `buildUpdatePage()` body**

```
sed -n '456,473p' /home/juliettango/oil-monitor/OilTankMonitor/OilTankMonitor.ino
```

- [ ] **Step 4: Add `streamUpdatePage()` immediately after `buildUpdatePage()`**

```cpp
void streamUpdatePage() {
  streamHtmlHeader("Firmware Update");
  server.sendContent(F("<h1>Firmware Update</h1>"
                       "<div class='status'>"
                       "<strong>Current Version:</strong> v"));
  server.sendContent(FW_VERSION);
  server.sendContent(F("<br>"
                       "<strong>Free Space:</strong> "));
  server.sendContent(String(ESP.getFreeSketchSpace() / 1024));
  server.sendContent(F(" KB"
                       "</div>"
                       "<p style='font-size:0.9em;'>Upload a compiled <code>.bin</code> firmware file. The device will flash itself and reboot.</p>"
                       "<form method='POST' action='/update' enctype='multipart/form-data'>"
                       "<label for='firmware'>Firmware File (.bin)</label>"
                       "<input type='file' id='firmware' name='firmware' accept='.bin' required "
                       "style='padding:10px;background:#16213e;border:1px solid #333;border-radius:6px;width:100%;box-sizing:border-box;'>"
                       "<button type='submit' onclick=\"this.innerText='Uploading... do not power off';this.disabled=true;this.form.submit();\">Upload &amp; Install</button>"
                       "</form>"
                       "<div class='nav' style='margin-top:16px;'><a href='/'>&larr; Back to Settings</a></div>"));
  streamHtmlFooter();
}
```

- [ ] **Step 5: Read current `buildUpdateResultPage()` body**

```
sed -n '475,495p' /home/juliettango/oil-monitor/OilTankMonitor/OilTankMonitor.ino
```

- [ ] **Step 6: Add `streamUpdateResultPage(bool success, const String& message)` immediately after `buildUpdateResultPage()`**

```cpp
void streamUpdateResultPage(bool success, const String& message) {
  streamHtmlHeader(String("Update ") + (success ? "Complete" : "Failed"));
  server.sendContent(F("<h1>Firmware Update "));
  server.sendContent(success ? F("Complete") : F("Failed"));
  server.sendContent(F("</h1><div class='status"));
  if (!success) server.sendContent(F(" warn"));
  server.sendContent(F("'>"));
  server.sendContent(message);
  if (success) {
    server.sendContent(F("<br><br>Redirecting in <span id='countdown'>15</span> seconds..."
                         "</div>"
                         "<script>"
                         "var sec=15;var el=document.getElementById('countdown');"
                         "var t=setInterval(function(){sec--;el.textContent=sec;"
                         "if(sec<=0){clearInterval(t);window.location='/';}"
                         "},1000);"
                         "</script>"));
  } else {
    server.sendContent(F("</div>"
                         "<div class='nav' style='margin-top:16px;'><a href='/update'>&larr; Try Again</a></div>"));
  }
  streamHtmlFooter();
}
```

- [ ] **Step 7: Rewrite `sendValidationError(const String& message)` to stream**

Locate `sendValidationError` (around line 590). Replace its body:

```cpp
void sendValidationError(const String& message) {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(400, "text/html", "");
  streamHtmlHeader("Configuration Error");
  server.sendContent(F("<h1>Configuration Error</h1>"
                       "<div class='status warn'>"));
  server.sendContent(message);
  server.sendContent(F("</div>"
                       "<div class='nav' style='margin-top:16px;'><a href='/'>&larr; Back to Settings</a></div>"));
  streamHtmlFooter();
}
```

- [ ] **Step 8: Update `handleSave()` to call `streamSavedPage` instead of `buildSavedPage`**

Locate the `server.send(200, "text/html", buildSavedPage());` call (around line 656). Replace with:

```cpp
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  streamSavedPage();
```

- [ ] **Step 9: Update `handleUpdatePage()` to call `streamUpdatePage`**

Locate `server.send(200, "text/html", buildUpdatePage());` (around line 715). Replace with:

```cpp
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  streamUpdatePage();
```

- [ ] **Step 10: Update `handleUpdateResult()` to call `streamUpdateResultPage`**

Locate the two `server.send(200, "text/html", buildUpdateResultPage(...));` calls (around lines 744 and any failure-path equivalent). For each, replace with:

```cpp
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  streamUpdateResultPage(success, "...");   // pass the same args as before
```

(Use the same `success` boolean and message string the original call used. If a failure path passes a literal string, keep it literal.)

- [ ] **Step 11: Update `handleFactoryReset()` to use streaming**

Locate `handleFactoryReset()` (around line 661). The current body uses inline `htmlHeader/htmlFooter`:

```cpp
  String page = htmlHeader("Factory Reset");
  page += "<h1>Factory Reset Complete</h1>";
  page += "<div class='status'>All settings cleared. The device is restarting...</div>";
  page += htmlFooter();
  server.send(200, "text/html", page);
```

Replace with:

```cpp
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  streamHtmlHeader("Factory Reset");
  server.sendContent(F("<h1>Factory Reset Complete</h1>"
                       "<div class='status'>All settings cleared. The device is restarting...</div>"));
  streamHtmlFooter();
```

(If the original body has slightly different text, transcribe character-for-character — read the actual current code first.)

- [ ] **Step 12: Compile**

```
arduino-cli compile --fqbn esp32:esp32:esp32 /home/juliettango/oil-monitor/OilTankMonitor/OilTankMonitor.ino
```

Expected: clean compile. The old `buildSavedPage`/`buildUpdatePage`/`buildUpdateResultPage` functions are now unused — the compiler may emit `-Wunused-function` warnings for them. That's expected; they get deleted in Task 3.

- [ ] **Step 13: Flash**

```
arduino-cli upload --fqbn esp32:esp32:esp32 -p /dev/ttyUSB0 /home/juliettango/oil-monitor/OilTankMonitor/OilTankMonitor.ino
```

- [ ] **Step 14: Visual smoke test**

In a desktop browser, with valid session cookie:

1. Visit `http://192.168.0.241/update` → should render the firmware-update form. Confirm "Current Version", "Free Space", and the file-upload control are present.
2. Visit `http://192.168.0.241/` → submit the form with current values unchanged. The Saved page should render with the countdown and redirect.

For `sendValidationError`, run:

```
curl -s -b /tmp/cookies.txt -X POST -d "ssid=test&password=test&bot_token=test&chat_id=1&sensor_type=1&tof_low=999&tof_half=500&tof_high=999" http://192.168.0.241/save | head -20
```

Expected: HTML containing `<h1>Configuration Error</h1>` and the threshold error message.

- [ ] **Step 15: Commit**

```
git add OilTankMonitor/OilTankMonitor.ino
git commit -m "$(cat <<'EOF'
Stream small post-action pages directly via sendContent (sub-D task 2)

Migrates the four small render handlers — buildSavedPage,
buildUpdatePage, buildUpdateResultPage, sendValidationError — and
the inline htmlHeader/Footer use in handleFactoryReset to call
streamHtmlHeader / streamHtmlFooter and stream their content via
server.sendContent.

The old String-returning functions remain unused (compiler may
warn) and are deleted in the next task once buildConfigPage has
also migrated.

Verified visually via /update render, /save form submit, and a
forced sendValidationError via bad threshold POST.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 3: Migrate `buildConfigPage` → `streamConfigPage` and delete dead code

**Files:**
- Modify: `OilTankMonitor/OilTankMonitor.ino`

This is the big one. `buildConfigPage()` is 166 lines; `streamConfigPage()` is the same logic with each `page += "..."` replaced by `server.sendContent(F("..."))` and each `page += dynamicValue` replaced by `server.sendContent(dynamicValue)`. The diff-empty check against `/tmp/before-config.html` is the safety net.

Because this task also deletes the old String-returning functions, it must be one commit.

- [ ] **Step 1: Re-capture pre-refactor `/` output (in case session changed since setup)**

```
curl -s -c /tmp/cookies.txt -d "password=admin" http://192.168.0.241/login -o /dev/null
curl -s -b /tmp/cookies.txt http://192.168.0.241/ -o /tmp/before-config.html
wc -c /tmp/before-config.html
```

(If the user has changed the password from `admin`, substitute it.) Expected: ~3.5–5 KB. Confirm the file contains `<form method='POST' action='/save'>`.

- [ ] **Step 2: Read `buildConfigPage()` in full**

```
sed -n '269,433p' /home/juliettango/oil-monitor/OilTankMonitor/OilTankMonitor.ino
```

This is the ground truth. Every static fragment must be transcribed character-for-character into the F() literals of `streamConfigPage`. Every dynamic value (`cfgSSID`, `cfgPassword`, `cfgBotToken`, `cfgChatID`, `cfgChatID2`, `cfgChatID3`, `cfgIP`, etc.) must remain a separate `server.sendContent(value)` call.

- [ ] **Step 3: Add `streamConfigPage()` immediately after `buildConfigPage()`**

Build it methodically by walking the existing `buildConfigPage` body top-to-bottom. The pattern for each existing line:

| Existing line pattern | Streaming equivalent |
|-----------------------|----------------------|
| `String page = htmlHeader("X");` | `streamHtmlHeader("X");` |
| `page += "literal";` | `server.sendContent(F("literal"));` |
| `page += "...x..." + variable + "...y...";` | `server.sendContent(F("...x..."));`<br>`server.sendContent(variable);`<br>`server.sendContent(F("...y..."));` |
| `page += String(ternary ? "a" : "b");` | `server.sendContent(ternary ? F("a") : F("b"));` |
| `page += htmlFooter();` | `streamHtmlFooter();` |
| `return page;` | (nothing — function is `void`) |

Adjacent F() string literals can be merged; the C++ compiler concatenates them at compile time. Where two `page += "..."` lines are adjacent with no dynamic content between them, collapse them into a single multi-line F() literal for efficiency:

```cpp
server.sendContent(F("<h2>WiFi Network</h2>"
                     "<label for='ssid'>SSID (Network Name)</label>"));
```

This is purely a code-density optimization; output bytes are unchanged.

**Don't write the new function in one go.** Walk the existing function in 10–20-line chunks, transcribing carefully. Diff each chunk visually against the source. The full 166-line function makes for a single ~180-line `streamConfigPage`.

- [ ] **Step 4: Update `handleRoot()` to call `streamConfigPage`**

Locate `handleRoot()` (around line 585):

```cpp
void handleRoot() {
  if (!requireAuth()) return;
  server.send(200, "text/html", buildConfigPage());
}
```

Replace with:

```cpp
void handleRoot() {
  if (!requireAuth()) return;
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  streamConfigPage();
}
```

- [ ] **Step 5: Compile**

```
arduino-cli compile --fqbn esp32:esp32:esp32 /home/juliettango/oil-monitor/OilTankMonitor/OilTankMonitor.ino
```

Expected: clean compile. At this point, every old String-returning function (`htmlHeader`, `htmlFooter`, `buildLoginPage`, `buildSavedPage`, `buildUpdatePage`, `buildUpdateResultPage`, `buildConfigPage`) is unused. Compiler will warn about unused functions.

- [ ] **Step 6: Flash and capture post-refactor `/`**

```
arduino-cli upload --fqbn esp32:esp32:esp32 -p /dev/ttyUSB0 /home/juliettango/oil-monitor/OilTankMonitor/OilTankMonitor.ino
sleep 8
curl -s -c /tmp/cookies.txt -d "password=admin" http://192.168.0.241/login -o /dev/null
curl -s -b /tmp/cookies.txt http://192.168.0.241/ -o /tmp/after-config.html
wc -c /tmp/after-config.html
```

- [ ] **Step 7: Diff `/` output**

```
diff /tmp/before-config.html /tmp/after-config.html
```

Expected: **no output** (files identical).

If there is a diff:
- The output is hex-printable HTML; visually inspect the diff to identify the missing/added/changed character.
- Most likely causes: mismatched quote (`'` vs `"`), missing space, dropped character in a long string literal, or a `String("ToF")` vs the new `F("ToF")` ternary expression evaluating differently.
- Fix the offending F() literal in `streamConfigPage`. Recompile, reflash, recapture, re-diff.
- Do **not** proceed to Step 8 until diff is empty.

- [ ] **Step 8: Delete the old String-returning functions**

Now that no caller remains, remove these functions entirely:

- `String htmlHeader(const String& title)` (lines ~237–263)
- `String htmlFooter()` (lines ~265–267)
- `String buildLoginPage(bool failed)` (lines ~540–554)
- `String buildSavedPage()` (lines ~436–454)
- `String buildUpdatePage()` (lines ~456–473)
- `String buildUpdateResultPage(bool, const String&)` (lines ~475–495)
- `String buildConfigPage()` (lines ~269–434)

Use `git grep -n "buildConfigPage\|buildLoginPage\|buildSavedPage\|buildUpdatePage\|buildUpdateResultPage\|htmlHeader\|htmlFooter"` to verify no callers remain (only definitions of the new `streamX` functions and their call sites should appear).

- [ ] **Step 9: Compile**

```
arduino-cli compile --fqbn esp32:esp32:esp32 /home/juliettango/oil-monitor/OilTankMonitor/OilTankMonitor.ino
```

Expected: clean compile, **no unused-function warnings**. Sketch size should be smaller than the post-Task-2 baseline (the old String-builder bodies were ~6–8 KB of compiled code; expect a meaningful drop).

- [ ] **Step 10: Re-flash and re-verify diff**

```
arduino-cli upload --fqbn esp32:esp32:esp32 -p /dev/ttyUSB0 /home/juliettango/oil-monitor/OilTankMonitor/OilTankMonitor.ino
sleep 8
curl -s -c /tmp/cookies.txt -d "password=admin" http://192.168.0.241/login -o /dev/null
curl -s -b /tmp/cookies.txt http://192.168.0.241/ -o /tmp/after-config-2.html
diff /tmp/before-config.html /tmp/after-config-2.html
```

Expected: still empty diff. (Deleting unused code shouldn't change output, but verifying twice is cheap.)

- [ ] **Step 11: Commit**

```
git add OilTankMonitor/OilTankMonitor.ino
git commit -m "$(cat <<'EOF'
Stream config page directly; delete String-returning render helpers

The big refactor: buildConfigPage (166 lines, ~4 KB output) becomes
streamConfigPage, calling server.sendContent(F(...)) for each static
fragment and server.sendContent(value) for each dynamic interpolation.
Peak heap during /home requests drops from ~4 KB transient String to
~200 bytes for dynamic-value Strings only.

With every caller migrated, the eight String-returning render
functions (htmlHeader, htmlFooter, buildLoginPage, buildSavedPage,
buildUpdatePage, buildUpdateResultPage, buildConfigPage) are now
deleted.

Output verified byte-for-byte identical via curl + diff against the
pre-refactor capture.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 4: Hardware verification — full UI walkthrough

**Files:**
- None (verification only — no code change, no commit)

- [ ] **Step 1: Visual desktop browser walkthrough**

In a desktop browser, navigate to `http://192.168.0.241/` and confirm:

1. The page loads and renders identical-looking to before.
2. Status box shows "Connected to <SSID>", IP, oil level, firmware version.
3. **WiFi section**: SSID and password inputs prefilled with current values.
4. **Telegram section**: bot token and 3 chat-ID inputs prefilled, "Add chat" buttons present.
5. **Sensor type dropdown**: shows current selection. Options visible: Digital threshold / ToF distance (VL53L1X) / IR break-beam.
6. **ToF thresholds** (when ToF selected): three numeric inputs prefilled. Hidden when other sensor types selected.
7. **IP configuration**: static-IP toggle, four IP fields hidden by default.
8. **Password change**: password field visible.
9. **Save button**: present at the bottom of the form.
10. **Update / factory-reset** links present in nav.

- [ ] **Step 2: Form-submit smoke test**

Submit the form with current values unchanged. Confirm:
- Saved page renders with countdown.
- After redirect, the config page reloads with the same values.

- [ ] **Step 3: Validation path**

Submit with intentionally-bad ToF thresholds (e.g., HIGH > LOW). Confirm:
- Validation error page renders with the message.
- "Back to Settings" link works.

- [ ] **Step 4: Update page**

Visit `http://192.168.0.241/update`. Confirm:
- Renders with current version, free space, file-upload control.
- "Back to Settings" link works.

(Don't actually upload firmware — just confirm the page is present.)

- [ ] **Step 5: Mobile rendering**

On a phone (per existing user setup workflow), load the same pages. Confirm rendering matches desktop.

- [ ] **Step 6: Heap measurement (optional)**

If desired, add temporary `Serial.printf` calls around `handleRoot`'s body and reload `/` while watching serial:

```cpp
void handleRoot() {
  if (!requireAuth()) return;
  Serial.printf("free heap before /: %u\n", ESP.getFreeHeap());
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  streamConfigPage();
  Serial.printf("free heap after /: %u\n", ESP.getFreeHeap());
}
```

Compare deltas. Expected: post-refactor delta is significantly smaller than pre-refactor (was ~4 KB transient drop; should now be a few hundred bytes for dynamic-value Strings).

Remove the temporary Serial.printf and reflash. **No commit for this step** — it's verification only.

- [ ] **Step 7: Soak**

Leave the device idle for 10+ minutes after a fresh `/` render. Confirm the device remains responsive (visit `/status` periodically) and shows no new heap-related issues.

- [ ] **Step 8: No commit**

This task is verification only. Mark complete in TodoWrite once all steps pass.

---

## Self-Review

**Spec coverage**
- "Convert `buildConfigPage` to streaming" → Task 3.
- "Convert helpers (`htmlHeader`/`htmlFooter`)" → Task 1, Steps 1–4 (added alongside the old ones); deleted in Task 3 Step 8.
- "Convert all small render functions" → Task 1 (login) and Task 2 (saved/update/result/validation/factory-reset).
- "Output bytes identical" → Task 1 Step 8 and Task 3 Step 7 require empty `diff`.
- "Delete old `String`-returning functions" → Task 3 Step 8.
- "Visual + mobile verification" → Task 4.
- "Optional heap measurement" → Task 4 Step 6.

**Placeholder scan**: every code change shows the actual code. The one "transcribe character-for-character" instruction in Task 3 Step 3 is supported by Step 2's `sed` command that emits the source-of-truth lines, plus an exhaustive line-pattern table — not a hand-wave.

**Type consistency**: helper names appear consistently — `streamHtmlHeader(const String&)`, `streamHtmlFooter()`, `streamLoginPage(bool)`, `streamSavedPage()`, `streamUpdatePage()`, `streamUpdateResultPage(bool, const String&)`, `streamConfigPage()`. Used identically in their definitions and call sites.

**Edge case noted**: the `buildUpdateResultPage` ternary `String("Update ") + (success ? "Complete" : "Failed")` in Task 2 Step 6 is one place where the streaming version may produce slightly different ordering of allocations vs. the original — but the rendered bytes are identical because we still build the same title String, just for `streamHtmlHeader`'s parameter rather than for `htmlHeader`'s. No diff impact.

No issues found.
