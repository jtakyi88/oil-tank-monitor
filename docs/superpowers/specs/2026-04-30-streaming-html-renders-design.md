# Streaming HTML Renders + Heap Cleanup — Design

**Date**: 2026-04-30
**Files touched**: `OilTankMonitor/OilTankMonitor.ino`
**Sub-project of**: Production-readiness cleanup pass (A → B → C → **D**)

## Background

The firmware's web handlers build their HTML output by accumulating a `String` via repeated `+=` concatenation, then handing it to `server.send()`. The dominant offender is `buildConfigPage()` (166 lines, ~70+ `page += "..."` operations, ~4 KB of HTML output): every render allocates and reallocates a heap-resident `String` large enough to hold the entire page. Smaller handlers (`buildSavedPage`, `buildUpdatePage`, `sendValidationError`) follow the same pattern but each output less than 1 KB.

Across all calls into the WebServer the firmware sustains hundreds of intermediate heap allocations per page render. Heap fragmentation has not yet caused a visible problem, but the firmware is otherwise paged for production-readiness — eliminating the giant transient String is a worthwhile reliability improvement.

A platform note shapes this design: **on ESP32 (arduino-esp32 v3.x), the `F()` macro is effectively a no-op.** Unlike AVR — where `F()` moves string literals from RAM to flash — on ESP32 every string literal already lives in flash by default. Wrapping literals in `F()` saves nothing on this hardware. The real leverage is *avoiding the heap copy*, not "moving to flash."

## Goal

Eliminate the multi-kilobyte transient `String` allocation during `/` (config page) renders by streaming the page directly to the WebServer client in chunks. Pre-reserve smaller pages' `String` buffers so they don't realloc during construction. Output bytes must be identical to the current implementation — no visible UI change.

## Non-Goals

- No HTML or CSS rewrite. The visible page must look exactly the same in desktop and mobile browsers.
- No form-input renames. `handleSave()` reads every field by `name='...'`; renaming any would break form submission.
- No XSS hardening for user-supplied fields (`cfgSSID`, `cfgBotToken`, etc.). Current code inserts those raw into the HTML; this design preserves that behavior. Hardening is a separate concern out of scope here.
- No `F()` macro sweep — no-op on this platform.
- No `/status` JSON refactor. JSON is short and refactoring it offers marginal heap benefit.
- No Telegram message construction refactor. Messages are short.

## Architecture

Each existing render function is replaced or modified based on output size:

| Existing function | Replacement | Rationale |
|-------------------|-------------|-----------|
| `String htmlHeader(const String& title)` | `void streamHtmlHeader(const String& title)` | Used by every handler — must support streaming for the big one to work. |
| `String htmlFooter()` | `void streamHtmlFooter()` | Companion to header. |
| `String buildConfigPage()` | `void streamConfigPage()` | The big one — ~4 KB output. The whole point of this sub-project. |
| `String buildSavedPage()` | unchanged signature; add `page.reserve(1024)` | <1 KB output; one allocation is fine. |
| `String buildUpdatePage()` | unchanged signature; add `page.reserve(2048)` | <2 KB output (firmware-update form). |
| `void sendValidationError(const String& message)` | unchanged structure; add `page.reserve(1024)` | <1 KB output; rarely called. |

The `stream*` functions write directly to the WebServer client via `server.sendContent(...)`. They never allocate a heap String for their output.

`handleRoot()` becomes:

```cpp
void handleRoot() {
  if (!requireAuth()) return;
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  streamConfigPage();
}
```

`server.setContentLength(CONTENT_LENGTH_UNKNOWN)` switches the response to chunked transfer encoding so we don't have to know the total length up front.

`buildSavedPage()` and `buildUpdatePage()` keep their current `String` return signature and existing call sites in `handleSave()` and `handleUpdatePage()`. Their internals just gain one `page.reserve(N)` line near the top to avoid the realloc cycle. They also need to switch from calling the old `htmlHeader/htmlFooter` (which return `String`) to a new approach — see "Header/footer compatibility" below.

`sendValidationError()` similarly keeps its signature but reserves and switches to the streaming header pair.

### Header/footer compatibility

`htmlHeader()` and `htmlFooter()` are called by both the big-handler path (which now streams) and the small-handler path (which still builds a String). Two options:

**Option A** *(chosen)*: Keep both forms. The streaming version `streamHtmlHeader(title)` writes to the server. The small handlers stop calling `htmlHeader()` and instead call `streamHtmlHeader()` followed by streaming their own content directly via `server.sendContent`, then `streamHtmlFooter()`. Their `String page; page.reserve(...)` pattern goes away — they too become streaming functions, just smaller ones. The only `String` left for these handlers is for any dynamic content they need to format mid-stream.

**Option B**: Keep two parallel implementations — one returning `String`, one streaming. Doubles the maintenance burden.

**Decision**: Option A. Once `streamHtmlHeader/Footer` exist, every handler streams. The `.reserve()` discussion above becomes moot for the small handlers — they don't allocate big Strings at all.

This simplifies the matrix:

| Existing function | Replacement | Behavior |
|-------------------|-------------|----------|
| `String htmlHeader(const String& title)` | `void streamHtmlHeader(const String& title)` | Streams; no allocation. |
| `String htmlFooter()` | `void streamHtmlFooter()` | Streams; no allocation. |
| `String buildConfigPage()` | `void streamConfigPage()` | Streams the whole config page. |
| `String buildSavedPage()` | `void streamSavedPage()` | Streams. |
| `String buildUpdatePage()` | `void streamUpdatePage()` | Streams. |
| `void sendValidationError(const String& message)` | unchanged signature; body streams instead of building a String | Streams. |

