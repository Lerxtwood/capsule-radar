# Codex project context

This file is a handoff note for starting a fresh Codex conversation without carrying the full chat history forward.

## Project shape

There are three related firmware codebases on this machine:

- `D:\git\Arduino\capsule-radar`
  - Main combined firmware slot.
  - Contains Capsule Radar plus embedded TamaPoke app.
  - Owns the browser web installer and GitHub release workflow.
- `D:\git\Arduino\PrintSphere`
  - Companion firmware slot for the PrintSphere app.
  - Built by the `capsule-radar` release workflow and installed into the second app slot.
- `D:\git\Arduino\TamaPoke`
  - Original upstream-ish TamaPoke project/fork.
  - Current combined work does not happen here unless intentionally backporting.

## Device / partition model

The device is a Waveshare ESP32-S3 AMOLED 1.75" round display device with 16 MB flash and PSRAM.

Current browser installer installs multiple firmware images:

- bootloader: `0x0`
- partition table: `0x8000`
- OTA data initial: `0x109000`
- CapsuleRadar combined firmware: `0x110000`
- PrintSphere firmware: `0x610000`

The web installer URL is:

`https://lerxtwood.github.io/capsule-radar/`

Firmware updates are intentionally handled by the browser web installer, not by in-device OTA flashing, because there are multiple firmware slots and updating the "other" slot from inside one firmware felt awkward.

Important local-dev caveat:

- A normal PlatformIO USB upload (`pio run -e esp32-s3-amoled-175 -t upload`) does **not** preserve this companion multi-slot layout.
- That standard upload path writes the app using the normal Arduino/PlatformIO offsets and can leave the PrintSphere slot non-bootable from the bootloader's point of view.
- Symptom when this happens:
  - `[app] radar PrintSphere pull-down failed: Boot partition switch failed: ESP_ERR_OTA_VALIDATE_FAILED`
- If that happens, repair the device by re-flashing the full companion layout described below.

## Current release state

Latest known release:

- `v2.0.16`
- GitHub release: `https://github.com/Lerxtwood/capsule-radar/releases/tag/v2.0.16`
- Capsule Radar commit: `92b4ce0` (`Improve cached aircraft photos and Wright Flyer placeholder`)

Current repo state when this note was last updated:

- `D:\git\Arduino\capsule-radar`
  - release commit pushed
  - one untracked scratch file remains under `tmp\`
- `D:\git\Arduino\PrintSphere`
  - not part of the `v2.0.16` change set

## Release workflow

To create a release:

1. Commit and push PrintSphere first if it changed.
2. Bump `FW_VERSION` in `D:\git\Arduino\capsule-radar\src\config.h`.
3. Bump `PRINTSPHERE_RELEASE_VERSION` in `D:\git\Arduino\PrintSphere\CMakeLists.txt`.
4. Commit and push capsule-radar.
5. Create and push tag from capsule-radar, e.g.:

   ```powershell
   git -C D:\git\Arduino\capsule-radar tag v2.0.12
   git -C D:\git\Arduino\capsule-radar push origin v2.0.12
   ```

6. Watch the release workflow:

   ```powershell
   gh -R Lerxtwood/capsule-radar run list --workflow release.yml --limit 3
   gh -R Lerxtwood/capsule-radar run watch <run-id> --exit-status
   ```

The workflow checks out `Lerxtwood/PrintSphere`, builds both firmwares, publishes release assets, and deploys the web installer.

## Build / flash commands

### PrintSphere local build

From `D:\git\Arduino\PrintSphere`:

```powershell
C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "& 'C:\Users\chris\esp\esp-idf-v5.5.4\export.ps1'; idf.py -D PRINTSPHERE_HW_VARIANT=amoled_1_75 build"
```

Note: the local CMake cache may preserve an old `PRINTSPHERE_RELEASE_VERSION`; for release builds, GitHub uses a clean checkout.

### PrintSphere slot flash

```powershell
C:\Users\chris\.espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe -m esptool --chip esp32s3 -p COM5 -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m 0x610000 D:\git\Arduino\PrintSphere\build\printsphere_idf.bin
```

### Capsule Radar local build/upload

Use PlatformIO from `D:\git\Arduino\capsule-radar`.

The environment used most often has been:

```powershell
C:\Users\chris\.platformio\penv\Scripts\platformio.exe run -e esp32-s3-amoled-175
```

Check `platformio.ini` for exact upload/OTA environments.

### Preferred local flashing for the companion layout

Do **not** use plain `pio run -t upload` for routine device testing if you want Radar/PrintSphere switching to keep working.

Instead, use the local helper script in `D:\git\Arduino\capsule-radar\scripts\flash_companion_layout.ps1`.

Normal Radar-only dev flash, preserving the companion layout:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File D:\git\Arduino\capsule-radar\scripts\flash_companion_layout.ps1 -Mode radar -Port COM5
```

PrintSphere slot only:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File D:\git\Arduino\capsule-radar\scripts\flash_companion_layout.ps1 -Mode printsphere -Port COM5
```

Full repair/reinstall of the complete companion layout:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File D:\git\Arduino\capsule-radar\scripts\flash_companion_layout.ps1 -Mode full -Port COM5
```

What the helper writes:

- `full`
  - bootloader at `0x0`
  - partition table at `0x8000`
  - OTA data at `0x109000`
  - Capsule Radar at `0x110000`
  - PrintSphere at `0x610000`
- `radar`
  - Capsule Radar only at `0x110000`
- `printsphere`
  - PrintSphere only at `0x610000`

## Current app switching behavior

The device can switch among:

- Radar
- TamaPoke
- PrintSphere

The combined firmware handles Radar/TamaPoke in the main slot. PrintSphere is a separate app slot. UI has top buttons/gestures for switching.

