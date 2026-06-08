# Capsule Radar (formerly "Plane Radar 2.0") — CLAUDE.md

Master context for Claude Code. Read this first, then `docs/` for detail.

## What we're building
A live ADS-B aircraft radar for the **Waveshare ESP32-S3-Touch-AMOLED-1.75** (round 466×466 AMOLED, capacitive touch). It's an evolution of the classic 240×240 GC9A01 "plane radar": same idea (pull nearby aircraft from an online ADS-B feed over WiFi, plot them on a radar scope centered on the user), but redesigned for a full-color high-res round AMOLED with touch, IMU, RTC and a speaker.

Centered on **Dénia, Spain** by default (configurable). Target end-result: a polished, MakerWorld-publishable desk gadget (3D-printed enclosure + this firmware).

The visual target is in `assets/plane_radar_2.0_mockup.html` — open it in a browser. That mockup is the source of truth for the look & feel (phosphor-green radar on true black, aircraft glyphs rotated by heading, altitude color-coding, fading trails, animated sweep, tap-to-inspect detail card, emergency highlight).

## Hardware (summary — full detail in docs/HARDWARE.md)
- MCU: ESP32-S3R8, 8 MB PSRAM, 16 MB flash, dual-core 240 MHz, WiFi + BLE5.
- Display: CO5300 AMOLED, 466×466, QSPI. Brightness via panel command (no PWM backlight pin).
- Touch: CST9217, I2C.
- IMU: QMI8658 (I2C). RTC: PCF85063 (I2C). PMIC: AXP2101 (I2C 0x34). Audio: ES8311 codec + speaker, dual mic.
- **Verified pins**: LCD_CS=12, LCD_RST=39, TP_INT=11, TP_RST=40, touch mirror_x/y = true.
- **Pins still to confirm from the official demo**: QSPI SCLK + D0..D3, and the shared I2C SDA/SCL. Do NOT guess these — copy them from the Waveshare Arduino factory demo (see below). They are left as `-1` placeholders in `src/config.h`.

## Stack decision
**PlatformIO + Arduino framework.** Libraries:
- `moononournation/GFX Library for Arduino` (Arduino_GFX) — CO5300 QSPI panel driver + framebuffer.
- `lvgl/lvgl` (v8.x or v9.x) — UI screens, touch input, widgets.
- `bblanchon/ArduinoJson` (v7) — parse the ADS-B feed.
- WiFi / WiFiClientSecure / HTTPClient (built-in).

ESP-IDF is a valid alternative (Waveshare ships IDF demos too) but Arduino is the faster path here and has the most community examples for this board. If we switch, only the driver/UI glue changes; `geo.*`, `adsb_client.*` logic and the data model port directly.

### Official Waveshare Arduino demos to crib from (do this first)
The board's wiki ships these examples — clone them and lift the exact init code:
- `01_HelloWorld` → CO5300 + Arduino_GFX databus pins (THIS gives us the missing QSPI/I2C pins).
- `03_LVGL_PCF85063_simpleTime` → RTC + LVGL wiring.
- `04_LVGL_QMI8658_ui` → IMU read.
- `05_LVGL_AXP2101_ADC_Data` → battery/PMIC.
- `06_LVGL_Widgets` → LVGL config reference (`lv_conf.h`).
- `08_ES8311` → audio codec init (for the alert "ping").
Wiki: https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.75

## Data source (full detail in docs/DATA_SOURCE.md)
**airplanes.live** free REST API. Query by position + radius:
`GET https://api.airplanes.live/v2/point/{lat}/{lon}/{radius_nm}`
Returns JSON with an aircraft array (key `ac`, readsb format). Fields we use: `hex`, `flight`, `lat`, `lon`, `alt_baro`, `track`/`true_heading`, `gs`, `baro_rate`, `squawk`, `seen_pos`.
- **Educational / non-commercial use only** — that's exactly this project. Be polite: ~1 request / 1–2 s, set a descriptive User-Agent.
- Fallback: **adsb.lol** (`https://api.adsb.lol/v2/point/...`, same format).
- Avoid OpenSky for now (OAuth2 + tighter limits, awkward on-device).