Every handler becomes:

```cpp
void handleX() {
  if (!requireAuth()) return;     // (where applicable)
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  streamX();
}
```

### What `streamConfigPage()` looks like internally

Each previous `page += "..."` becomes a `server.sendContent(F("..."))` or, for dynamic content, `server.sendContent(value)`. The `F()` macro around string literals here is harmless on ESP32 (it's a no-op) and harmlessly portable. Example pattern:

```cpp
void streamConfigPage() {
  streamHtmlHeader("Oil Tank Monitor - Settings");

  if (!apMode && WiFi.status() == WL_CONNECTED) {
    server.sendContent(F("<div class='status'>"));
    server.sendContent(F("<strong>Status:</strong> Connected to <em>"));
    server.sendContent(cfgSSID);
    server.sendContent(F("</em><br><strong>IP:</strong> "));
    server.sendContent(WiFi.localIP().toString());
    // ... etc
    server.sendContent(F("</div>"));
  } else {
    server.sendContent(F("<div class='status warn'>"
                         "<strong>Setup Mode</strong> — Configure your settings below, then the device will connect to your WiFi."
                         "</div>"));
  }

  server.sendContent(F("<h1>Oil Tank Monitor</h1><form method='POST' action='/save'>"));
  // ... continue per the existing buildConfigPage structure
  streamHtmlFooter();
}
```

Adjacent static fragments may be collapsed into one `sendContent` call (the C++ adjacent-string-literal concatenation handles this for free). This reduces the call count without changing output.

## Output Equivalence

The output bytes — every `<` and `>` and class name and form input — must match the current implementation byte-for-byte. The verification strategy is:

1. Capture the current page output before refactoring (`curl -sL http://192.168.0.241/ > before.html`).
2. After refactoring, capture the new output (`curl -sL http://192.168.0.241/ > after.html`).
3. `diff before.html after.html` must be empty.

Any non-empty diff indicates a transcription bug.

## Files & Flash

One file: `OilTankMonitor/OilTankMonitor.ino`. Expected flash delta: small (−500 to −2000 bytes; the eliminated String concatenation code paths are slightly bulkier than direct sendContent calls). Heap delta during /home requests: peak transient allocation drops from ~4 KB to under ~200 bytes (just dynamic-value Strings like `cfgSSID`).

## Edge Cases

- **AP-mode boot** with no WiFi: `streamConfigPage()` still runs (handler is registered in both modes). The `if (!apMode && ...)` branch is the same as before; the warn-banner else-branch streams instead of building a String. Same output.
- **Form submit with bad input** (`/save` with bad ToF thresholds): `sendValidationError()` streams a small error page. Output identical to current.
- **`/update` and `/factory-reset`** handlers continue to use `streamHtmlHeader/Footer` and stream small bodies. Output identical to current.
- **Authentication failure** in `requireAuth()`: handler returns before any send happens, same as before. The auth flow is untouched.
- **Concurrent requests**: ESP32's WebServer is single-task by design; only one request renders at a time. No new concurrency concerns.
- **Chunked encoding compatibility**: every modern browser handles chunked transfer encoding. iOS Safari, Chrome mobile, and Firefox have all supported it for over a decade. No risk.

## Testing

Project has no automated tests. Manual hardware verification:

1. **Pre-refactor capture**: `curl -sL http://192.168.0.241/ -o /tmp/before.html` (with valid session cookie if needed). Save for comparison.
2. **Compile clean**: `arduino-cli compile`.
3. **Flash to ESP32**.
4. **Post-refactor capture**: `curl -sL http://192.168.0.241/ -o /tmp/after.html`. `diff before.html after.html` must produce no output.
5. **Visual smoke test**: load `http://192.168.0.241/` in a desktop browser. Walk every section. Confirm WiFi, Telegram, sensor type dropdown, ToF thresholds, IP config, and password change sections all look identical.
6. **Form submit**: submit the form with current values unchanged → confirm Saved page renders, then re-render config page shows the same values.
7. **Validation path**: submit with intentionally bad ToF thresholds → confirm validation error page renders with the right message.
8. **Mobile**: load on phone (per existing user setup workflow). Confirm rendering matches.
9. **Update / factory-reset pages**: visit `/update`, confirm form renders. (Don't actually upload firmware; just confirm the page is present.)
10. **Heap check** *(optional, nice-to-have)*: temporarily add `Serial.printf("free heap before /: %u\n", ESP.getFreeHeap())` at the top of `handleRoot()` and `Serial.printf("free heap after /: %u\n", ESP.getFreeHeap())` at the bottom. Reload the page a few times. Confirm pre→post delta is significantly smaller than before (was ~4 KB; should now be ~200 bytes).
11. **Soak**: leave the device idle for 10+ minutes after a render to confirm no new heap leak.

## Implementation Notes

This is the riskiest sub-project of the four. The mitigation is the diff-based output equivalence test — *any* missed character or attribute will be caught immediately. If the diff isn't empty, the refactor is wrong; revert and try again.

The plan should split this into roughly three tasks:

1. **D-1**: Add `streamHtmlHeader`/`streamHtmlFooter`; convert the small handlers (`buildSavedPage`, `buildUpdatePage`, `sendValidationError`) into streaming form. Keep `buildConfigPage` building a String for now (small handlers can be tested independently). Verify diff-empty for the small pages.

2. **D-2**: Convert `buildConfigPage` → `streamConfigPage`. The big single-commit migration. Single commit because intermediate states don't render. Verify diff-empty for the config page.

3. **D-3**: Hardware verification — full UI walkthrough on browser + mobile, soak test, optional heap measurement. No code change; no commit.
