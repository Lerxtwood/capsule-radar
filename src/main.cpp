// Capsule Radar — entry point / glue. SKELETON: TODOs mark what to implement.
// Order of work is in CLAUDE.md (milestones). Bring up the Waveshare demo first.
#include <Arduino.h>
#include <WiFi.h>
#include <vector>
#include "config.h"
#include "aircraft.h"
#include "geo.h"
#include "adsb_client.h"
#include "route.h"
#include "route_client.h"
#include "photo.h"
#include "photo_client.h"
#include "radar_view.h"
#include "ui.h"
#include "display.h"                  // M0: CO5300 + LVGL bring-up
#include "imu_qmi8658.h"             // face-down sleep
#include "gps.h"                     // LC76G GNSS (-G variant only)
#include "battery.h"                 // AXP2101 battery gauge
#include "rtc_pcf85063.h"            // PCF85063 RTC (offline clock + date)
#include "audio.h"                   // ES8311 alert pings
#include "tamapoke/tamapoke_app.h"   // guest pet app
#include "tamapoke/sdmon.h"          // TamaPoke SD sprite loader (PUT/LS over serial)
#include <set>                       // audio: track which contacts are in range
#include <string>
#include <WiFiManager.h>             // captive portal
#include <Preferences.h>            // NVS (persist theme/settings)
#include <time.h>                   // NTP/RTC clock + date
#include <WebServer.h>              // configuration web page
#include <HTTPClient.h>             // remote release manifest + firmware download
#include <WiFiClientSecure.h>       // HTTPS for remote firmware download
#include <ESPmDNS.h>                // http://capsuleradar.local
#include <ArduinoOTA.h>             // OTA firmware update over WiFi (PlatformIO/espota)
#include <Update.h>                 // browser OTA: self-flash an uploaded .bin
#include <esp_heap_caps.h>          // largest-free-block metric (heap health)
#include <mbedtls/sha256.h>         // verify downloaded firmware before flashing

// ---- shared state ----
static std::vector<Aircraft> g_aircraft;      // latest snapshot
static SemaphoreHandle_t     g_ac_mutex;      // guards g_aircraft
static volatile bool         g_acDirty = false; // set when a new snapshot is ready
static AdsbClient            g_adsb;
static RadarSettings         g_settings;
static WiFiManager           g_wm;
static int                   g_brightnessDay = BRIGHTNESS_DEFAULT;   // user brightness (web/NVS)
static int                   g_volume = 60;                          // alert volume 0..100 (web/NVS)
static bool                  g_muted  = false;                       // mute alert pings
static bool                  g_quietHours = false;                   // mute radar sounds between quiet start/end
static int                   g_quietStartMin = 22 * 60;              // minutes after midnight, local time
static int                   g_quietEndMin = 8 * 60;                 // minutes after midnight, local time
static int                   g_alertMode = 2;                        // 0=off 1=emergencies 2=new+emergencies (web/NVS)
static float                 g_proximityKm = 0.0f;                   // proximity alert radius, km (0=off) (web/NVS)
static uint32_t              g_idleDimMs = IDLE_DIM_MS;              // dim after this idle time (0 = never)
static bool                  g_showSweep = true;                     // rotating sweep line on/off (web/NVS)
static bool                  g_prefetchDetails = true;               // preload route/photo for new in-range aircraft
static bool                  g_genericPhotos = true;                 // type-based Wikimedia fallback when exact photo is absent
static int                   g_units = 0;                            // 0=Aviation 1=Metric 2=Imperial (web/NVS)
static int                   g_appMode = 0;                          // 0=radar 1=TamaPoke (web/NVS)
static int                   g_tamapokeRotation = 0;                 // TamaPoke rotation 0/1/2/3 = 0/90/180/270
static bool                  g_tamapokeDimOnUsb = false;             // idle-dim TamaPoke even when plugged into USB
static bool                  g_tamapokeReady = false;                // guest app initialized?
static volatile int          g_pendingAppMode = -1;                  // defer app switches out of the web handler
static volatile bool         g_pendingAppSave = false;               // save requested for deferred app switch
static bool                  g_tamapokeBootGuard = false;            // saved guest app boot is still proving itself
static uint32_t              g_tamapokeBootGuardUntilMs = 0;
static bool                  g_spriteLoaderSdStarted = false;        // lazy-mount SD when Web Serial uploads sprites
static bool                  g_showAirports = true;                  // airport markers on/off (web/NVS)
static int                   g_rotation = 0;                         // display rotation 0/1/2/3 = 0/90/180/270 (web/NVS)
static int                   g_rotationOffset = 0;                   // fine display rotation adjustment, degrees (web/NVS)
static bool                  g_useGps = false;                       // auto-set home from the LC76G GPS (-G variant) (web/NVS)
static int                   g_trailLen = 2;                         // aircraft trails 0=off 1=short 2=med 3=long (web/NVS)
static int                   g_trackingFontSize = 0;                 // aircraft floating labels 0=small 1=medium 2=large
static volatile bool         g_onBattery = false;                    // discharging (set on core 1, read on core 0)
static bool                  g_rtcSynced = false;                    // RTC written from NTP this session?
static std::vector<Aircraft> g_snap;                                 // last snapshot (instant re-render on zoom)
static volatile bool         g_requery = false;                      // range changed -> adsb_task re-begins
static float                 g_requeryKm = 0.0f;
static volatile bool         g_feedOk = true;                        // ADS-B feed healthy? (HUD warning)
static volatile uint32_t     g_lastFeedOkMs = 0;                     // millis() of the last good poll (HUD staleness)
static volatile uint32_t     g_rebootAtMs = 0;                       // !=0: reboot when millis() reaches it (clean start after WiFi config)
static volatile bool         g_firmwareUpdateInProgress = false;     // pause background network work while self-flashing
static bool                  g_firmwareUpdateMode = false;           // lightweight boot mode for GitHub TLS + self-update
static String                g_tz = TZ_STR;                          // POSIX timezone (web-configurable, NVS); applied via configTzTime
struct PrefetchTrack { std::string hex, call; uint32_t startedMs; };
static std::vector<PrefetchTrack> g_prefetching;

// Web-selectable time zones (label + POSIX TZ). The <option> value is the index; the save
// handler maps it back to the POSIX string stored in NVS and used by configTzTime at boot.
// (Index avoids putting POSIX strings with '<>' / ',' into HTML attributes.)
// offMin = standard (winter) UTC offset in minutes; dst = 1 if the zone observes DST.
// The web page uses these to auto-pick the visitor's zone from their browser clock.
static const struct { const char *label; const char *tz; int offMin; int dst; } TZOPTS[] = {
    {"UTC",                      "UTC0",                              0, 0},
    {"London / Lisbon",          "GMT0BST,M3.5.0/1,M10.5.0",          0, 1},
    {"Madrid / Paris / Berlin",  "CET-1CEST,M3.5.0,M10.5.0/3",       60, 1},
    {"Athens / Helsinki",        "EET-2EEST,M3.5.0/3,M10.5.0/4",     120, 1},
    {"New York (US Eastern)",    "EST5EDT,M3.2.0,M11.1.0",          -300, 1},
    {"Chicago (US Central)",     "CST6CDT,M3.2.0,M11.1.0",          -360, 1},
    {"Denver (US Mountain)",     "MST7MDT,M3.2.0,M11.1.0",          -420, 1},
    {"Phoenix (Arizona)",        "MST7",                            -420, 0},
    {"Los Angeles (US Pacific)", "PST8PDT,M3.2.0,M11.1.0",          -480, 1},
    {"Anchorage (Alaska)",       "AKST9AKDT,M3.2.0,M11.1.0",        -540, 1},
    {"Honolulu (Hawaii)",        "HST10",                           -600, 0},
    {"Argentina / Brazil (E)",   "<-03>3",                          -180, 0},
    {"India (IST)",              "<+0530>-5:30",                     330, 0},
    {"China / Singapore",        "<+08>-8",                          480, 0},
    {"Japan / Korea",            "JST-9",                            540, 0},
    {"Sydney (AU Eastern)",      "AEST-10AEDT,M10.1.0,M4.1.0/3",     600, 1},
    {"Auckland (NZ)",            "NZST-12NZDT,M9.5.0,M4.1.0/3",      720, 1},
};
static const int TZOPTS_N = sizeof(TZOPTS) / sizeof(TZOPTS[0]);

static const char *RELEASE_MANIFEST_URL = "https://github.com/Lerxtwood/capsule-radar/releases/latest/download/manifest.json";
static const uint32_t MIN_FIRMWARE_SIZE_BYTES = 512UL * 1024UL;