## Architecture (full detail in docs/ARCHITECTURE.md)
- **Core 0 task** (`adsb_task`): WiFi keepalive, fetch + parse the feed every `POLL_INTERVAL_MS`, write into a shared `std::vector<Aircraft>` guarded by a FreeRTOS mutex.
- **Core 1 / Arduino loop**: LVGL tick + render. Reads the aircraft list under the mutex, projects lat/lon → screen (see `geo.*`), draws the scope, sweep, trails, glyphs, labels and the active detail card.
- 8 MB PSRAM easily holds a full RGB565 framebuffer (466×466×2 ≈ 434 KB) and double-buffer; allocate LVGL draw buffers in PSRAM.
- Settings (WiFi creds, home lat/lon, range, units, theme) in NVS (`Preferences`). First-boot **captive portal** (WiFiManager) to enter them. **OTA** via ArduinoOTA.

## Repo layout
```
plane-radar-2.0/
├─ CLAUDE.md              ← you are here
├─ README.md
├─ platformio.ini
├─ src/
│  ├─ config.h           ← user/build config + pin map (EDIT pins from demo)
│  ├─ geo.h              ← haversine / bearing / project-to-screen (complete)
│  ├─ aircraft.h         ← Aircraft data model
│  ├─ adsb_client.h/.cpp ← fetch + parse airplanes.live (working draft, untested on HW)
│  ├─ radar_view.h       ← scope rendering API (to implement)
│  └─ main.cpp           ← task setup + glue (skeleton with TODOs)
├─ docs/
│  ├─ HARDWARE.md
│  ├─ DATA_SOURCE.md
│  ├─ FEATURES.md
│  ├─ ARCHITECTURE.md
│  └─ SETUP.md
└─ assets/
   └─ plane_radar_2.0_mockup.html   ← visual target
```

## Build / flash
```
pio run                        # build
pio run -t upload              # flash over USB-C
pio device monitor -b 115200   # serial
```
First make the Waveshare `01_HelloWorld` equivalent light up, then bring this scaffold's pins in line and build upward through the milestones.

## Roadmap (suggested milestones)
- **M0 — Bring-up**: get the official HelloWorld/LVGL widgets demo running; copy verified databus + I2C pins into `config.h`. Backlight + touch + a "hello" screen.
- **M1 — Static scope**: draw rings, crosshair, N/E/S/W, center dot, animated sweep. Match the mockup palette on true-black AMOLED.
- **M2 — Live data**: WiFi + captive portal; `adsb_client` fetch/parse; project aircraft to screen; glyphs rotated by `track`; altitude color map; fading trails.
- **M3 — Touch & detail**: hit-test nearest glyph on tap → detail card (callsign, type, alt, gs, vs, dist, bearing, squawk). Swipeable views: radar / list / stats.
- **M4 — Polish**: range zoom; north-up vs track-up; emergency/military/type alerts + speaker ping; idle auto-dim; IMU face-down sleep / shake-to-refresh; OTA; persist settings.

## Conventions & guardrails
- C++17. Keep the render path non-blocking — no network or `delay()` in the LVGL loop; all I/O lives in `adsb_task`.
- Touch the shared aircraft vector only under `xSemaphoreTake(g_ac_mutex, ...)`.
- All tunables live in `config.h`. No magic numbers in render code.
- HTTPS: for a hobby device `WiFiClientSecure::setInsecure()` is acceptable; a pinned root cert is the "proper" option — note the choice in code.
- **Never invent the unknown GPIO pins.** They come from the official demo. Placeholders are `-1` and the build should assert/log if they're still `-1`.
- API is non-commercial; keep request cadence gentle and User-Agent honest.
