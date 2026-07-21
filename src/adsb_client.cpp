// Fetch nearby aircraft from airplanes.live (fallback adsb.lol) and parse the
// readsb JSON into a vector<Aircraft>.
//
// Memory safety (important on the ESP32): we parse straight from the HTTP stream
// (no full-body String), use an ArduinoJson field filter so only the ~12 fields we
// need are kept, and hard-cap the number of aircraft (ADSB_MAX_AIRCRAFT). The radar
// then keeps only the nearest ~20 for display.
#include "adsb_client.h"
#include "config.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>   // v7
#include <esp_heap_caps.h>

static constexpr uint32_t ADSB_RATE_LIMIT_BACKOFF_MS = 60000UL;

// Parse the JSON in PSRAM, not internal RAM. Otherwise the per-poll JSON alloc/free
// churn fragments the internal heap and, after a while, mbedTLS can't find a large
// enough contiguous block for the TLS handshake (-32512), freezing the feed.
struct PsramJsonAllocator : ArduinoJson::Allocator {
    void* allocate(size_t n) override { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
    void  deallocate(void* p) override { heap_caps_free(p); }
    void* reallocate(void* p, size_t n) override { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM); }
};
static PsramJsonAllocator s_jsonPsram;

void AdsbClient::begin(double homeLat, double homeLon, float rangeKm) {
    _lat = homeLat; _lon = homeLon; _rangeKm = rangeKm;
}

bool AdsbClient::cooldownActive() const {
    return hostCooldownActive(_primaryCooldownUntilMs) || hostCooldownActive(_fallbackCooldownUntilMs);
}

uint32_t AdsbClient::cooldownRemainingMs() const {
    const uint32_t primary = hostCooldownRemainingMs(_primaryCooldownUntilMs);
    const uint32_t fallback = hostCooldownRemainingMs(_fallbackCooldownUntilMs);
    if (!primary) return fallback;
    if (!fallback) return primary;
    return primary < fallback ? primary : fallback;
}

bool AdsbClient::hostCooldownActive(uint32_t untilMs) const {
    return untilMs && (int32_t)(millis() - untilMs) < 0;
}

uint32_t AdsbClient::hostCooldownRemainingMs(uint32_t untilMs) const {
    if (!hostCooldownActive(untilMs)) return 0;
    return untilMs - millis();
}

bool AdsbClient::poll(std::vector<Aircraft>& out) {
    if (WiFi.status() != WL_CONNECTED) return false;
    const bool primaryCooling = hostCooldownActive(_primaryCooldownUntilMs);
    const bool fallbackCooling = hostCooldownActive(_fallbackCooldownUntilMs);

    // Prefer the primary host, and give it a quick second try before touching the fallback:
    // the primary is reliable in practice, while the fallback can be slow to time out from
    // some networks (turning one transient primary blip into a long no-data gap + amber HUD).
    if (!primaryCooling) {
        if (fetchFrom(ADSB_PRIMARY_HOST, out)) return true;
        if (fetchFrom(ADSB_PRIMARY_HOST, out)) return true;   // transient blip -> retry the healthy host
    }
    if (!fallbackCooling) return fetchFrom(ADSB_FALLBACK_HOST, out);   // last resort / cooldown escape hatch
    return false;
}

bool AdsbClient::fetchFrom(const char* host, std::vector<Aircraft>& out) {
    const double nm = _rangeKm * 0.539957;            // km -> nautical miles (API radius unit)
    char url[160];
    snprintf(url, sizeof(url), "https://%s/v2/point/%.4f/%.4f/%.0f", host, _lat, _lon, nm);

    WiFiClientSecure client;
#if ADSB_HTTPS_INSECURE
    client.setInsecure();                              // hobby: skip cert validation
#else
    // client.setCACert(ROOT_CA_PEM);                  // production: pin the root CA
#endif

    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(6000);    // fail reasonably fast: a slow host must not block the
    http.setTimeout(8000);           // task (and the user's route/photo lookups) for too long
    if (!http.begin(client, url)) { Serial.printf("[adsb] begin failed (%s)\n", host); return false; }
    http.addHeader("User-Agent", ADSB_USER_AGENT);
    http.addHeader("Accept", "application/json");

    const int code = http.GET();
    if (code != 200) {
        if (code == 429) {
            uint32_t &cooldownUntilMs = (strcmp(host, ADSB_PRIMARY_HOST) == 0)
                ? _primaryCooldownUntilMs : _fallbackCooldownUntilMs;
            cooldownUntilMs = millis() + ADSB_RATE_LIMIT_BACKOFF_MS;
            Serial.printf("[adsb] HTTP 429 (%s) -> backing off for %lus\n",
                          host, (unsigned long)(ADSB_RATE_LIMIT_BACKOFF_MS / 1000UL));
        } else {
            Serial.printf("[adsb] HTTP %d (%s)\n", code, host);
        }
        http.end();
        return false;
    }

    // Only keep the fields we use -> much smaller parsed document.
    JsonDocument filter(&s_jsonPsram);
    const char* keys[] = { "ac", "aircraft" };
    const char* flds[] = { "hex", "flight", "t", "lat", "lon", "alt_baro",
                           "track", "true_heading", "gs", "baro_rate",
                           "squawk", "seen_pos", "dbFlags" };
    for (const char* k : keys)
        for (const char* f : flds)
            filter[k][0][f] = true;

    JsonDocument doc(&s_jsonPsram);
    DeserializationError err = deserializeJson(doc, http.getStream(),
                                               DeserializationOption::Filter(filter));
    http.end();
    if (err) return false;

    JsonArrayConst arr = doc["ac"].as<JsonArrayConst>();
    if (arr.isNull()) arr = doc["aircraft"].as<JsonArrayConst>();
    if (arr.isNull()) return false;

    std::vector<Aircraft> tmp;
    const uint32_t now = millis();
    for (JsonObjectConst a : arr) {
        if ((int)tmp.size() >= ADSB_MAX_AIRCRAFT) break;   // hard cap: protect RAM

        if (a["lat"].isNull() || a["lon"].isNull()) continue;  // need a position

        Aircraft ac;
        ac.hex    = (const char*)(a["hex"] | "");
        if (ac.hex.length() == 0) continue;
        ac.flight = String((const char*)(a["flight"] | "")); ac.flight.trim();
        ac.type   = (const char*)(a["t"] | "");
        ac.lat    = a["lat"].as<double>();
        ac.lon    = a["lon"].as<double>();

        if (a["alt_baro"].is<const char*>()) { ac.onGround = true; ac.altBaro = 0; }  // "ground"
        else                                  ac.altBaro = a["alt_baro"] | 0.0f;

        ac.track    = a["track"].is<float>() ? a["track"].as<float>() : (a["true_heading"] | NAN);
        ac.gs       = a["gs"] | NAN;
        ac.baroRate = a["baro_rate"] | NAN;
        ac.squawk   = a["squawk"].is<const char*>() ? atoi(a["squawk"]) : (a["squawk"] | -1);
        ac.seenPos  = a["seen_pos"] | 0;
        ac.military = ((a["dbFlags"] | 0u) & 0x1) != 0;
        ac.lastUpdateMs = now;

        tmp.push_back(std::move(ac));
    }

    out.swap(tmp);
    _lastOkMs = now;
    return true;
}