// ---- networking task (core 0): fetch + parse, never touches the display ----
static void adsb_task(void*) {
    std::vector<Aircraft> fresh;
    bool wasConnected = false;
    uint32_t lastPoll = 0;
    uint32_t lastFeedOk = millis();          // self-heal: time of last good (or no-WiFi) poll
    for (;;) {
        const bool conn = (WiFi.status() == WL_CONNECTED);
        if (g_firmwareUpdateInProgress) {
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }
        if (conn && !wasConnected) {
            Serial.printf("[adsb] WiFi up, IP %s\n", WiFi.localIP().toString().c_str());
            configTzTime(g_tz.c_str(), "pool.ntp.org", "time.nist.gov");  // local time (web-configurable TZ)
            Serial.println("[web] config: http://capsuleradar.local/  (or the IP above)");
            // mDNS + OTA are started on core 1 (loop) to keep all mDNS use on one core
        }
        wasConnected = conn;
        // self-heal: a long feed outage while WiFi is up usually means the internal heap
        // fragmented and the TLS handshake can't allocate -> reboot to recover (settings persist).
        if (!conn) lastFeedOk = millis();
        else if (millis() - lastFeedOk > 180000UL) {
            Serial.println("[adsb] feed stuck >180s with WiFi up -> restarting to recover");
            delay(100);
            ESP.restart();
        }
        if (g_requery) {                          // display range changed (double-tap zoom)
            g_adsb.begin(g_settings.homeLat, g_settings.homeLon, g_requeryKm);
            g_requery = false;
            lastPoll = 0;                         // poll immediately at the new radius
        }
        if (conn) {
            // The live aircraft feed is the primary job, so poll FIRST every cycle. That keeps
            // it refreshing even while the user taps around — a slow route/photo lookup (below)
            // can block this single network task, so it must never get ahead of the feed.
            const uint32_t nowMs = millis();
            const uint32_t pollInterval = g_onBattery ? POLL_INTERVAL_BATTERY_MS : POLL_INTERVAL_MS;
            if (lastPoll == 0 || nowMs - lastPoll >= pollInterval) {  // aircraft feed
                lastPoll = nowMs;
                static int failCount = 0;
                // poll() flips to the alternate host on failure, so consecutive polls already
                // alternate hosts; a single transient miss is absorbed by the failCount window.
                if (g_adsb.poll(fresh)) {
                    Serial.printf("[adsb] fetched %u aircraft\n", (unsigned)fresh.size());
                    failCount = 0;
                    g_feedOk = true;
                    lastFeedOk = nowMs;
                    g_lastFeedOkMs = nowMs;          // HUD: mark data as fresh
                    if (xSemaphoreTake(g_ac_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
                        g_aircraft.swap(fresh);   // O(1) handoff: no per-Aircraft String copies under the lock
                        g_acDirty = true;
                        xSemaphoreGive(g_ac_mutex);
                    }
                } else {
                    Serial.println("[adsb] poll failed");
                    if (++failCount >= 5) g_feedOk = false;   // sustained outage -> HUD warning
                }
            }
            // Then the on-demand lookups for the selected aircraft. Their timeouts are kept
            // short (see photo_client / route_client) so a slow photo server can't freeze the
            // feed for long; the next loop iteration polls again as soon as they return.
            char wantCall[12];
            if (route_pending(wantCall, sizeof(wantCall))) {
                char from[40] = "", to[40] = "";
                if (route_cache_get(wantCall, from, sizeof(from), to, sizeof(to))) {
                    route_store(wantCall, from, to);                       // NVS hit, no network
                    Serial.printf("[route] %s (cache): '%s' -> '%s'\n", wantCall, from, to);
                } else if (route_fetch(wantCall, from, sizeof(from), to, sizeof(to))) {
                    route_store(wantCall, from, to);
                    route_cache_put(wantCall, from, to);                  // remember across reboots
                    Serial.printf("[route] %s (net): '%s' -> '%s'\n", wantCall, from, to);
                } else {
                    route_store(wantCall, from, to);   // empty -> don't refetch this session
                    Serial.printf("[route] %s: no route\n", wantCall);
                }
            }
            char wantHex[10], wantType[8];
            if (photo_pending(wantHex, sizeof(wantHex), wantType, sizeof(wantType)))
                photo_fetch(wantHex, g_genericPhotos ? wantType : "");
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

static void loadSettings() {
    Preferences p;
    p.begin("capsuleradar", true);
    g_settings.homeLat = p.getDouble("homeLat", HOME_LAT_DEFAULT);
    g_settings.homeLon = p.getDouble("homeLon", HOME_LON_DEFAULT);
    g_settings.rangeKm = p.getFloat("rangeKm", RANGE_KM_DEFAULT);
    g_brightnessDay    = p.getInt("bright", BRIGHTNESS_DEFAULT);
    g_volume           = p.getInt("vol", 60);
    g_muted            = p.getBool("mute", false);
    g_quietHours       = p.getBool("quiet", false);
    g_quietStartMin    = constrain(p.getInt("qstart", 22 * 60), 0, 1439);
    g_quietEndMin      = constrain(p.getInt("qend", 8 * 60), 0, 1439);
    g_alertMode        = p.getInt("alertmode", 2);
    g_proximityKm      = p.getFloat("proxkm", 0.0f);
    g_useGps           = p.getBool("usegps", false);
    g_trailLen         = p.getInt("traillen", 2);
    g_trackingFontSize = constrain(p.getInt("trackfont", 0), 0, 2);
    g_idleDimMs        = p.getUInt("idledim", IDLE_DIM_MS);
    g_units            = p.getInt("units", 0);
    g_prefetchDetails  = p.getBool("prefetch", true);
    g_genericPhotos    = p.getBool("genericpic", true);
    g_tz               = p.getString("tz", TZ_STR);
    p.end();

    Preferences appp;
    appp.begin("cr_app", true);
    g_appMode = constrain(appp.getInt("mode", -1), -1, 1);
    g_tamapokeRotation = constrain(appp.getInt("rot", 0), 0, 3);
    g_tamapokeDimOnUsb = appp.getBool("dimonusb", false);
    const bool appBootGuard = appp.getBool("bootguard", false);
    appp.end();
    if (g_appMode < 0) {
        // One-time compatibility fallback from the earlier merge-test builds.
        p.begin("capsuleradar", true);
        g_appMode = constrain(p.getInt("app", 0), 0, 1);
        p.end();
    }
    Serial.printf("[app] loaded preference -> %s (bootGuard=%d)\n",
                  g_appMode == 1 ? "TamaPoke" : "Capsule Radar",
                  appBootGuard ? 1 : 0);
}

static void minutesToTimeValue(int minutes, char *out, size_t outLen) {
    minutes = constrain(minutes, 0, 1439);
    snprintf(out, outLen, "%02d:%02d", minutes / 60, minutes % 60);
}

static int parseTimeToMinutes(const String &value, int fallback) {
    if (value.length() < 4) return fallback;
    const int colon = value.indexOf(':');
    if (colon <= 0) return fallback;
    const int hh = value.substring(0, colon).toInt();
    const int mm = value.substring(colon + 1).toInt();
    if (hh < 0 || hh > 23 || mm < 0 || mm > 59) return fallback;
    return hh * 60 + mm;
}

static bool quietHoursActiveNow() {
    if (!g_quietHours || g_quietStartMin == g_quietEndMin) return false;
    struct tm ti;
    if (!getLocalTime(&ti, 0)) return false;   // fail open until RTC/NTP gives us a clock
    const int nowMin = ti.tm_hour * 60 + ti.tm_min;
    if (g_quietStartMin < g_quietEndMin) {
        return nowMin >= g_quietStartMin && nowMin < g_quietEndMin;
    }
    return nowMin >= g_quietStartMin || nowMin < g_quietEndMin;  // crosses midnight
}

static bool radarSoundAllowed() {
    return audio_present() && !g_muted && !quietHoursActiveNow() && g_volume > 0;
}

// Audio alerts. g_alertMode: 0 = off, 1 = emergencies only, 2 = new aircraft + emergencies.
// g_proximityKm > 0 also pings (once) when any aircraft crosses into that radius.
static void checkAudioEvents() {
    const bool canAudio = radarSoundAllowed();
    static std::set<std::string> seen, seenProx;
    static bool first = true;
    static uint32_t lastNew = 0;
    std::set<std::string> now, nowProx;
    for (const Aircraft &ac : g_snap) {
        const double d = geo::haversineKm(g_settings.homeLat, g_settings.homeLon, ac.lat, ac.lon);
        if (d > g_settings.rangeKm) continue;                 // in-range only
        const std::string hex = ac.hex.c_str();
        now.insert(hex);
        const bool isNew     = !first && !seen.count(hex);
        const bool emergency = acIsEmergency(ac.squawk) || ac.military;  // military: feed dbFlags

        // proximity: fire once, when an aircraft first crosses into the radius (any aircraft)
        if (g_proximityKm > 0.0f && d <= g_proximityKm) {
            nowProx.insert(hex);
            if (canAudio && !first && !seenProx.count(hex)) audio_play(AUDIO_ALERT);
        }

        // new-in-range pings (on entry), gated by the alert mode
        if (isNew) {
            if (g_prefetchDetails) {
                if (ac.flight.length()) route_prefetch(ac.flight.c_str());
                photo_prefetch(ac.hex.c_str(), ac.type.c_str());
                bool tracked = false;
                for (PrefetchTrack &p : g_prefetching) {
                    if (p.hex != hex) continue;
                    p.call = ac.flight.c_str(); p.startedMs = millis(); tracked = true; break;
                }
                if (!tracked) g_prefetching.push_back({hex, ac.flight.c_str(), millis()});
                radar::setPrefetching(ac.hex.c_str(), true);
                Serial.printf("[prefetch] queued %s / %s\n", ac.flight.c_str(), ac.hex.c_str());
            }
            if (canAudio && emergency) { if (g_alertMode >= 1) audio_play(AUDIO_ALERT); }   // emergencies only / +new
            else if (canAudio && g_alertMode >= 2 && millis() - lastNew > 3000) {
                audio_play(AUDIO_NEW);                                          // new contact (rate-limited)
                lastNew = millis();
            }
        }
    }
    seen.swap(now);
    seenProx.swap(nowProx);
    first = false;
}

static void updatePrefetchIndicators() {
    static uint32_t lastCheck = 0;
    if (millis() - lastCheck < 200) return;
    lastCheck = millis();
    for (size_t i = 0; i < g_prefetching.size();) {
        char from[40], to[40];
        const bool routeDone = g_prefetching[i].call.empty() ||
            route_get(g_prefetching[i].call.c_str(), from, sizeof(from), to, sizeof(to));
        const bool photoDone = photo_done(g_prefetching[i].hex.c_str());
        const uint32_t elapsed = millis() - g_prefetching[i].startedMs;
        const bool timedOut = elapsed > 60000UL;
        const bool completed = (routeDone && photoDone) && elapsed >= 1500UL;
        if (completed || timedOut) {
            radar::setPrefetching(g_prefetching[i].hex.c_str(), false);
            if (completed) ui_preview_aircraft(g_prefetching[i].hex.c_str(), 5000);
            g_prefetching.erase(g_prefetching.begin() + i);
        } else ++i;
    }
}

// Double-tap zoom: change the display range, persist it, and ask adsb_task to
// re-query at a matching radius (safely, on its own core). Re-render immediately.
static void persistAppMode(int mode);
static void onRangeChange(float km) {
    g_settings.rangeKm = km;
    Preferences p;
    p.begin("capsuleradar", false);
    p.putFloat("rangeKm", km);
    p.end();
    g_requeryKm = constrain(km * 1.6f, 50.0f, 200.0f);
    g_requery = true;
    radar::update(g_snap, g_settings);   // instant visual zoom from the last snapshot
    ui_set_range_km(km);
    ui_on_data_updated();
}

static void requestTamapokeFromRadar() {
    Serial.println("[app] radar top-center gesture -> TamaPoke");
    persistAppMode(1);
    g_pendingAppMode = 1;
    g_pendingAppSave = true;
}

// Persist the visual theme in NVS (called when the user long-presses to switch).
static void saveTheme(int t) {
    Preferences p;
    p.begin("capsuleradar", false);
    p.putInt("theme", t);
    p.end();
}

// Convert a UTC broken-down time to time_t (mktime assumes local TZ, so flip to UTC0).
static time_t utc_to_time(struct tm *utc) {
    setenv("TZ", "UTC0", 1); tzset();
    const time_t t = mktime(utc);
    setenv("TZ", TZ_STR, 1); tzset();   // restore local TZ for getLocalTime()
    return t;
}

// Seed the ESP system clock from the RTC so the clock/date are right before NTP.
static void rtc_seed_clock() {
    struct tm utc;
    if (!rtc_read(&utc)) { Serial.println("[rtc] no valid time stored"); return; }
    const time_t t = utc_to_time(&utc);
    struct timeval tv = { t, 0 };
    settimeofday(&tv, nullptr);
    Serial.println("[rtc] system clock seeded from RTC");
}

// Brightness combines idle auto-dim and face-down sleep (sleep wins -> screen off).
static bool g_asleep = false;   // face-down
static bool g_idle   = false;   // no touch for a while
static void applyBrightness() {
    int b = g_brightnessDay;
    if (g_idle  && BRIGHTNESS_IDLE  < b) b = BRIGHTNESS_IDLE;   // idle only dims down
    if (g_asleep) b = 0;                                         // face-down -> screen off
    display::setBrightness(b);
}

// ----------------------------- configuration web --------------------------------
static WebServer g_web(80);

static void handleRoot() {
    const int th = radar::theme();
    const int ranges[] = {10, 15, 25, 30, 50, 100, 150, 250};
    // The value submitted stays in km (the device works in km); only the label is shown in
    // the user's chosen distance unit so the config page matches the screen.
    const float    ufac  = (g_units == 0) ? 0.539957f : (g_units == 2 ? 0.621371f : 1.0f);
    const char    *uname = (g_units == 0) ? "nm" : (g_units == 2 ? "mi" : "km");
    String ropts;
    for (int r : ranges) {
        char o[72];
        snprintf(o, sizeof(o), "<option value=%d%s>%.0f %s</option>",
                 r, (r == (int)(g_settings.rangeKm + 0.5f)) ? " selected" : "", r * ufac, uname);
        ropts += o;
    }
    const char *tnames[] = {"Phosphor", "Orb", "Amber CRT", "Military"};
    String topts;
    for (int i = 0; i < 4; ++i) {
        char o[80];
        snprintf(o, sizeof(o), "<option value=%d%s>%s</option>", i, i == th ? " selected" : "", tnames[i]);
        topts += o;
    }
    const int idleSecs[] = {10, 20, 30, 60, 120, 300, 1800, 3600, 7200, 14400, 28800};
    const int curIdle = (int)(g_idleDimMs / 1000);
    String iopts;
    for (int sV : idleSecs) {
        char lbl[16];
        if      (sV < 60)   snprintf(lbl, sizeof(lbl), "%d s", sV);
        else if (sV < 3600) snprintf(lbl, sizeof(lbl), "%d min", sV / 60);
        else                snprintf(lbl, sizeof(lbl), "%d h", sV / 3600);
        char o[96];
        snprintf(o, sizeof(o), "<option value=%d%s>%s</option>", sV, sV == curIdle ? " selected" : "", lbl);
        iopts += o;
    }
    { char o[64]; snprintf(o, sizeof(o), "<option value=0%s>Never</option>", curIdle == 0 ? " selected" : ""); iopts += o; }
    const char *unames[] = {"Aviation (ft, kt, nm)", "Metric (m, km/h, km)", "Imperial (ft, mph, mi)"};
    String uopts;
    for (int i = 0; i < 3; ++i) {
        char o[96];
        snprintf(o, sizeof(o), "<option value=%d%s>%s</option>", i, i == g_units ? " selected" : "", unames[i]);
        uopts += o;
    }
    const char *rnames[] = {"0\xc2\xb0 (default)", "90\xc2\xb0", "180\xc2\xb0", "270\xc2\xb0"};
    String rotopts;
    for (int i = 0; i < 4; ++i) {
        char o[64];
        snprintf(o, sizeof(o), "<option value=%d%s>%s</option>", i, i == g_rotation ? " selected" : "", rnames[i]);
        rotopts += o;
    }
    const char *tlnames[] = {"Off", "Short", "Medium", "Long"};
    String tlopts;
    for (int i = 0; i < 4; ++i) {
        char o[64];
        snprintf(o, sizeof(o), "<option value=%d%s>%s</option>", i, i == g_trailLen ? " selected" : "", tlnames[i]);
        tlopts += o;
    }
    const char *tfnames[] = {"Small", "Medium", "Large"};
    String tfopts;
    for (int i = 0; i < 3; ++i) {
        char o[64];
        snprintf(o, sizeof(o), "<option value=%d%s>%s</option>", i, i == g_trackingFontSize ? " selected" : "", tfnames[i]);
        tfopts += o;
    }
    const char *appnames[] = {"Capsule Radar", "TamaPoke"};
    String appopts;
    for (int i = 0; i < 2; ++i) {
        char o[80];
        snprintf(o, sizeof(o), "<option value=%d%s>%s</option>", i, i == g_appMode ? " selected" : "", appnames[i]);
        appopts += o;
    }
    const char *anames[] = {"Off", "Emergencies only", "New aircraft + emergencies"};
    String aopts;
    for (int i = 0; i < 3; ++i) {
        char o[80];
        snprintf(o, sizeof(o), "<option value=%d%s>%s</option>", i, i == g_alertMode ? " selected" : "", anames[i]);
        aopts += o;
    }
    const int proxUnit[] = {0, 2, 5, 10, 25};   // 0 = off; rest in the user's distance unit
    String popts;
    for (int pv : proxUnit) {
        const float pkm = (pv == 0) ? 0.0f : (pv / ufac);   // user unit -> km (value submitted)
        const bool  sel = (pv == 0) ? (g_proximityKm <= 0.0f) : (fabsf(g_proximityKm - pkm) < 0.4f);
        char lbl[24];
        if (pv == 0) snprintf(lbl, sizeof(lbl), "Off");
        else         snprintf(lbl, sizeof(lbl), "%d %s", pv, uname);
        char o[80];
        snprintf(o, sizeof(o), "<option value=%.3f%s>%s</option>", pkm, sel ? " selected" : "", lbl);
        popts += o;
    }
    char quietStart[6], quietEnd[6];
    minutesToTimeValue(g_quietStartMin, quietStart, sizeof(quietStart));
    minutesToTimeValue(g_quietEndMin, quietEnd, sizeof(quietEnd));
    String tzopts;   // time-zone dropdown (value = index into TZOPTS; mapped to POSIX TZ on save)
    for (int i = 0; i < TZOPTS_N; ++i) {
        char o[128];
        snprintf(o, sizeof(o), "<option value=%d data-off=%d data-dst=%d%s>%s</option>",
                 i, TZOPTS[i].offMin, TZOPTS[i].dst, g_tz == TZOPTS[i].tz ? " selected" : "", TZOPTS[i].label);
        tzopts += o;
    }
    String gpsRow;   // only on the -G variant: offer to auto-set the centre from GPS
    if (gps_present()) {
        gpsRow  = "<label><input type=checkbox class=ck ";
        gpsRow += g_useGps ? "checked" : "";
        gpsRow += " onchange='gp(this.checked)'>Use GPS for location</label>";
        gpsRow += "<div style='font-size:12px;opacity:.6;margin:-2px 0 6px'>"
                  "When on, the location above is used until the GPS gets a fix, then it takes over.</div>";
    }
    static const size_t BUFSZ = 14336;
    static char *buf = (char *)ps_malloc(BUFSZ);   // PSRAM: keep this big page buffer off the scarce
    if (!buf) return;                              //   internal heap (the contiguous RAM mbedTLS needs)
    snprintf(buf, BUFSZ,
        "<!DOCTYPE html><html><head><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>Capsule Radar</title>"
        "<link rel=stylesheet href='https://unpkg.com/leaflet@1.9.4/dist/leaflet.css'>"
        "<script src='https://unpkg.com/leaflet@1.9.4/dist/leaflet.js'></script>"
        "<style>"
        "*{box-sizing:border-box}"
        "body{background:radial-gradient(circle at 50%% -10%%,#0a1f15,#04100a 70%%);color:#cdd6d1;"
        "font-family:system-ui,-apple-system,sans-serif;margin:0 auto;padding:20px;max-width:480px;min-height:100vh}"
        ".hd{display:flex;align-items:center;gap:12px;margin-bottom:16px}"
        ".dot{width:44px;height:44px;border-radius:50%%;border:2px solid #1dff86;position:relative;"
        "overflow:hidden;flex:0 0 auto;box-shadow:0 0 16px rgba(29,255,134,.4)}"
        ".dot::before{content:'';position:absolute;inset:0;animation:sw 3s linear infinite;"
        "background:conic-gradient(from 0deg,rgba(29,255,134,.65),transparent 55%%)}"
        "@keyframes sw{to{transform:rotate(360deg)}}"
        "h1{color:#1dff86;font-size:20px;margin:0}.sub{color:#6f8c7d;font-size:12px;margin:2px 0 0}"
        ".t{color:#1dff86;font-size:11px;letter-spacing:1.5px;text-transform:uppercase;margin-bottom:10px;opacity:.85}"
        "label{display:block;margin:12px 0 4px;color:#9affc8;font-size:13px}"
        "input,select{width:100%%;box-sizing:border-box;padding:10px;border-radius:8px;border:1px solid #2a4a39;"
        "background:#0c1a12;color:#eafff3;font-size:16px}"
        "input:focus,select:focus{outline:none;border-color:#1dff86;box-shadow:0 0 0 2px rgba(29,255,134,.18)}"
        "button{margin-top:16px;width:100%%;padding:12px;border:0;border-radius:8px;background:#1dff86;"
        "color:#04140b;font-weight:700;font-size:16px}button:active{opacity:.85}"
        ".w{background:#ffb23c}.card{background:rgba(10,20,14,.85);border:1px solid #1f3a2b;border-radius:14px;padding:16px;margin-bottom:14px}"
        ".ft{color:#5f7a6c;font-size:12px;text-align:center;margin-top:6px}.ft code{color:#9affc8}"
        ".ck{width:auto;display:inline;margin-right:8px;vertical-align:middle}"
        ".sec{background:#0c1a12!important;color:#1dff86!important;border:1px solid #2a4a39!important}"
        ".nav{display:flex;gap:8px;margin:-4px 0 14px}.nav a{flex:1;text-align:center;text-decoration:none;"
        "padding:9px 8px;border:1px solid #2a4a39;border-radius:9px;color:#9affc8;background:#0c1a12}"
        ".nav a.on{background:#1dff86;color:#04140b;border-color:#1dff86;font-weight:700}"
        "#map{height:220px;border-radius:10px;margin:6px 0 8px;border:1px solid #2a4a39;z-index:0}"
        "</style></head><body>"
        "<div class=hd><div class=dot></div><div><h1>Capsule Radar</h1><p class=sub>Live ADS-B radar &middot; configuration</p></div></div>"
        "<nav class=nav><a class=on href=/>Capsule-Radar</a><a href=/tamapoke>TamaPoke</a><a href=/update>Firmware</a></nav>"
        "<div class=card><div class=t>Location &amp; range</div><form method=POST action=/save>"
        "<label>Center point &mdash; tap the map or drag the pin</label>"
        "<div id=map></div>"
        "<label>Center latitude</label><input id=lat name=lat value='%.5f'>"
        "<label>Center longitude</label><input id=lon name=lon value='%.5f'>"
        "%s"
        "<label>Display range</label><select name=range>%s</select>"
        "<label>Theme</label><select name=theme>%s</select>"
        "<label>Time zone</label><select name=tz>%s</select>"
        "<button>Save &amp; restart</button></form></div>"
        "<div class=card><div class=t>Display</div>"
        "<label>Brightness</label>"
        "<input type=range min=5 max=255 value='%d' oninput='b(this.value,0)' onchange='b(this.value,1)'>"
        "<label>Dim screen after</label><select onchange='d(this.value)'>%s</select>"
        "<label>Active app</label><select onchange='app(this.value)'>%s</select>"
        "<label><input type=checkbox class=ck %s onchange='sw(this.checked)'>Show radar sweep</label>"
        "<label><input type=checkbox class=ck %s onchange='pf(this.checked)'>Preload aircraft details</label>"
        "<label><input type=checkbox class=ck %s onchange='gf(this.checked)'>Use generic aircraft photos</label>"
        "<label><input type=checkbox class=ck %s onchange='ap(this.checked)'>Show airports</label>"
        "<label>Aircraft trails</label><select onchange='tl(this.value)'>%s</select>"
        "<label>Tracking label font</label><select onchange=\"fetch('/tracking-font?v='+this.value+'&save=1')\">%s</select>"
        "<label>Screen rotation (USB-C position)</label><select onchange='ro(this.value)'>%s</select>"
        "<label>Rotation offset (degrees)</label><input type=number min=-45 max=45 step=1 value='%d' onchange='rf(this.value)'>"
        "<label>Units</label><select onchange='u(this.value)'>%s</select></div>"
        "<div class=card><div class=t>Sound</div>"
        "<label>Volume</label>"
        "<input type=range min=0 max=100 value='%d' oninput='v(this.value,0)' onchange='v(this.value,1)'>"
        "<label><input type=checkbox class=ck %s onchange='m(this.checked)'>Mute alerts</label>"
        "<label><input id=quiet type=checkbox class=ck %s onchange='qh()'>Quiet hours</label>"
        "<div style='display:grid;grid-template-columns:1fr 1fr;gap:8px'>"
        "<div><label>Quiet start</label><input id=qs type=time value='%s' onchange='qh()'></div>"
        "<div><label>Quiet end</label><input id=qe type=time value='%s' onchange='qh()'></div></div>"
        "<p style='color:#6f8c7d;font-size:12px;margin:6px 0 0'>Mutes radar beeps during this local-time window, including overnight ranges.</p>"
        "<label>Alert on</label><select onchange='al(this.value)'>%s</select>"
        "<label>Proximity alert</label><select onchange='px(this.value)'>%s</select>"
        "<button type=button class=sec onclick='t()'>Test ping</button></div>"
        "<div class=card><div class=t>Network</div>"
        "<p style='color:#9affc8;font-size:13px;margin:0 0 4px'>Forget the saved WiFi and reopen the setup portal.</p>"
        "<form method=POST action=/wifi><button class=w>Reset WiFi</button></form></div>"
        "<p class=ft>Reach me at <code>capsuleradar.local</code> &middot; v" FW_VERSION "</p>"
        "<script>"
        "var C=[%.5f,%.5f];var MAP=L.map('map').setView(C,10);"
        "L.tileLayer('https://tile.openstreetmap.org/{z}/{x}/{y}.png',{maxZoom:19,attribution:'(c) OpenStreetMap'}).addTo(MAP);"
        "var MK=L.marker(C,{draggable:true}).addTo(MAP);"
        "function S(p){document.getElementById('lat').value=p.lat.toFixed(5);document.getElementById('lon').value=p.lng.toFixed(5);}"
        "MK.on('dragend',function(){S(MK.getLatLng());});"
        "MAP.on('click',function(e){MK.setLatLng(e.latlng);S(e.latlng);});"
        "setTimeout(function(){MAP.invalidateSize();},300);"
        "function b(v,s){fetch('/bright?v='+v+(s?'&save=1':''))}"
        "function v(x,s){fetch('/vol?v='+x+(s?'&save=1':''))}"
        "function m(c){fetch('/vol?mute='+(c?1:0)+'&save=1')}"
        "function qh(){fetch('/vol?quiet='+(document.getElementById('quiet').checked?1:0)+'&qstart='+document.getElementById('qs').value+'&qend='+document.getElementById('qe').value+'&save=1')}"
        "function t(){fetch('/vol?test=1')}"
        "function d(v){fetch('/idle?v='+v+'&save=1')}"
        "function app(v){fetch('/app?v='+v+'&save=1')}"
        "function sw(c){fetch('/sweep?v='+(c?1:0)+'&save=1')}"
        "function pf(c){fetch('/prefetch?v='+(c?1:0)+'&save=1')}"
        "function gf(c){fetch('/generic-photos?v='+(c?1:0)+'&save=1')}"
        "function ap(c){fetch('/airports?v='+(c?1:0)+'&save=1')}"
        "function tl(v){fetch('/trail?v='+v+'&save=1')}"
        "function tf(v){fetch('/tracking-font?v='+v+'&save=1')}"
        "function ro(v){fetch('/rotate?v='+v+'&save=1')}"
        "function rf(v){fetch('/rotate-offset?v='+v+'&save=1')}"
        "function u(v){fetch('/units?v='+v+'&save=1')}"
        "function al(v){fetch('/alerts?mode='+v+'&save=1')}"
        "function px(v){fetch('/alerts?prox='+v+'&save=1')}"
        "function gp(c){fetch('/gps?v='+(c?1:0)+'&save=1')}"
        // auto-pick the visitor's time zone from their browser clock (only if they haven't set one)
        "var TZSET=%d;(function(){if(TZSET)return;"
        "var d=new Date(),j=new Date(d.getFullYear(),0,1).getTimezoneOffset(),"
        "u=new Date(d.getFullYear(),6,1).getTimezoneOffset(),o=-Math.max(j,u),s=(j!=u)?1:0,"
        "e=document.querySelector('select[name=tz]'),b=-1,i;"
        "for(i=0;i<e.options.length;i++){if(+e.options[i].dataset.off===o&&+e.options[i].dataset.dst===s){b=i;break;}}"
        "if(b<0)for(i=0;i<e.options.length;i++){if(+e.options[i].dataset.off===o){b=i;break;}}"
        "if(b>=0)e.selectedIndex=b;})();</script></body></html>",
        g_settings.homeLat, g_settings.homeLon, gpsRow.c_str(), ropts.c_str(), topts.c_str(),
        tzopts.c_str(),
        g_brightnessDay, iopts.c_str(), appopts.c_str(),
        g_showSweep ? "checked" : "", g_prefetchDetails ? "checked" : "",
        g_genericPhotos ? "checked" : "",
        g_showAirports ? "checked" : "", tlopts.c_str(), tfopts.c_str(), rotopts.c_str(), g_rotationOffset, uopts.c_str(),
        g_volume, g_muted ? "checked" : "", g_quietHours ? "checked" : "",
        quietStart, quietEnd, aopts.c_str(), popts.c_str(),
        g_settings.homeLat, g_settings.homeLon, (g_tz == TZ_STR ? 0 : 1));
    g_web.send(200, "text/html", buf);
}

static void handleSave() {
    Preferences p;
    p.begin("capsuleradar", false);
    // Reject out-of-range coordinates so a typo can't leave the radar unusable.
    if (g_web.hasArg("lat")) {
        const double lat = g_web.arg("lat").toDouble();
        if (lat >= -90.0 && lat <= 90.0) p.putDouble("homeLat", lat);
    }
    if (g_web.hasArg("lon")) {
        const double lon = g_web.arg("lon").toDouble();
        if (lon >= -180.0 && lon <= 180.0) p.putDouble("homeLon", lon);
    }
    if (g_web.hasArg("range")) p.putFloat("rangeKm", g_web.arg("range").toFloat());
    if (g_web.hasArg("theme")) p.putInt("theme", g_web.arg("theme").toInt());
    if (g_web.hasArg("tz")) {
        const int i = g_web.arg("tz").toInt();
        if (i >= 0 && i < TZOPTS_N) p.putString("tz", TZOPTS[i].tz);
    }
    p.end();
    g_web.send(200, "text/html",
        "<meta http-equiv=refresh content='4;url=/'><body style='background:#06100a;color:#1dff86;"
        "font-family:sans-serif;padding:24px'>Saved. Restarting&hellip;</body>");
    delay(400);
    ESP.restart();
}

static void handleWifi() {
    g_web.send(200, "text/html",
        "<body style='background:#06100a;color:#ffb23c;font-family:sans-serif;padding:24px'>"
        "WiFi reset. Connect to the <b>CapsuleRadar-Setup</b> network to reconfigure.</body>");
    delay(400);
    g_wm.resetSettings();
    ESP.restart();
}

static void handleBright() {
    if (g_web.hasArg("v")) {
        g_brightnessDay = constrain((int)g_web.arg("v").toInt(), 0, 255);
        applyBrightness();
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("capsuleradar", false);
            p.putInt("bright", g_brightnessDay);
            p.end();
        }
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleVol() {
    if (g_web.hasArg("v"))    { g_volume = constrain((int)g_web.arg("v").toInt(), 0, 100); audio_set_volume(g_volume); }
    if (g_web.hasArg("mute")) { g_muted = g_web.arg("mute").toInt() != 0; audio_set_muted(g_muted); }
    if (g_web.hasArg("quiet")) g_quietHours = g_web.arg("quiet").toInt() != 0;
    if (g_web.hasArg("qstart")) g_quietStartMin = parseTimeToMinutes(g_web.arg("qstart"), g_quietStartMin);
    if (g_web.hasArg("qend"))   g_quietEndMin   = parseTimeToMinutes(g_web.arg("qend"), g_quietEndMin);
    if (g_web.hasArg("save")) {
        Preferences p;
        p.begin("capsuleradar", false);
        p.putInt("vol", g_volume);
        p.putBool("mute", g_muted);
        p.putBool("quiet", g_quietHours);
        p.putInt("qstart", g_quietStartMin);
        p.putInt("qend", g_quietEndMin);
        p.end();
    }
    if (g_web.hasArg("test")) {
        if (g_web.arg("test").toInt() == 2) audio_selftest();   // long tone, ignores mute
        else if (radarSoundAllowed()) audio_play(AUDIO_NEW);
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleAlerts() {   // what triggers the alert sound (live)
    if (g_web.hasArg("mode")) g_alertMode   = constrain((int)g_web.arg("mode").toInt(), 0, 2);
    if (g_web.hasArg("prox")) g_proximityKm = g_web.arg("prox").toFloat();   // km (0 = off)
    if (g_web.hasArg("save")) {
        Preferences p;
        p.begin("capsuleradar", false);
        p.putInt("alertmode", g_alertMode);
        p.putFloat("proxkm", g_proximityKm);
        p.end();
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleIdle() {   // idle auto-dim timeout (seconds; 0 = never)
    if (g_web.hasArg("v")) {
        const long s = g_web.arg("v").toInt();
        g_idleDimMs = (s <= 0) ? 0 : (uint32_t)s * 1000;
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("capsuleradar", false);
            p.putUInt("idledim", g_idleDimMs);
            p.end();
        }
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleUnits() {   // measurement units preset (live re-render)
    if (g_web.hasArg("v")) {
        g_units = constrain((int)g_web.arg("v").toInt(), 0, 2);
        ui_set_units(g_units);
        ui_set_range_km(g_settings.rangeKm);   // refresh the zoom-button label
        ui_on_data_updated();                  // re-render card/list/stats in the new units
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("capsuleradar", false);
            p.putInt("units", g_units);
            p.end();
        }
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleSweep() {   // show/hide the rotating sweep line (live)
    if (g_web.hasArg("v")) {
        g_showSweep = g_web.arg("v").toInt() != 0;
        radar::setSweepEnabled(g_showSweep);          // loop()/core 1: safe to touch LVGL
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("capsuleradar", false);
            p.putBool("sweep", g_showSweep);
            p.end();
        }
    }
    g_web.send(200, "text/plain", "ok");
}

static void handlePrefetch() {   // preload route/photo when an aircraft enters range
    if (g_web.hasArg("v")) {
        g_prefetchDetails = g_web.arg("v").toInt() != 0;
        if (!g_prefetchDetails) {
            route_cancel_prefetches();
            photo_cancel_prefetches();
            for (const PrefetchTrack &p : g_prefetching) radar::setPrefetching(p.hex.c_str(), false);
            g_prefetching.clear();
        }
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("capsuleradar", false);
            p.putBool("prefetch", g_prefetchDetails);
            p.end();
        }
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleGenericPhotos() {   // use a type-based generic image when no exact photo exists
    if (g_web.hasArg("v")) {
        g_genericPhotos = g_web.arg("v").toInt() != 0;
        photo_reset_fallback_cache();
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("capsuleradar", false);
            p.putBool("genericpic", g_genericPhotos);
            p.end();
        }
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleTrail() {   // aircraft trail length 0/1/2/3 (live)
    if (g_web.hasArg("v")) {
        g_trailLen = constrain((int)g_web.arg("v").toInt(), 0, 3);
        radar::setTrailLength(g_trailLen);
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("capsuleradar", false);
            p.putInt("traillen", g_trailLen);
            p.end();
        }
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleTrackingFont() {   // aircraft floating label font size 0=small 1=medium 2=large (live)
    if (g_web.hasArg("v")) {
        g_trackingFontSize = constrain((int)g_web.arg("v").toInt(), 0, 2);
        radar::setTrackingFontSize(g_trackingFontSize);
        Serial.printf("[ui] tracking label font -> %d\n", g_trackingFontSize);
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("capsuleradar", false);
            p.putInt("trackfont", g_trackingFontSize);
            p.end();
        }
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleAirports() {   // show/hide airport markers (live)
    if (g_web.hasArg("v")) {
        g_showAirports = g_web.arg("v").toInt() != 0;
        radar::setAirportsEnabled(g_showAirports);
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("capsuleradar", false);
            p.putBool("airports", g_showAirports);
            p.end();
        }
    }
    g_web.send(200, "text/plain", "ok");
}

static bool activateApp(int mode) {
    mode = constrain(mode, 0, 1);
    Serial.printf("[app] switching -> %s\n", mode == 1 ? "TamaPoke" : "Capsule Radar");
    if (mode == 1 && !g_tamapokeReady) {
        g_tamapokeReady = tamapoke_begin();
        if (!g_tamapokeReady) {
            Serial.println("[app] TamaPoke start failed");
            return false;
        }
    }
    if (mode == 1) {
        tamapoke_set_rotation((uint8_t)g_tamapokeRotation);
        tamapoke_set_dim_on_usb(g_tamapokeDimOnUsb);
    }
    g_appMode = mode;
    if (g_appMode == 0) {
        display::invalidate();   // repaint over the last TamaPoke frame
        applyBrightness();
    }
    Serial.printf("[app] active -> %s\n", g_appMode == 1 ? "TamaPoke" : "Capsule Radar");
    return true;
}

static void persistAppMode(int mode) {
    mode = constrain(mode, 0, 1);
    Preferences p;
    if (!p.begin("cr_app", false)) {
        Serial.println("[app] save failed: Preferences.begin(cr_app) failed");
        return;
    }
    const size_t wroteMode = p.putInt("mode", mode);
    const size_t wroteGuard = p.putBool("bootguard", false);
    p.end();
    Serial.printf("[app] saved preference -> %s (modeBytes=%u guardBytes=%u)\n",
                  mode == 1 ? "TamaPoke" : "Capsule Radar",
                  (unsigned)wroteMode, (unsigned)wroteGuard);
}

static bool appBootGuardWasSet() {
    Preferences p;
    if (!p.begin("cr_app", true)) return false;
    const bool wasSet = p.getBool("bootguard", false);
    p.end();
    return wasSet;
}

static void setAppBootGuard(bool active) {
    Preferences p;
    if (!p.begin("cr_app", false)) {
        Serial.println("[app] boot guard save failed: Preferences.begin(cr_app) failed");
        return;
    }
    const size_t wrote = p.putBool("bootguard", active);
    p.end();
    Serial.printf("[app] boot guard -> %d (bytes=%u)\n", active ? 1 : 0, (unsigned)wrote);
}

static void handleApp() {   // switch active full-screen app (live)
    if (g_web.hasArg("v")) {
        const int mode = constrain((int)g_web.arg("v").toInt(), 0, 1);
        // Active app is a durable preference. Persist every explicit switch so
        // stale/cached config pages or missing query args cannot make the UI
        // appear to switch while leaving the next boot on radar.
        persistAppMode(mode);
        g_pendingAppMode = mode;
        g_pendingAppSave = true;
        g_web.send(200, "text/plain", mode == 1 ? "switching TamaPoke" : "switching Capsule Radar");
        return;
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleRotate() {   // display rotation 0/90/180/270 for any USB-C orientation (live)
    if (g_web.hasArg("v")) {
        g_rotation = constrain((int)g_web.arg("v").toInt(), 0, 3);
        display::setRotation((uint8_t)g_rotation);
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("capsuleradar", false);
            p.putInt("rot", g_rotation);
            p.end();
        }
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleRotateOffset() {   // fine display rotation adjustment in degrees (live)
    if (g_web.hasArg("v")) {
        g_rotationOffset = constrain((int)g_web.arg("v").toInt(), -45, 45);
        display::setRotationOffset((int8_t)g_rotationOffset);
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("capsuleradar", false);
            p.putInt("rotoff", g_rotationOffset);
            p.end();
        }
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleGps() {   // auto-set the centre point from the LC76G GPS (-G variant)
    if (g_web.hasArg("v")) {
        g_useGps = g_web.arg("v").toInt() != 0;
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("capsuleradar", false);
            p.putBool("usegps", g_useGps);
            p.end();
        }
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleTamapokePage() {
    const char *rnames[] = {"0\xc2\xb0 (default)", "90\xc2\xb0", "180\xc2\xb0", "270\xc2\xb0"};
    String ropts;
    for (int i = 0; i < 4; ++i) {
        char o[64];
        snprintf(o, sizeof(o), "<option value=%d%s>%s</option>", i, i == g_tamapokeRotation ? " selected" : "", rnames[i]);
        ropts += o;
    }
    static const size_t BUFSZ = 4096;
    static char *buf = (char *)ps_malloc(BUFSZ);
    if (!buf) return;
    snprintf(buf, BUFSZ,
        "<!DOCTYPE html><html><head><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>TamaPoke Settings</title><style>"
        "*{box-sizing:border-box}"
        "body{background:radial-gradient(circle at 50%% -10%%,#1f1530,#090611 70%%);color:#ddd6e8;"
        "font-family:system-ui,-apple-system,sans-serif;margin:0 auto;padding:20px;max-width:480px;min-height:100vh}"
        ".hd{display:flex;align-items:center;gap:12px;margin-bottom:16px}"
        ".dot{width:44px;height:44px;border-radius:50%%;border:2px solid #ff7ad9;box-shadow:0 0 16px rgba(255,122,217,.4);"
        "background:radial-gradient(circle at 35%% 35%%,#fff,#ff7ad9 35%%,#673ab7 68%%,#1b1028)}"
        "h1{color:#ff7ad9;font-size:20px;margin:0}.sub{color:#9b83b5;font-size:12px;margin:2px 0 0}"
        ".nav{display:flex;gap:8px;margin:-4px 0 14px}.nav a{flex:1;text-align:center;text-decoration:none;"
        "padding:9px 8px;border:1px solid #45345a;border-radius:9px;color:#e8b8ff;background:#130d1c}"
        ".nav a.on{background:#ff7ad9;color:#16081a;border-color:#ff7ad9;font-weight:700}"
        ".card{background:rgba(18,10,28,.88);border:1px solid #45345a;border-radius:14px;padding:16px;margin-bottom:14px}"
        ".t{color:#ff7ad9;font-size:11px;letter-spacing:1.5px;text-transform:uppercase;margin-bottom:10px;opacity:.9}"
        "label{display:block;margin:12px 0 4px;color:#e8b8ff;font-size:13px}"
        "select{width:100%%;box-sizing:border-box;padding:10px;border-radius:8px;border:1px solid #45345a;background:#130d1c;color:#fff;font-size:16px}"
        ".ck{width:auto;display:inline;margin-right:8px;vertical-align:middle}"
        ".note{color:#bca8cc;font-size:12px;line-height:1.35;margin:8px 0 0}"
        ".ft{color:#8b7898;font-size:12px;text-align:center;margin-top:6px}.ft code{color:#e8b8ff}"
        "</style></head><body>"
        "<div class=hd><div class=dot></div><div><h1>TamaPoke</h1><p class=sub>Pet app settings</p></div></div>"
        "<nav class=nav><a href=/>Capsule-Radar</a><a class=on href=/tamapoke>TamaPoke</a><a href=/update>Firmware</a></nav>"
        "<div class=card><div class=t>Display</div>"
        "<label>TamaPoke screen rotation</label><select onchange='tr(this.value)'>%s</select>"
        "<label><input type=checkbox class=ck %s onchange='du(this.checked)'>Dim while plugged in</label>"
        "<p class=note>When off, TamaPoke stays bright while USB power is connected. It still dims on battery to protect the display and save power.</p>"
        "<p class=note>Escape hatch: hold the top-center of the TamaPoke screen for about 2 seconds to return to Capsule Radar.</p>"
        "</div><p class=ft>Reach me at <code>capsuleradar.local</code> &middot; v" FW_VERSION "</p>"
        "<script>function tr(v){fetch('/tamapoke/rotate?v='+v+'&save=1')}function du(c){fetch('/tamapoke/dim-usb?v='+(c?1:0)+'&save=1')}</script></body></html>",
        ropts.c_str(), g_tamapokeDimOnUsb ? "checked" : "");
    g_web.send(200, "text/html", buf);
}

static void handleTamapokeRotate() {
    if (g_web.hasArg("v")) {
        g_tamapokeRotation = constrain((int)g_web.arg("v").toInt(), 0, 3);
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("cr_app", false);
            p.putInt("rot", g_tamapokeRotation);
            p.end();
        }
        if (g_appMode == 1) tamapoke_set_rotation((uint8_t)g_tamapokeRotation);
        Serial.printf("[tamapoke] saved rotation -> %d\n", g_tamapokeRotation);
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleTamapokeDimUsb() {
    if (g_web.hasArg("v")) {
        g_tamapokeDimOnUsb = g_web.arg("v").toInt() != 0;
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("cr_app", false);
            p.putBool("dimonusb", g_tamapokeDimOnUsb);
            p.end();
        }
        if (g_appMode == 1) tamapoke_set_dim_on_usb(g_tamapokeDimOnUsb);
        Serial.printf("[tamapoke] saved dim-on-usb -> %d\n", g_tamapokeDimOnUsb ? 1 : 0);
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleSpriteSerialLoader() {
    // TamaPoke has its own serial console when it is active. This lightweight
    // hook lets the first-time web installer copy sprites to SD while the
    // device is still sitting in Capsule Radar.
    if (g_appMode != 0 || !Serial.available()) return;

    String line = Serial.readStringUntil('\n');
    line.trim();
    if (!(line.startsWith("PUT ") || line == "LS")) return;

    if (!g_spriteLoaderSdStarted) {
        g_spriteLoaderSdStarted = sdBegin();
        Serial.printf("[sprite] SD loader %s\n", g_spriteLoaderSdStarted ? "ready" : "failed");
    }
    if (!g_spriteLoaderSdStarted) {
        Serial.println("ERR");
        return;
    }
    if (!sdSerialCommand(line)) Serial.println("ERR");
}

// ---- browser OTA: upload an app .bin over WiFi and self-flash ----
static String jsonEscape(const String &s) {
    String out;
    out.reserve(s.length() + 8);
    for (uint16_t i = 0; i < s.length(); ++i) {
        const char c = s[i];
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else out += c;
    }
    return out;
}

static String jsonValueForKey(const String &json, const String &key) {
    const String pattern = "\"" + key + "\"";
    int keyIndex = json.indexOf(pattern);
    if (keyIndex < 0) return "";
    int colonIndex = json.indexOf(':', keyIndex + pattern.length());
    if (colonIndex < 0) return "";
    int valueStart = colonIndex + 1;
    while (valueStart < (int)json.length() && isspace((unsigned char)json[valueStart])) valueStart++;
    if (valueStart >= (int)json.length()) return "";
    if (json[valueStart] == '"') {
        valueStart++;
        String value;
        bool escaped = false;
        for (int i = valueStart; i < (int)json.length(); ++i) {
            const char c = json[i];
            if (escaped) { value += c; escaped = false; }
            else if (c == '\\') escaped = true;
            else if (c == '"') return value;
            else value += c;
        }
        return "";
    }
    int valueEnd = valueStart;
    while (valueEnd < (int)json.length() && json[valueEnd] != ',' && json[valueEnd] != '}') valueEnd++;
    String value = json.substring(valueStart, valueEnd);
    value.trim();
    return value;
}

static bool fetchRemoteUpdateManifest(String &manifest, String &error) {
    if (WiFi.status() != WL_CONNECTED) {
        error = "WiFi is not connected.";
        return false;
    }
    g_firmwareUpdateInProgress = true;   // pause ADS-B/route/photo TLS work while GitHub TLS connects
    delay(250);
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setReuse(false);
    http.useHTTP10(true);
    http.setTimeout(15000);
    if (!http.begin(client, RELEASE_MANIFEST_URL)) {
        error = "Could not open release manifest URL.";
        g_firmwareUpdateInProgress = false;
        return false;
    }
    http.addHeader("User-Agent", "CapsuleRadar/" FW_VERSION " ESP32-S3 OTA");
    const int code = http.GET();
    if (code != HTTP_CODE_OK) {
        error = "Manifest request failed with HTTP " + String(code) + " (" + http.errorToString(code) +
                "), heap=" + String((unsigned)ESP.getFreeHeap()) +
                " largest=" + String((unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)) + ".";
        http.end();
        g_firmwareUpdateInProgress = false;
        return false;
    }
    manifest = http.getString();
    http.end();
    if (manifest.length() == 0) {
        error = "Manifest was empty.";
        g_firmwareUpdateInProgress = false;
        return false;
    }
    g_firmwareUpdateInProgress = false;
    return true;
}

static bool parseRemoteUpdateManifest(const String &manifest, String &version, String &firmwareUrl,
                                      String &sha256, uint32_t &size, String &error) {
    version = jsonValueForKey(manifest, "version");
    firmwareUrl = jsonValueForKey(manifest, "firmware");
    sha256 = jsonValueForKey(manifest, "sha256");
    size = (uint32_t)jsonValueForKey(manifest, "size").toInt();
    sha256.toLowerCase();
    if (version.length() == 0 || firmwareUrl.length() == 0 || sha256.length() != 64 || size < MIN_FIRMWARE_SIZE_BYTES) {
        error = "Manifest is missing version, firmware URL, SHA-256, or a valid size.";
        return false;
    }
    if (!firmwareUrl.startsWith("https://")) {
        error = "Firmware URL must use HTTPS.";
        return false;
    }
    return true;
}

static uint16_t versionPart(const String &version, uint8_t partIndex) {
    uint8_t currentPart = 0;
    uint16_t value = 0;
    bool collecting = false;
    for (uint16_t i = 0; i <= version.length(); ++i) {
        const char c = i < version.length() ? version[i] : '.';
        if (isdigit((unsigned char)c)) {
            if (currentPart == partIndex) {
                value = (value * 10) + (c - '0');
                collecting = true;
            }
        } else if (c == '.') {
            if (currentPart == partIndex) return collecting ? value : 0;
            currentPart++;
            value = 0;
            collecting = false;
        }
    }
    return value;
}

static bool isRemoteVersionNewer(const String &remoteVersion) {
    String current = FW_VERSION;
    for (uint8_t part = 0; part < 3; ++part) {
        const uint16_t remotePart = versionPart(remoteVersion, part);
        const uint16_t currentPart = versionPart(current, part);
        if (remotePart > currentPart) return true;
        if (remotePart < currentPart) return false;
    }
    return false;
}

static bool firmwareUpdateModeSaved() {
    Preferences p;
    if (!p.begin("cr_app", true)) return false;
    const bool enabled = p.getBool("updmode", false);
    p.end();
    return enabled;
}

static void saveFirmwareUpdateMode(bool enabled) {
    Preferences p;
    if (!p.begin("cr_app", false)) return;
    p.putBool("updmode", enabled);
    p.end();
}

static void handleEnterUpdateMode() {
    saveFirmwareUpdateMode(true);
    g_web.send(200, "text/html",
        "<!DOCTYPE html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<body style='background:#06100a;color:#1dff86;font-family:system-ui,sans-serif;padding:24px'>"
        "<h1>Entering updater mode</h1><p>The device is restarting into a lightweight firmware updater. Reload "
        "<code>http://capsuleradar.local/update</code> in about 10 seconds.</p></body></html>");
    delay(700);
    ESP.restart();
}

static void handleExitUpdateMode() {
    saveFirmwareUpdateMode(false);
    g_web.send(200, "text/html",
        "<!DOCTYPE html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<body style='background:#06100a;color:#1dff86;font-family:system-ui,sans-serif;padding:24px'>"
        "<h1>Leaving updater mode</h1><p>Restarting back into Capsule Radar...</p></body></html>");
    delay(700);
    ESP.restart();
}

static bool installRemoteFirmware(const String &firmwareUrl, const String &expectedSha256,
                                  uint32_t expectedSize, String &error) {
    if (WiFi.status() != WL_CONNECTED) {
        error = "WiFi is not connected.";
        return false;
    }
    g_firmwareUpdateInProgress = true;

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setReuse(false);
    http.useHTTP10(true);
    http.setTimeout(20000);
    if (!http.begin(client, firmwareUrl)) {
        error = "Could not open firmware URL.";
        g_firmwareUpdateInProgress = false;
        return false;
    }
    http.addHeader("User-Agent", "CapsuleRadar/" FW_VERSION " ESP32-S3 OTA");
    const int code = http.GET();
    if (code != HTTP_CODE_OK) {
        error = "Firmware download failed with HTTP " + String(code) + " (" + http.errorToString(code) + ").";
        http.end();
        g_firmwareUpdateInProgress = false;
        return false;
    }
    if (!Update.begin(expectedSize)) {
        error = "Could not start firmware update.";
        Update.printError(Serial);
        http.end();
        g_firmwareUpdateInProgress = false;
        return false;
    }

    mbedtls_sha256_context shaContext;
    mbedtls_sha256_init(&shaContext);
    mbedtls_sha256_starts(&shaContext, 0);

    WiFiClient *stream = http.getStreamPtr();
    stream->setTimeout(10000);
    uint8_t buffer[1024];
    uint32_t written = 0;
    unsigned long lastReadAt = millis();
    while (http.connected() && written < expectedSize) {
        size_t available = stream->available();
        if (available == 0) {
            if (millis() - lastReadAt > 20000UL) {
                error = "Firmware download timed out.";
                Update.abort();
                mbedtls_sha256_free(&shaContext);
                http.end();
                g_firmwareUpdateInProgress = false;
                return false;
            }
            delay(1);
            yield();
            continue;
        }
        size_t toRead = available < sizeof(buffer) ? available : sizeof(buffer);
        const size_t remainingBytes = expectedSize - written;
        toRead = toRead < remainingBytes ? toRead : remainingBytes;
        const int bytesRead = stream->readBytes(buffer, toRead);
        if (bytesRead <= 0) continue;
        lastReadAt = millis();
        mbedtls_sha256_update(&shaContext, buffer, bytesRead);
        if (Update.write(buffer, bytesRead) != (size_t)bytesRead) {
            error = "Firmware flash write failed.";
            Update.abort();
            mbedtls_sha256_free(&shaContext);
            http.end();
            g_firmwareUpdateInProgress = false;
            return false;
        }
        written += bytesRead;
    }
    http.end();
    if (written != expectedSize) {
        error = "Firmware download size mismatch.";
        Update.abort();
        mbedtls_sha256_free(&shaContext);
        g_firmwareUpdateInProgress = false;
        return false;
    }

    uint8_t digest[32];
    mbedtls_sha256_finish(&shaContext, digest);
    mbedtls_sha256_free(&shaContext);
    char actualSha256[65];
    for (uint8_t i = 0; i < 32; ++i) snprintf(actualSha256 + (i * 2), 3, "%02x", digest[i]);
    actualSha256[64] = '\0';
    if (expectedSha256 != String(actualSha256)) {
        error = "Firmware SHA-256 verification failed.";
        Update.abort();
        g_firmwareUpdateInProgress = false;
        return false;
    }
    if (!Update.end(true)) {
        error = "ESP32 firmware validation failed.";
        Update.printError(Serial);
        g_firmwareUpdateInProgress = false;
        return false;
    }
    Serial.printf("[update] remote done: %u bytes sha256=%s\n", (unsigned)written, actualSha256);
    return true;
}

static void handleRemoteUpdateCheck() {
    String manifest, error, version, firmwareUrl, sha256;
    uint32_t size = 0;
    if (!fetchRemoteUpdateManifest(manifest, error) ||
        !parseRemoteUpdateManifest(manifest, version, firmwareUrl, sha256, size, error)) {
        g_web.send(500, "application/json", "{\"error\":\"" + jsonEscape(error) + "\"}");
        return;
    }
    const bool newer = isRemoteVersionNewer(version);
    String msg = newer ? "Version " + version + " is available (" + String(size) + " bytes)."
                       : "Current firmware " + String(FW_VERSION) + " is up to date.";
    String out = "{\"currentVersion\":\"" + jsonEscape(FW_VERSION) + "\",\"remoteVersion\":\"" +
                 jsonEscape(version) + "\",\"updateAvailable\":" + (newer ? "true" : "false") +
                 ",\"size\":" + String(size) + ",\"message\":\"" + jsonEscape(msg) + "\"}";
    g_web.send(200, "application/json", out);
}

static void handleRemoteUpdateInstall() {
    String manifest, error, version, firmwareUrl, sha256;
    uint32_t size = 0;
    if (!fetchRemoteUpdateManifest(manifest, error) ||
        !parseRemoteUpdateManifest(manifest, version, firmwareUrl, sha256, size, error)) {
        g_firmwareUpdateInProgress = false;
        g_web.send(500, "text/plain", error);
        return;
    }
    if (!isRemoteVersionNewer(version)) {
        g_web.send(409, "text/plain", "No newer firmware is available.");
        return;
    }
    Serial.printf("[update] installing remote firmware %s (%u bytes)\n", version.c_str(), (unsigned)size);
    if (!installRemoteFirmware(firmwareUrl, sha256, size, error)) {
        g_web.send(500, "text/plain", error);
        return;
    }
    g_web.sendHeader("Connection", "close");
    g_web.sendHeader("Refresh", "8; url=/");
    g_web.send(200, "text/html",
        "<!DOCTYPE html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<body style='background:#06100a;color:#1dff86;font-family:system-ui,sans-serif;padding:24px'>"
        "<h1>Firmware updated</h1><p>Downloaded and verified version " + version +
        ". Restarting now...</p><script>setTimeout(function(){location.href='/'},8000)</script></body></html>");
    delay(900);
    saveFirmwareUpdateMode(false);
    ESP.restart();
}

static void handleUpdatePage() {
    String page = F(
        "<!DOCTYPE html><html><head><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>Capsule Radar - Update</title><style>"
        "body{background:radial-gradient(circle at 50% -10%,#0a1f15,#04100a 70%);color:#cdd6d1;"
        "font-family:system-ui,sans-serif;margin:0 auto;padding:20px;max-width:480px;min-height:100vh}"
        "h1{color:#1dff86;font-size:20px}.card{background:rgba(10,20,14,.85);border:1px solid #1f3a2b;border-radius:14px;padding:16px;margin-bottom:14px}"
        ".nav{display:flex;gap:8px;margin:0 0 14px}.nav a{flex:1;text-align:center;text-decoration:none;"
        "padding:9px 8px;border:1px solid #2a4a39;border-radius:9px;color:#9affc8;background:#0c1a12}"
        ".nav a.on{background:#1dff86;color:#04140b;border-color:#1dff86;font-weight:700}"
        "input,button{width:100%;box-sizing:border-box;padding:11px;border-radius:8px;margin-top:8px;font-size:16px}"
        "input{background:#0c1a12;color:#eafff3;border:1px solid #2a4a39}"
        "button{border:0;background:#1dff86;color:#04140b;font-weight:700}button:disabled{opacity:.45}.sec{background:#0c1a12;color:#1dff86;border:1px solid #2a4a39}"
        "#bar{height:12px;background:#0c1a12;border-radius:6px;overflow:hidden;margin-top:14px;display:none}"
        "#fill,#rfill{height:100%;width:0;background:#1dff86;transition:width .2s}#msg,#rmsg{margin-top:10px;color:#9affc8;font-size:13px}"
        "#rbar{height:12px;background:#0c1a12;border-radius:6px;overflow:hidden;margin-top:14px;display:none}"
        ".t{color:#1dff86;font-size:11px;letter-spacing:1.5px;text-transform:uppercase;margin-bottom:10px;opacity:.85}"
        "a{color:#1dff86}p{color:#9affc8;font-size:13px}"
        "</style></head><body><h1>Firmware update (OTA)</h1>"
        "<nav class=nav><a href=/>Capsule-Radar</a><a href=/tamapoke>TamaPoke</a><a class=on href=/update>Firmware</a></nav>"
        "<div class=card><div class=t>GitHub release</div>"
        "<p>Current firmware: <b>v" FW_VERSION "</b></p>"
        "<p>Check the latest GitHub Release and install the verified <code>CapsuleRadar-ota.bin</code> directly from the device.</p>"
        "<button class=sec onclick=c()>Check GitHub</button><button id=ri disabled onclick=ri()>Install latest</button>"
        "<button class=sec onclick=um()>Restart in lightweight updater mode</button><button class=sec onclick=xum()>Exit updater mode</button>"
        "<div id=rbar><div id=rfill></div></div><div id=rmsg>Waiting.</div></div>"
        "<div class=card><div class=t>Manual upload</div>"
        "<p>Upload the <b>app firmware</b> <code>CapsuleRadar-ota.bin</code> from the GitHub release. "
        "Do NOT use the merged flash image here.</p>"
        "<input type=file id=f accept='.bin'>"
        "<button onclick=u()>Update over WiFi</button>"
        "<div id=bar><div id=fill></div></div><div id=msg></div></div>"
        "<script>var ready=0,MODE=@,pt=0,pp=0;"
        "function setm(s){document.getElementById('rmsg').innerText=s}function ib(){document.getElementById('ri').disabled=!ready}"
        "function prog(p){document.getElementById('rbar').style.display='block';document.getElementById('rfill').style.width=p+'%'}"
        "function pstart(max,step,ms){clearInterval(pt);pp=8;prog(pp);pt=setInterval(()=>{pp=Math.min(max,pp+step);prog(pp)},ms)}"
        "function pstop(p){clearInterval(pt);prog(p)}"
        "function checkNow(){var b=document.getElementById('ri');b.disabled=true;setm('Checking GitHub...');pstart(85,6,650);"
        "return fetch('/remote-update-check').then(r=>r.json().then(j=>({ok:r.ok,j:j}))).then(o=>{if(!o.ok)throw Error(o.j.error||'Check failed');"
        "pstop(100);ready=o.j.updateAvailable?1:0;ib();setm(o.j.message);localStorage.removeItem('crAutoCheck');return o.j;}).catch(e=>{pstop(0);ready=0;ib();setm('Check failed: '+e.message);localStorage.removeItem('crAutoCheck');});}"
        "function pollCheck(n){setm('Restarting into lightweight updater and checking GitHub...');pstart(70,3,700);let tries=0,t=setInterval(()=>{tries++;"
        "fetch('/remote-update-check').then(r=>r.json().then(j=>({ok:r.ok,j:j}))).then(o=>{if(!o.ok)throw Error(o.j.error||'Check failed');clearInterval(t);"
        "pstop(100);ready=o.j.updateAvailable?1:0;ib();setm(o.j.message);localStorage.removeItem('crAutoCheck');}).catch(e=>{if(tries>n){clearInterval(t);pstop(0);ready=0;ib();setm('Check failed after updater restart: '+e.message);localStorage.removeItem('crAutoCheck');}});},2500)}"
        "function c(){if(MODE)return checkNow();localStorage.setItem('crAutoCheck','1');ready=0;ib();setm('Restarting into lightweight updater and checking GitHub...');"
        "fetch('/enter-update-mode',{method:'POST'}).catch(()=>{});pollCheck(24);}"
        "function ri(){if(!ready)return;var m=document.getElementById('rmsg'),b=document.getElementById('ri');b.disabled=true;m.innerText='Downloading and installing. Do not power off...';pstart(92,2,900);"
        "fetch('/remote-update-install',{method:'POST'}).then(r=>r.text().then(t=>({ok:r.ok,t:t}))).then(o=>{if(!o.ok)throw Error(o.t||'Install failed');pstop(100);document.open();document.write(o.t);document.close();}).catch(e=>{pstop(0);b.disabled=false;m.innerText='Install failed: '+e.message;});}"
        "function um(){document.getElementById('rmsg').innerText='Restarting into lightweight updater mode...';fetch('/enter-update-mode',{method:'POST'}).then(r=>r.text()).then(t=>{document.open();document.write(t);document.close();});}"
        "function xum(){document.getElementById('rmsg').innerText='Restarting back to normal mode...';fetch('/exit-update-mode',{method:'POST'}).then(r=>r.text()).then(t=>{document.open();document.write(t);document.close();});}"
        "if(localStorage.getItem('crAutoCheck')==='1'){if(MODE)setTimeout(checkNow,800);else pollCheck(24);}"
        "function u(){var f=document.getElementById('f').files[0];if(!f){return}"
        "var x=new XMLHttpRequest(),fd=new FormData();fd.append('f',f);"
        "document.getElementById('bar').style.display='block';"
        "x.upload.onprogress=function(e){if(e.lengthComputable)document.getElementById('fill').style.width=(e.loaded/e.total*100)+'%'};"
        "x.onload=function(){document.getElementById('msg').innerText=x.responseText+' - rebooting...'};"
        "x.onerror=function(){document.getElementById('msg').innerText='Upload failed'};"
        "x.open('POST','/update');x.send(fd);}</script></body></html>");
    page.replace("MODE=@", g_firmwareUpdateMode ? "MODE=1" : "MODE=0");
    g_web.send(200, "text/html", page);
}

static void handleUpdateUpload() {
    HTTPUpload &up = g_web.upload();
    if (up.status == UPLOAD_FILE_START) {
        g_firmwareUpdateInProgress = true;
        Serial.printf("[update] start: %s\n", up.filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (Update.write(up.buf, up.currentSize) != up.currentSize) Update.printError(Serial);
    } else if (up.status == UPLOAD_FILE_END) {
        if (Update.end(true)) Serial.printf("[update] done: %u bytes\n", (unsigned)up.totalSize);
        else Update.printError(Serial);
    } else if (up.status == UPLOAD_FILE_ABORTED) {
        Update.abort();
        g_firmwareUpdateInProgress = false;
    }
}

static void setupFirmwareUpdateMode() {
    g_firmwareUpdateMode = true;
    g_firmwareUpdateInProgress = false;
    Serial.println("[update] lightweight updater mode");

    WiFi.mode(WIFI_STA);
    WiFi.begin();
    const uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000UL) {
        delay(100);
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[update] WiFi up, IP %s\n", WiFi.localIP().toString().c_str());
        MDNS.begin("capsuleradar");
    } else {
        Serial.println("[update] WiFi not connected; updater page will still start if network recovers");
    }

    g_web.on("/", HTTP_GET, handleUpdatePage);
    g_web.on("/update", HTTP_GET, handleUpdatePage);
    g_web.on("/enter-update-mode", HTTP_POST, handleEnterUpdateMode);
    g_web.on("/exit-update-mode", HTTP_POST, handleExitUpdateMode);
    g_web.on("/remote-update-check", HTTP_GET, handleRemoteUpdateCheck);
    g_web.on("/remote-update-install", HTTP_POST, handleRemoteUpdateInstall);
    g_web.on("/update", HTTP_POST,
        []() {
            const bool ok = !Update.hasError();
            g_web.send(200, "text/plain", ok ? "OK" : "FAIL");
            delay(800);
            if (ok) {
                saveFirmwareUpdateMode(false);
                ESP.restart();
            }
            g_firmwareUpdateInProgress = false;
        },
        handleUpdateUpload);
    g_web.begin();
    Serial.printf("[update] page: http://%s/update\n",
                  WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "capsuleradar.local");
}

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\nCapsule Radar boot");
    if (firmwareUpdateModeSaved()) {
        setupFirmwareUpdateMode();
        return;
    }

    if (PIN_LCD_SCLK < 0 || PIN_I2C_SDA < 0) {
        Serial.println("[!] Pins in config.h are still -1. Copy them from the Waveshare demo.");
    }
    Serial.printf("PSRAM: %u bytes free\n", (unsigned)ESP.getFreePsram());

    loadSettings();
    route_cache_begin();   // clear stale route cache if the label format changed

    // --- Display + LVGL (M0) ----------------------------------------------
    // CO5300 AMOLED over QSPI + LVGL draw buffers in PSRAM, then a hello screen.
    // The panel is powered from the always-on DC1 rail, so it lights without the
    // PMIC. Touch (CST9217 indev) + AXP2101 come in later milestones.
    if (!display::begin()) {
        Serial.println("[!] display::begin() failed — check QSPI pins / power.");
    }

    // restore the saved theme, then persist any future change
    {
        Preferences p;
        p.begin("capsuleradar", true);
        const int t = p.getInt("theme", THEME_PHOSPHOR);
        g_showSweep = p.getBool("sweep", true);
        g_showAirports = p.getBool("airports", true);
        g_rotation = p.getInt("rot", 0);
        g_rotationOffset = constrain(p.getInt("rotoff", 0), -45, 45);
        p.end();
        radar::setTheme(t);
        radar::setSweepEnabled(g_showSweep);
        radar::setAirportsEnabled(g_showAirports);
        radar::setTrailLength(g_trailLen);
        radar::setTrackingFontSize(g_trackingFontSize);
        display::setRotation((uint8_t)g_rotation);
        display::setRotationOffset((int8_t)g_rotationOffset);
    }
    radar::setThemeChangedCb(saveTheme);
    ui_set_range_cb(onRangeChange);              // on-screen zoom button
    ui_set_app_switch_cb(requestTamapokeFromRadar); // top-center hold switches to TamaPoke
    ui_set_units(g_units);                       // apply saved unit preset
    ui_set_range_km(g_settings.rangeKm);         // show the loaded range

    imu_begin();       // face-down sleep (no-op if the IMU isn't detected)
    battery_begin();   // AXP2101 (no-op if not detected / no battery)
    gps_begin();       // LC76G GNSS (no-op if not the -G variant)
    battery_enable_codec_rail();   // power the ES8311 analog rail before audio init

    setenv("TZ", TZ_STR, 1); tzset();   // local time for display even before NTP
    rtc_begin();
    rtc_seed_clock();                   // offline clock/date from the PCF85063
    if (audio_begin()) {                // ES8311 alert pings (no-op if codec absent)
        audio_set_volume(g_volume);
        audio_set_muted(g_muted);
    }

    // --- Radar UI ----------------------------------------------------------
    // radar::init() runs inside display::begin() (LVGL must be up first).

    // --- WiFi (captive portal, non-blocking) ------------------------------
    // First boot opens the "CapsuleRadar-Setup" AP to enter WiFi creds. Non-blocking
    // so the radar keeps animating while you configure WiFi from your phone.
    g_wm.setConfigPortalBlocking(false);
    g_wm.setTitle("Capsule Radar");
    // light phosphor-green theme for the captive portal (small CSS, injected into <head>)
    g_wm.setCustomHeadElement(
        "<style>"
        "body{background:#06100a;color:#cdd6d1;font-family:system-ui,sans-serif}"
        "h1,h2,h3{color:#1dff86}"
        "button,input[type=submit],.btn{background:#1dff86!important;color:#04140b!important;"
        "border:0!important;border-radius:8px!important;font-weight:700}"
        "input,select{background:#0c1a12!important;color:#eafff3!important;"
        "border:1px solid #2a4a39!important;border-radius:8px!important}"
        "a{color:#1dff86}.q{filter:hue-rotate(90deg)}"
        "</style>");
    // After the portal saves new credentials, reboot for a clean start: WiFiManager's
    // own port-80 server (and mDNS) don't cleanly hand over to our web server / STA
    // interface in non-blocking mode, so the config page is flaky until a fresh boot.
    g_wm.setSaveConfigCallback([]() {
        Serial.println("[wifi] new credentials saved -> rebooting for a clean web/mDNS start");
        g_rebootAtMs = millis() + 2500;   // let the portal deliver its 'saved' page first
    });
    if (g_wm.autoConnect("CapsuleRadar-Setup"))
        Serial.println("[wifi] connected");
    else
        Serial.println("[wifi] config portal open - join 'CapsuleRadar-Setup' to set WiFi; UI stays live");

    // --- OTA ---------------------------------------------------------------
    // ArduinoOTA is started from loop() once WiFi connects (see otaUp there).

    // --- ADS-B client + task ----------------------------------------------
    float queryKm = g_settings.rangeKm * 1.6f;          // query wider than the display range
    if (queryKm < 50.0f)  queryKm = 50.0f;
    if (queryKm > 200.0f) queryKm = 200.0f;
    g_adsb.begin(g_settings.homeLat, g_settings.homeLon, queryKm);
    g_ac_mutex = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(adsb_task, "adsb", 16384, nullptr, 1, nullptr, 0);  // TLS needs a big stack

    // configuration web page (http://capsuleradar.local/)
    g_web.on("/", handleRoot);
    g_web.on("/save", HTTP_POST, handleSave);
    g_web.on("/wifi", HTTP_POST, handleWifi);
    g_web.on("/bright", handleBright);
    g_web.on("/vol", handleVol);
    g_web.on("/alerts", handleAlerts);
    g_web.on("/idle", handleIdle);
    g_web.on("/sweep", handleSweep);
    g_web.on("/prefetch", handlePrefetch);
    g_web.on("/generic-photos", handleGenericPhotos);
    g_web.on("/airports", handleAirports);
    g_web.on("/app", handleApp);
    g_web.on("/trail", handleTrail);
    g_web.on("/tracking-font", handleTrackingFont);
    g_web.on("/rotate", handleRotate);
    g_web.on("/rotate-offset", handleRotateOffset);
    g_web.on("/gps", handleGps);
    g_web.on("/units", handleUnits);
    g_web.on("/tamapoke", HTTP_GET, handleTamapokePage);
    g_web.on("/tamapoke/rotate", handleTamapokeRotate);
    g_web.on("/tamapoke/dim-usb", handleTamapokeDimUsb);
    g_web.on("/update", HTTP_GET, handleUpdatePage);
    g_web.on("/enter-update-mode", HTTP_POST, handleEnterUpdateMode);
    g_web.on("/exit-update-mode", HTTP_POST, handleExitUpdateMode);
    g_web.on("/remote-update-check", HTTP_GET, handleRemoteUpdateCheck);
    g_web.on("/remote-update-install", HTTP_POST, handleRemoteUpdateInstall);
    g_web.on("/update", HTTP_POST,
        []() {
            const bool ok = !Update.hasError();
            g_web.send(200, "text/plain", ok ? "OK" : "FAIL");
            delay(800);
            if (ok) ESP.restart();
            g_firmwareUpdateInProgress = false;
        },
        handleUpdateUpload);
    g_web.begin();

    if (g_appMode != 0) {
        if (appBootGuardWasSet()) {
            Serial.println("[app] previous TamaPoke boot did not confirm; falling back to radar");
            persistAppMode(0);
            g_appMode = 0;
        } else {
            Serial.println("[app] saved TamaPoke mode found; starting with boot guard");
            setAppBootGuard(true);
            if (activateApp(g_appMode)) {
                g_tamapokeBootGuard = true;
                g_tamapokeBootGuardUntilMs = millis() + 15000;
            } else {
                Serial.println("[app] saved TamaPoke mode failed; staying in radar");
                persistAppMode(0);
                g_appMode = 0;
                display::invalidate();
            }
        }
    }

    Serial.println("setup done");
}

void loop() {
    if (g_firmwareUpdateMode) {
        g_web.handleClient();
        if (g_rebootAtMs && (int32_t)(millis() - g_rebootAtMs) >= 0) { delay(50); ESP.restart(); }
        delay(2);
        return;
    }

    if (g_appMode == 0) display::loop();  // drive LVGL (render dirty areas + run timers)
    else                tamapoke_loop();  // guest app owns direct full-screen drawing
    g_wm.process();                 // service the WiFi config portal (non-blocking)
    g_web.handleClient();           // serve the configuration web page
    handleSpriteSerialLoader();     // first-time web installer can copy TamaPoke sprites

    if (g_appMode == 1 && tamapoke_consume_radar_request()) {
        persistAppMode(0);
        activateApp(0);
    }

    if (g_pendingAppMode >= 0) {
        const int mode = constrain((int)g_pendingAppMode, 0, 1);
        const bool save = g_pendingAppSave;
        g_pendingAppMode = -1;
        g_pendingAppSave = false;

        const bool ok = activateApp(mode);
        if (!ok && save) {
            persistAppMode(0);
        } else if (ok && save) {
            persistAppMode(mode);
            Serial.printf("[app] persisted -> %s\n", mode == 1 ? "TamaPoke" : "Capsule Radar");
        }
    }

    if (g_tamapokeBootGuard && (int32_t)(millis() - g_tamapokeBootGuardUntilMs) >= 0) {
        g_tamapokeBootGuard = false;
        setAppBootGuard(false);
        Serial.println("[app] TamaPoke boot confirmed");
    }

    if (g_useGps) gps_poll();       // pull NMEA from the LC76G (only when GPS auto-location is on)

    // scheduled reboot after a fresh WiFi config (see setSaveConfigCallback)
    if (g_rebootAtMs && (int32_t)(millis() - g_rebootAtMs) >= 0) { delay(50); ESP.restart(); }

    // OTA: set up once WiFi is up, then service it every loop (flash over the air)
    static bool otaUp = false;
    if (!otaUp && WiFi.status() == WL_CONNECTED) {
        ArduinoOTA.setHostname("capsuleradar");        // -> capsuleradar.local (registers mDNS)
        ArduinoOTA.begin();
        MDNS.addService("http", "tcp", 80);            // advertise the config web page
        otaUp = true;
        Serial.println("[ota] ready: pio run -e esp32-s3-amoled-175-ota -t upload");
    }
    if (otaUp) ArduinoOTA.handle();

    // Push a fresh ADS-B snapshot to the radar (copy under the mutex, render outside).
    if (g_appMode == 0 && g_acDirty) {
        if (xSemaphoreTake(g_ac_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            g_snap.swap(g_aircraft);   // O(1) handoff under the lock; render on g_snap outside it.
            g_acDirty = false;         // g_aircraft now holds the previous snapshot (overwritten next poll)
            xSemaphoreGive(g_ac_mutex);
            radar::update(g_snap, g_settings); // rebuild the glyph/trail layer
            ui_on_data_updated();              // refresh card/list/stats
            checkAudioEvents();                // ping new-in-range / emergency / military
        }
    }
    if (g_appMode == 0) updatePrefetchIndicators();

    // periodic: HUD clock + wifi/battery indicators
    static uint32_t lastStatus = 0;
    if (millis() - lastStatus > 5000) {
        lastStatus = millis();
#if DEBUG_MEM
        static uint32_t lastFrames = 0;
        const uint32_t fr = display_frames();
        const unsigned fps = (fr - lastFrames) / 5;
        lastFrames = fr;
        Serial.printf("[mem] heap %u (min %u, biggest %u) | psram %u free | up %lus | aircraft %d | fps %u\n",
                      (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                      (unsigned)ESP.getFreePsram(), (unsigned long)(millis() / 1000),
                      (int)g_snap.size(), fps);
#endif
        char clk[8] = "--:--";
        struct tm ti;
        const bool haveTime = getLocalTime(&ti, 0);
        if (haveTime) {
            snprintf(clk, sizeof(clk), "%02d:%02d", ti.tm_hour, ti.tm_min);
            char date[20];
            strftime(date, sizeof(date), "%d %b %Y", &ti);   // e.g. "08 Jun 2026"
            ui_set_date(date);
        }
        const bool wifiUp = (WiFi.status() == WL_CONNECTED);
        const int  rssi   = wifiUp ? (int)WiFi.RSSI() : -127;
        // "fresh" = we got aircraft data recently. Catches a stalled feed (weak WiFi dropping
        // polls intermittently) that never trips the consecutive-fail counter -> aircraft
        // freeze but the icon would otherwise stay white.
        const bool feedFresh = wifiUp && (millis() - g_lastFeedOkMs < 18000UL);
        if (g_appMode == 0) ui_set_status(wifiUp, feedFresh, rssi, clk);
        char net[80];
        if (WiFi.status() == WL_CONNECTED)
            snprintf(net, sizeof(net), "Configure at\ncapsuleradar.local\n%s", WiFi.localIP().toString().c_str());
        else
            snprintf(net, sizeof(net), "WiFi setup:\njoin CapsuleRadar-Setup");
        if (g_appMode == 0) ui_set_netinfo(net);
        const bool bpresent = battery_present();
        const bool externalPower = battery_external_power();
        if (g_appMode == 0) ui_set_battery(battery_percent(), battery_charging(), bpresent);
        g_onBattery = bpresent && !externalPower;
        // GPS HUD/Stats: 0 = off/no module (hidden), 1 = acquiring, 2 = fix
        const int gpsState = (!g_useGps || !gps_present()) ? 0 : (gps_has_fix() ? 2 : 1);
        if (g_appMode == 0) ui_set_gps(gpsState, gps_satellites());
        // once NTP has a real fix, persist it to the RTC (core 1 only)
        if (!g_rtcSynced && time(nullptr) > 1700000000L) {
            time_t now = time(nullptr);
            struct tm utc;
            gmtime_r(&now, &utc);
            if (rtc_write(&utc)) { g_rtcSynced = true; Serial.println("[rtc] saved NTP time"); }
        }
        // GPS auto-location (-G variant): re-centre the radar when the fix moves enough.
        if (g_useGps) {
            double glat, glon;
            if (gps_location(&glat, &glon) &&
                geo::haversineKm(g_settings.homeLat, g_settings.homeLon, glat, glon) > 1.0) {
                g_settings.homeLat = glat; g_settings.homeLon = glon;   // radar/coastline recenter
                // re-query the new area — set the radius too (same formula as boot/zoom), or
                // adsb_task would re-begin with a stale/zero g_requeryKm and fetch 0 aircraft.
                g_requeryKm = constrain(g_settings.rangeKm * 1.6f, 50.0f, 200.0f);
                g_requery = true;                                       // adsb_task re-queries the new area
                Serial.printf("[gps] re-centred to %.4f, %.4f\n", glat, glon);
            }
        }
    }

    // face-down -> screen off (IMU); flip face-up to wake
    static uint32_t lastImu = 0;
    static int fdCount = 0;
    if (g_appMode == 0 && millis() - lastImu > 400) {
        lastImu = millis();
        const int fd = imu_facedown();              // 1 down, 0 not, -1 read error
        if (fd > 0)       { if (fdCount < 8) fdCount++; }
        else if (fd == 0) fdCount = 0;              // -1 (I2C hiccup): leave the counter as-is
        const bool sleep = (fdCount >= 4);   // ~1.6 s face-down
        const bool idle  = g_idleDimMs > 0 && display::inactiveMs() > g_idleDimMs;
        if (sleep != g_asleep || idle != g_idle) {
            g_asleep = sleep;
            g_idle = idle;
            applyBrightness();
        }
    }

    delay(5);
}
