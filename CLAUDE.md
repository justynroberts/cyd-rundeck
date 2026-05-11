# cyd-rundeck — CLAUDE.md

A "Cheap Yellow Display" (ESP32-2432S028R) that talks to Rundeck. Distilled from
prior work building the PagerDuty variant — the hardware, LVGL, and provisioning
notes here are paid-for in debugging hours, don't relitigate them.

---

## CYD hardware (ESP32-2432S028R v2, USB-C, ST7789)

Working TFT_eSPI build flags — these are not the defaults, the CYD ecosystem
documents conflicting values for different board revisions. **Use these for v2:**

- `ST7789_DRIVER`, `TFT_WIDTH=240`, `TFT_HEIGHT=320`
- `TFT_RGB_ORDER=TFT_BGR` — NOT RGB (red renders as blue otherwise)
- `TFT_INVERSION_OFF=1` — NOT ON (blacks become white otherwise)
- TFT pins: `MOSI=13, SCLK=14, CS=15, DC=2, RST=-1, BL=21`
- Touch (XPT2046) on a **separate** SPI host (VSPIClass):
  `MOSI=32, MISO=39, SCLK=25, CS=33, IRQ=36`

If colours look wrong, flip `BGR/RGB` before changing anything else.

A working `platformio.ini` and starter pin map live in the sibling `pagerduty-cyd`
repo — copy from there rather than rederive.

---

## LVGL on ESP32 — production gotchas

### Memory pool fragmentation kills everything
LVGL's heap is one allocator. Rebuilding lists every poll (clean + recreate)
fragments it. Eventually `lv_obj_create` returns NULL and the next call
crashes inside `lv_obj_mark_layout_as_dirty`.

- **Fix:** only rebuild the *visible* list. Cache references to value labels
  for in-place updates.
- **Don't** keep bumping `LV_MEM_SIZE` — you hit the `dram0_0_seg` overflow well
  before 64 KB. Pair any increase with shrinking `DRAW_BUF_LINES`.

### Screen transition races
`lv_scr_load_anim()` called repeatedly on the same target restarts the animation;
it never finishes. Symptom: `lv_scr_act()` reports the new screen but pixels
never refresh.

- **First transition out of a modal (Connecting → Dashboard):** use
  `lv_scr_load_anim(scr, NONE, 0, 0, false)` — synchronous, no animation.
- Subsequent transitions can animate.

### Symbol glyphs require their host font
`LV_SYMBOL_LEFT/HOME/SETTINGS` are encoded characters that only exist in LVGL's
default Montserrat builds. Swap to a custom font (Outfit, Inter, …) without
including the symbol range and those glyphs render as squares.

- **Fix:** when running `lv_font_conv`, include the LVGL FontAwesome symbol
  range and merge it into your font. The pagerduty-cyd build does this — see
  `tools/build_fonts.sh` there.

### Thin font weights are unreadable on cheap TFTs
The CYD's contrast ratio is mediocre. Anything thinner than the font's Bold or
Medium weight visually disappears off-axis. Default to **Bold** for body text on
this hardware. Reserve Thin for very large display sizes only.

### Default Montserrat lacks UTF-8 punctuation
Middle-dot `·` (U+00B7) and other common Unicode separators aren't in LVGL's
bundled Montserrat — they render as squares. Use ASCII separators (`-`, `|`, `/`)
or include the full charset when generating a custom font.

### WiFi + LVGL must run on different cores
TLS handshakes block 2–3 s. Doing them on the LVGL core freezes the screen.

- WiFi / network on core 0 as a FreeRTOS task (`xTaskCreatePinnedToCore`).
- LVGL on core 1 (Arduino default).
- Communicate via `portMUX_TYPE`-protected globals. **Never** call LVGL
  functions from the WiFi task.

### State-driven screen logic must recognise all screens
Main-loop checks like "if not on a known screen, force overview" silently break
when you add a new rotating screen and forget to update `currentScreen()`.
Symptom: dashboard auto-rotates to the new screen for ~30 ms then yanks back.
Have **one** place that knows about all screens.

---

## WiFi provisioning — skip WiFiManager

### WiFiManager v2.0.17 is unreliable on Arduino-ESP32 v2.x
Symptoms: form submissions silently fail; non-blocking mode loses POSTs;
"fail saving credentials" with no diagnostic. Replace with ~120 lines of
`WebServer` + `DNSServer`: own captive portal, own scan, own form handler,
own NVS write. See `pagerduty-cyd/src/wifi_setup.cpp`.

### Connect on first boot is flaky
Saved-creds connect can fail 1-in-3 with `Reason: 39 - TIMEOUT` even on a
known-good network. Always retry: 3 attempts × 25 s timeout, then fall back
to captive portal.

### Captive-portal magic URLs
Catch all of these and redirect to your portal root:

| OS      | URL(s) |
|---------|--------|
| iOS     | `/library/test/success.html`, `/hotspot-detect.html` |
| Android | `/generate_204`, `/gen_204` |
| Windows | `/connecttest.txt`, `/ncsi.txt` |

### Scan in AP_STA mode is finicky
The scan can silently fail or return 0 networks. The portal must:
- Not require the dropdown selection (let the user type a manual SSID)
- Distinguish "scanning" (n == -1) from "no networks found" (n == 0)
- Self-heal: rekick the scan every ~8 s while in portal loop
- Offer an explicit Rescan button

### mDNS gives you a stable URL
`MDNS.begin("rundeck")` + `MDNS.addService("http", "tcp", 80)` →
`http://rundeck.local/`. Works on most networks. IP fallback for noisy ones —
Apple usually resolves it, Android often doesn't.

### Factory reset gesture
Hardware RST just reboots — it doesn't clear creds. Add a boot-time touch hold:
sample `ts.touched()` after `display::begin()`, draw a progress bar with raw
TFT (LVGL not yet up), wipe `Preferences` + `WiFi.disconnect(true, true)` if
held for ~2 s, then `ESP.restart()`.

---

## Rundeck REST API — adapt from PagerDuty patterns

Rundeck-specific endpoints TBD when implementing. General patterns that apply:

### TLS handshake errors are mostly transient
HTTP `-1` (refused), `-5` (not connected), `-11` (read timeout) usually succeed
on the second try. Wrap **mutating** actions in a 3-attempt retry with ~600 ms
backoff. Don't retry permanent errors (401, 403, 404).

### Server-side narrowing > client-side filter
Push filter params through to the API (project, status, job-id). Smaller
responses, less LVGL pool churn, faster updates. The trade-off is that derived
counts reflect the filter — either accept that or do a separate small fetch
for totals.

### URL-encode brackets in array params
`statuses[]=running` → `statuses%5B%5D=running`. Some intermediaries strip
un-encoded `[]`.

### Watch payload size
Big response bodies fail ArduinoJson with "incomplete input" because the TLS
read truncates mid-stream. Trim with `?fields=…` / pagination rather than
parsing everything.

### Auth
Rundeck uses an `X-Rundeck-Auth-Token` header (User API tokens generated via
the Rundeck UI under user profile). Store in NVS; treat as secret. Validate
the token on save by hitting `GET /api/N/system/info` — quick, low-impact.

### Region / base URL
Rundeck is self-hosted, so the base URL is per-deployment. Make it a
mandatory setup field in the captive portal alongside the API token.

---

## What lives where

- This repo (`cyd-rundeck`): the Rundeck-specific firmware
- Sibling repo (`pagerduty-cyd`): reference implementation — copy
  `platformio.ini`, `display.cpp`, `wifi_setup.cpp`, `tools/build_fonts.sh`
  as starting points; don't rewrite from scratch