PrintSphere has a top-center `Radar` button/area to switch back to the combined firmware. Radar has top buttons for TamaPoke and Printer. TamaPoke has a top return-to-radar affordance, though it can be subtle visually.

## Recent Capsule Radar work

Most recent focus after the earlier route work: aircraft route display, aircraft photo UX, and fallback image behavior.

Changes now included through `v2.0.16`:

- FlightAware HTML scraping is the primary route source, with ADS-B based fallback still available.
- Route card now uses two lines:
  - line 1 compact ICAO route plus source, for example `KLAX -> KORD (FlightAware)`
  - line 2 verbose airport names, wrapped as needed
- Aircraft photo detail view now supports tap-to-zoom:
  - tap thumbnail to open large image
  - tap image again to close it
  - tap outside the zoomed image to dismiss the whole detail card
- Aircraft photo fetching was simplified:
  - fetch the full-size image once
  - derive the thumbnail from that cached full image
  - reuse the cached full image for zoom instead of performing a second fetch on tap
- Full-size aircraft images are now cached with the aircraft photo slot, so repeat zooms do not require another network request unless that aircraft ages out of the photo cache.
- A bundled public-domain Wright Flyer placeholder image is embedded in firmware and used when no aircraft photo is available or when the network/photo fetch path fails.
- The Wright Flyer placeholder was later cropped tighter around the aircraft to remove the white photo-holder corner visible in the original Library of Congress image.

Relevant files:

- `D:\git\Arduino\capsule-radar\src\photo.cpp`
- `D:\git\Arduino\capsule-radar\src\photo.h`
- `D:\git\Arduino\capsule-radar\src\photo_client.cpp`
- `D:\git\Arduino\capsule-radar\src\ui.cpp`
- `D:\git\Arduino\capsule-radar\platformio.ini`
- `D:\git\Arduino\capsule-radar\assets\wright_placeholder.jpg`

Helpful log messages to watch:

- `[route] ... (fa-...)` for FlightAware-derived routes
- `[photo] ... thumb ... full ... by ...` for normal image success
- `[photo] ... cached full ...` when a full-size image is reused for zoom
- `[photo] ... using bundled Wright Flyer placeholder` when the built-in fallback image is used

## Recent PrintSphere work

Most recent focus: improving multi-color print state handling and progress behavior.

Changes completed before `v2.0.11`:

- Faster recovery from stale `Changing filament` state by watching newer Bambu `snow` / tray signals.
- Avoid showing stale `100%` overall progress at the start of a new print.
- Reset progress, layer counters, and related state when a new active print starts.
- Added extra reset path for printing the same job again: stale complete progress plus active/pre-print stage now resets to zero even if filename/job name is unchanged.

Relevant files:

- `D:\git\Arduino\PrintSphere\main\src\bambu_cloud_client.cpp`
- `D:\git\Arduino\PrintSphere\main\src\printer_client.cpp`
- `D:\git\Arduino\PrintSphere\main\src\status_resolver.cpp`
- `D:\git\Arduino\PrintSphere\main\src\ui.cpp`

Helpful log messages to watch:

- `Clearing stale cloud filament stage after snow settled`
- `Resetting print progress for new cloud job`
- `Resetting stale 100% cloud progress for active/pre-print update`
- Local equivalents may appear if LAN/local MQTT is the active source.

## Known / watch-list issues

- `tmp\flightaware_ual1941.html` is an untracked scratch/debug artifact in `capsule-radar`. It was intentionally left out of the release commit.
- FlightAware scraping works well enough to keep, but it is still inherently brittle because the source is HTML and may need future maintenance if the page structure changes.
- The aircraft photo system now caches full-size images more aggressively. If memory pressure or eviction behavior becomes suspicious later, revisit the photo slot count and full-image dimensions in `src\photo.cpp`.
- `Changing filament` now returns to `printing` much faster, but may be slightly premature. This was accepted as likely okay during testing.
- The initial print progress reset needs more real-world testing, especially:
  - printing a second copy of the same file,
  - starting a print while the previous cloud state still looks active,
  - early multi-color prints where filament changes happen on layer 1.
- PrintSphere thermal behavior may need a future pass. User observed noticeable heat from the metal case while PrintSphere is running. No change was made yet, but likely contributors to revisit are: light sleep disabled, default brightness around 80%, USB power save defaulting off, and always-awake behavior during active prints.
- PrintSphere release/build time is expected to be longer now because the capsule-radar workflow builds both PlatformIO firmware and ESP-IDF PrintSphere firmware, then deploys Pages.
- GitHub Actions may show a Node.js deprecation warning for upstream actions; this has not blocked releases.

## Design preferences / user expectations

- Stability matters more than cleverness.
- Web installer is preferred for firmware installation/update because it updates all slots consistently.
- User likes polished UI on the AMOLED display and is open to fancy visuals, but only if they do not destabilize core status tracking.
- For Capsule Radar:
  - route correctness matters more than strict purity of data source
  - showing a useful fallback is better than showing nothing
  - smoother UI matters, especially around aircraft detail selection and photo zoom behavior
- For PrintSphere status:
  - pre-print state artwork/themes exist,
  - main screen can show part preview while printing,
  - layer progress estimator is acceptable when close enough,
  - nozzle target temps were removed because they made the UI jumpy/wrappy,
  - bed/chamber target temps are useful and should remain.

## If starting fresh

Begin by checking:

```powershell
git -C D:\git\Arduino\capsule-radar status --short --branch
git -C D:\git\Arduino\PrintSphere status --short --branch
gh -R Lerxtwood/capsule-radar release view --json tagName,url
```

Then ask what the user is seeing on the current firmware before making changes.
