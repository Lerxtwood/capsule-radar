// Route lookup via adsbdb.com (free, no API key).
#include "route_client.h"
#include "config.h"
#include "geo.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <string.h>
#include <math.h>
#include <time.h>   // route-cache TTL

#define ROUTE_CACHE_MAX 40    // keep NVS roomy for durable settings/app selection
#define ROUTE_LEARN_MAX 24
static constexpr bool ROUTE_NVS_CACHE_READS_ENABLED = true;   // normal mode: reuse persisted route cache
static constexpr uint32_t ROUTE_CACHE_TTL_S = 900UL;          // 15 min: good enough for brief reuse, short enough for recycled callsigns
static constexpr uint32_t ROUTE_LEARN_TTL_S = 7200UL;         // 2h: remember likely same-day rotations, but forget stale legs quickly
static constexpr int ROUTE_SCORE_STRONG = 5;
static constexpr int ROUTE_SCORE_PERSIST = 4;
static constexpr uint32_t ROUTE_TRANSPORT_BACKOFF_MS = 15000UL;
static constexpr size_t ROUTE_MIN_TLS_BLOCK_BYTES = 28000;
static constexpr size_t ROUTE_MIN_FLIGHTAWARE_BLOCK_BYTES = 30000;

static char s_lastFetchMode[16] = "none";
static char s_lastFetchUrl[128] = "";
static int  s_lastFetchStatus = 0;
static constexpr size_t FLIGHTAWARE_SCRAPE_LIMIT = 25600;
static const char *FLIGHTAWARE_USER_AGENT =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
    "AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/126.0.0.0 Safari/537.36";
static uint32_t s_routeTransportCooldownUntilMs = 0;

struct PsramAlloc : ArduinoJson::Allocator {
    void *allocate(size_t n) override { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
    void  deallocate(void *p) override { heap_caps_free(p); }
    void *reallocate(void *p, size_t n) override { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM); }
};
static PsramAlloc s_jsonPsram;

struct RouteEndpoint {
    char label[40];
    char code[8];
    float lat, lon;
    bool hasCoord;
};

struct LearnedRoute {
    RouteEndpoint from, to;
    uint32_t stamp;
    int score;
    bool valid;
};

static void normalize_token(const char *in, char *out, size_t on) {
    size_t j = 0;
    for (const char *p = in; p && *p && j + 1 < on; ++p) {
        if (*p == ' ') continue;
        char c = *p;
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        out[j++] = c;
    }
    out[j] = 0;
}

static uint32_t route_identity_hash(const char *hex, const char *callsign) {
    char normHex[12] = "", normCall[12] = "";
    normalize_token(hex, normHex, sizeof(normHex));
    normalize_token(callsign, normCall, sizeof(normCall));
    const char *basis = normHex[0] ? normHex : normCall;
    const uint8_t *p = reinterpret_cast<const uint8_t *>(basis);
    uint32_t hash = 2166136261UL;
    while (*p) {
        hash ^= *p++;
        hash *= 16777619UL;
    }
    hash ^= '|';
    hash *= 16777619UL;
    p = reinterpret_cast<const uint8_t *>(normCall);
    while (*p) {
        hash ^= *p++;
        hash *= 16777619UL;
    }
    return hash;
}

static void route_cache_key(const char *hex, const char *callsign, char *out, size_t on) {
    snprintf(out, on, "r%08lx", static_cast<unsigned long>(route_identity_hash(hex, callsign)));
}

static void route_learn_key(const char *callsign, char *out, size_t on) {
    snprintf(out, on, "l%08lx", static_cast<unsigned long>(route_identity_hash("", callsign)));
}

#define ROUTE_FMT_VER 10   // bump to invalidate cache after switching to two-line route formatting
#define ROUTE_LEARN_FMT_VER 1

static void route_fetch_reset_debug() {
    snprintf(s_lastFetchMode, sizeof(s_lastFetchMode), "%s", "none");
    s_lastFetchUrl[0] = 0;
    s_lastFetchStatus = 0;
}

static bool route_transport_cooling(const char *tag = nullptr) {
    if (!s_routeTransportCooldownUntilMs) return false;
    const int32_t remainingMs = static_cast<int32_t>(s_routeTransportCooldownUntilMs - millis());
    if (remainingMs <= 0) return false;
    if (tag && tag[0]) {
        Serial.printf("[route] %s skipped: transport cooldown %ld ms remaining\n",
                      tag, static_cast<long>(remainingMs));
    }
    return true;
}

static void route_note_transport_failure(const char *tag, int code) {
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    const String why = HTTPClient::errorToString(code);
    Serial.printf("[route] %s transport %d (%s), biggest internal block=%u\n",
                  tag ? tag : "-", code, why.c_str(), (unsigned)largest);
    s_routeTransportCooldownUntilMs = millis() + ROUTE_TRANSPORT_BACKOFF_MS;
}

static bool route_memory_ok(const char *tag, size_t minBlock) {
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    if (largest >= minBlock) return true;
    Serial.printf("[route] %s skipped: low memory, biggest internal block=%u (<%u)\n",
                  tag ? tag : "-", (unsigned)largest, (unsigned)minBlock);
    return false;
}

void route_cache_begin() {
    Preferences p;
    if (!p.begin("routes", false)) return;
    if (p.getUChar("__v", 0) != ROUTE_FMT_VER) { p.clear(); p.putUChar("__v", ROUTE_FMT_VER); }
    p.end();
    Preferences l;
    if (!l.begin("routelearn", false)) return;
    if (l.getUChar("__v", 0) != ROUTE_LEARN_FMT_VER) { l.clear(); l.putUChar("__v", ROUTE_LEARN_FMT_VER); }
    l.end();
}

bool route_cache_get(const char *hex, const char *callsign, char *line1, size_t l1n, char *line2, size_t l2n) {
    if (l1n) line1[0] = 0;
    if (l2n) line2[0] = 0;
    if (!ROUTE_NVS_CACHE_READS_ENABLED) return false;
    if ((!hex || !hex[0]) && (!callsign || !callsign[0])) return false;
    char key[12];
    route_cache_key(hex, callsign, key, sizeof(key));
    if (!key[0]) return false;
    Preferences p;
    if (!p.begin("routes", true)) return false;
    String v = p.getString(key, "");     // stored as "epoch|from|to"
    p.end();
    if (v.length() == 0) return false;
    const int b1 = v.indexOf('|');
    if (b1 < 0) return false;
    const uint32_t ts = (uint32_t)v.substring(0, b1).toInt();
    const String rest = v.substring(b1 + 1);
    const int b2 = rest.indexOf('|');
    if (b2 < 0) return false;
    const uint32_t now = (uint32_t)time(nullptr);    // expire stale routes (reused callsigns)
    if (now > 1700000000UL && ts > 1700000000UL && (now - ts) > ROUTE_CACHE_TTL_S) return false;
    snprintf(line1, l1n, "%s", rest.substring(0, b2).c_str());
    snprintf(line2, l2n, "%s", rest.substring(b2 + 1).c_str());
    return true;
}

void route_cache_put(const char *hex, const char *callsign, const char *line1, const char *line2) {
    if ((!hex || !hex[0]) && (!callsign || !callsign[0])) return;
    char key[12];
    route_cache_key(hex, callsign, key, sizeof(key));
    if (!key[0]) return;
    Preferences p;
    if (!p.begin("routes", false)) return;
    int n = p.getInt("__n", 0);
    if (n >= ROUTE_CACHE_MAX) { p.clear(); n = 0; }   // wrap to bound NVS usage
    String v = String((uint32_t)time(nullptr)) + "|" + String(line1 ? line1 : "") + "|" + String(line2 ? line2 : "");
    if (p.putString(key, v) > 0) p.putInt("__n", n + 1);
    p.end();
}

// Most recognizable short airport label: a cleaned-up name ("Teesside", "Palma de
// Mallorca", "London Heathrow"), falling back to the municipality, then the IATA code.
static void pick_airport(JsonObjectConst ap, char *out, size_t n) {
    String s = (const char *)(ap["name"] | "");
    s.replace(" International Airport", "");
    s.replace(" Regional Airport", "");
    s.replace(" Airport", "");
    s.replace(" International", "");
    s.trim();
    if (s.length() == 0 || s.length() > 18) {           // name missing or too long -> municipality/IATA
        const char *muni = ap["municipality"] | "";
        const char *iata = ap["iata_code"] | "";
        snprintf(out, n, "%s", muni[0] ? muni : iata);
        return;
    }
    snprintf(out, n, "%s", s.c_str());
}

static void fill_endpoint(JsonObjectConst ap, RouteEndpoint &ep) {
    ep.label[0] = 0;
    ep.code[0] = 0;
    ep.lat = ep.lon = 0.0f;
    ep.hasCoord = false;
    pick_airport(ap, ep.label, sizeof(ep.label));
    const char *iata = ap["iata_code"] | "";
    const char *icao = ap["icao_code"] | "";
    snprintf(ep.code, sizeof(ep.code), "%s", icao[0] ? icao : iata);
    if (!ap["latitude"].isNull() && !ap["longitude"].isNull()) {
        ep.lat = ap["latitude"].as<float>();
        ep.lon = ap["longitude"].as<float>();
        ep.hasCoord = true;
    }
}

static void trim_ascii(char *s) {
    if (!s) return;
    size_t len = strlen(s);
    size_t start = 0;
    while (start < len && (s[start] == ' ' || s[start] == '\t' || s[start] == '\r' || s[start] == '\n')) start++;
    size_t end = len;
    while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r' || s[end - 1] == '\n')) end--;
    if (start > 0) memmove(s, s + start, end - start);
    s[end - start] = 0;
}

static void html_decode_basic(char *s) {
    if (!s || !s[0]) return;
    char out[64];
    size_t oi = 0;
    for (size_t i = 0; s[i] && oi + 1 < sizeof(out); ) {
        if (!strncmp(s + i, "&#39;", 5)) {
            out[oi++] = '\'';
            i += 5;
        } else if (!strncmp(s + i, "&amp;", 5)) {
            out[oi++] = '&';
            i += 5;
        } else if (!strncmp(s + i, "&quot;", 6)) {
            out[oi++] = '"';
            i += 6;
        } else {
            out[oi++] = s[i++];
        }
    }
    out[oi] = 0;
    snprintf(s, 64, "%s", out);
}

static bool extract_attr_value(const String &html, const char *needle, char *out, size_t on) {
    if (!needle || !needle[0] || !out || on == 0) return false;
    const int tagStart = html.indexOf(needle);
    if (tagStart < 0) return false;
    const int contentPos = html.indexOf("content=\"", tagStart);
    if (contentPos < 0) return false;
    const int valueStart = contentPos + 9;
    const int valueEnd = html.indexOf('"', valueStart);
    if (valueEnd <= valueStart) return false;
    snprintf(out, on, "%s", html.substring(valueStart, valueEnd).c_str());
    return true;
}

static bool extract_between(const String &html, const char *startNeedle, const char *endNeedle,
                            char *out, size_t on) {
    if (!startNeedle || !endNeedle || !out || on == 0) return false;
    const int start = html.indexOf(startNeedle);
    if (start < 0) return false;
    const int valueStart = start + strlen(startNeedle);
    const int end = html.indexOf(endNeedle, valueStart);
    if (end <= valueStart) return false;
    snprintf(out, on, "%s", html.substring(valueStart, end).c_str());
    return true;
}

static void code_to_label(const char *code, char *out, size_t on) {
    if (!out || on == 0) return;
    out[0] = 0;
    if (!code || !code[0]) return;
    char norm[8] = "";
    normalize_token(code, norm, sizeof(norm));
    if (strlen(norm) == 4 && norm[0] == 'K') snprintf(out, on, "%s", norm + 1);
    else                                      snprintf(out, on, "%s", norm);
}

static bool parse_flightaware_description(const char *desc, RouteEndpoint &from, RouteEndpoint &to) {
    if (!desc || !desc[0]) return false;
    const char *fromNeedle = strstr(desc, " flight from ");
    if (!fromNeedle) return false;
    fromNeedle += 13;
    const char *toNeedle = strstr(fromNeedle, " to ");
    if (!toNeedle || toNeedle <= fromNeedle) return false;

    char fromLabel[sizeof(from.label)] = "";
    char toLabel[sizeof(to.label)] = "";
    size_t fromLen = static_cast<size_t>(toNeedle - fromNeedle);
    size_t toLen = strlen(toNeedle + 4);
    if (fromLen >= sizeof(fromLabel)) fromLen = sizeof(fromLabel) - 1;
    if (toLen >= sizeof(toLabel)) toLen = sizeof(toLabel) - 1;
    memcpy(fromLabel, fromNeedle, fromLen);
    fromLabel[fromLen] = 0;
    memcpy(toLabel, toNeedle + 4, toLen);
    toLabel[toLen] = 0;
    html_decode_basic(fromLabel);
    html_decode_basic(toLabel);
    trim_ascii(fromLabel);
    trim_ascii(toLabel);
    if (!fromLabel[0] || !toLabel[0]) return false;
    snprintf(from.label, sizeof(from.label), "%s", fromLabel);
    snprintf(to.label, sizeof(to.label), "%s", toLabel);
    return true;
}

static void endpoint_code_or_label(const RouteEndpoint &ep, char *out, size_t on) {
    if (!out || on == 0) return;
    out[0] = 0;
    if (ep.code[0]) snprintf(out, on, "%s", ep.code);
    else            snprintf(out, on, "%s", ep.label[0] ? ep.label : "?");
}

static void format_route_lines(const RouteEndpoint &fromEp, const RouteEndpoint &toEp,
                               bool flightAware, bool oldRoute,
                               char *line1, size_t l1n, char *line2, size_t l2n) {
    char fromCode[16] = "", toCode[16] = "";
    endpoint_code_or_label(fromEp, fromCode, sizeof(fromCode));
    endpoint_code_or_label(toEp, toCode, sizeof(toCode));
    snprintf(line1, l1n, "%s -> %s%s%s",
             fromCode[0] ? fromCode : "?",
             toCode[0] ? toCode : "?",
             flightAware ? " (FlightAware)" : "",
             oldRoute ? " (old)" : "");
    snprintf(line2, l2n, "%s -> %s",
             fromEp.label[0] ? fromEp.label : (fromCode[0] ? fromCode : "?"),
             toEp.label[0] ? toEp.label : (toCode[0] ? toCode : "?"));
}

static bool route_fetch_flightaware(const char *callsign, char *line1, size_t l1n, char *line2, size_t l2n) {
    if (!callsign || !callsign[0]) return false;
    if (route_transport_cooling("flightaware")) return false;
    if (!route_memory_ok("flightaware", ROUTE_MIN_FLIGHTAWARE_BLOCK_BYTES)) return false;
    Serial.printf("[route] flightaware trying: %s\n", callsign);

    char url[128];
    snprintf(url, sizeof(url), "https://www.flightaware.com/live/flight/%s", callsign);
    snprintf(s_lastFetchUrl, sizeof(s_lastFetchUrl), "%s", url);

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(3000);
    http.setTimeout(5000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    if (!http.begin(client, url)) return false;
    http.addHeader("User-Agent", FLIGHTAWARE_USER_AGENT);
    http.addHeader("Accept", "text/html,application/xhtml+xml");
    http.addHeader("Accept-Language", "en-US,en;q=0.9");
    http.addHeader("Accept-Encoding", "identity");
    http.addHeader("Cache-Control", "no-cache");

    const int code = http.GET();
    s_lastFetchStatus = code;
    if (code != 200) {
        if (code < 0) route_note_transport_failure("flightaware", code);
        http.end();
        return false;
    }

    String html;
    html.reserve(6144);
    WiFiClient *stream = http.getStreamPtr();
    const uint32_t start = millis();
    while (http.connected() && html.length() < FLIGHTAWARE_SCRAPE_LIMIT && (millis() - start) < 2500UL) {
        int avail = stream->available();
        if (avail <= 0) {
            delay(10);
            continue;
        }
        while (avail-- > 0 && html.length() < FLIGHTAWARE_SCRAPE_LIMIT) {
            int ch = stream->read();
            if (ch < 0) break;
            html += static_cast<char>(ch);
        }
        if (html.indexOf("<meta name=\"origin\"") >= 0 &&
            html.indexOf("<meta name=\"destination\"") >= 0 &&
            html.indexOf("flight from ") >= 0) {
            break;
        }
    }
    http.end();
    if (html.length() == 0) return false;

    RouteEndpoint faFrom = {}, faTo = {};
    char desc[128] = "";
    char originCode[8] = "";
    char destCode[8] = "";
    const bool haveDesc = extract_attr_value(html, "<meta property=\"og:description\"", desc, sizeof(desc)) ||
                          extract_attr_value(html, "<meta name=\"twitter:description\"", desc, sizeof(desc)) ||
                          extract_between(html, "\"og:description\" content=\"", "\"", desc, sizeof(desc));
    const bool haveOrigin = extract_attr_value(html, "<meta name=\"origin\"", originCode, sizeof(originCode));
    const bool haveDest = extract_attr_value(html, "<meta name=\"destination\"", destCode, sizeof(destCode));

    if (haveDesc) parse_flightaware_description(desc, faFrom, faTo);
    if (haveOrigin) snprintf(faFrom.code, sizeof(faFrom.code), "%s", originCode);
    if (haveDest)   snprintf(faTo.code, sizeof(faTo.code), "%s", destCode);

    if (!faFrom.label[0] && haveOrigin) code_to_label(originCode, faFrom.label, sizeof(faFrom.label));
    if (!faTo.label[0] && haveDest)     code_to_label(destCode, faTo.label, sizeof(faTo.label));
    if (!faFrom.label[0] || !faTo.label[0]) return false;

    format_route_lines(faFrom, faTo, true, false, line1, l1n, line2, l2n);
    snprintf(s_lastFetchMode, sizeof(s_lastFetchMode), "%s", haveDesc ? "fa-og" : "fa-meta");
    return true;
}

static bool is_commercial_callsign(const char *callsign) {
    char cs[12] = "";
    normalize_token(callsign, cs, sizeof(cs));
    if (!(cs[0] >= 'A' && cs[0] <= 'Z' && cs[1] >= 'A' && cs[1] <= 'Z' && cs[2] >= 'A' && cs[2] <= 'Z')) return false;
    for (int i = 3; cs[i]; ++i) if (cs[i] >= '0' && cs[i] <= '9') return true;
    return false;
}

static float angle_diff_deg(float a, float b) {
    float d = fmodf(fabsf(a - b), 360.0f);
    return d > 180.0f ? 360.0f - d : d;
}

static float route_excess_km(const RouteEndpoint &from, const RouteEndpoint &to, float lat, float lon) {
    if (!from.hasCoord || !to.hasCoord) return 1.0e9f;
    const float od = (float)geo::haversineKm(from.lat, from.lon, to.lat, to.lon);
    const float ao = (float)geo::haversineKm(from.lat, from.lon, lat, lon);
    const float ad = (float)geo::haversineKm(lat, lon, to.lat, to.lon);
    return ao + ad - od;
}

static int route_confidence_score(const RouteEndpoint &from, const RouteEndpoint &to,
                                  float lat, float lon, float track) {
    if (!from.label[0] || !to.label[0]) return -4;
    int score = 0;
    if (from.hasCoord && to.hasCoord) {
        const float excess = route_excess_km(from, to, lat, lon);
        const float ao = (float)geo::haversineKm(from.lat, from.lon, lat, lon);
        const float ad = (float)geo::haversineKm(lat, lon, to.lat, to.lon);
        if      (excess < 80.0f)  score += 4;
        else if (excess < 180.0f) score += 3;
        else if (excess < 350.0f) score += 2;
        else if (excess < 600.0f) score += 1;
        else                      score -= 3;
        if      (ao < 60.0f || ad < 60.0f)  score += 2;
        else if (ao < 150.0f || ad < 150.0f) score += 1;
        if (!isnan(track)) {
            const float bt = (float)geo::bearingDeg(lat, lon, to.lat, to.lon);
            const float bf = (float)geo::bearingDeg(lat, lon, from.lat, from.lon);
            const float dt = angle_diff_deg(track, bt);
            const float df = angle_diff_deg(track, bf);
            if (dt + 30.0f < df) score += 2;
            else if (df + 30.0f < dt) score -= 2;
        }
    } else score -= 1;
    return score;
}

static bool maybe_flip_direction(RouteEndpoint &from, RouteEndpoint &to, float lat, float lon, float track) {
    if (!from.hasCoord || !to.hasCoord || isnan(track)) return false;
    const float ao = (float)geo::haversineKm(from.lat, from.lon, lat, lon);
    const float ad = (float)geo::haversineKm(to.lat, to.lon, lat, lon);
    const float bf = (float)geo::bearingDeg(lat, lon, from.lat, from.lon);
    const float bt = (float)geo::bearingDeg(lat, lon, to.lat, to.lon);
    const float df = angle_diff_deg(track, bf);
    const float dt = angle_diff_deg(track, bt);
    if (ad + 80.0f < ao && df + 35.0f < dt && route_excess_km(from, to, lat, lon) < 250.0f) {
        RouteEndpoint tmp = from; from = to; to = tmp;
        return true;
    }
    return false;
}

static bool same_pair(const RouteEndpoint &aFrom, const RouteEndpoint &aTo,
                      const RouteEndpoint &bFrom, const RouteEndpoint &bTo) {
    const bool sameDir = strcmp(aFrom.code, bFrom.code) == 0 && strcmp(aTo.code, bTo.code) == 0;
    const bool revDir  = strcmp(aFrom.code, bTo.code) == 0 && strcmp(aTo.code, bFrom.code) == 0;
    return sameDir || revDir ||
           ((strcmp(aFrom.label, bFrom.label) == 0 && strcmp(aTo.label, bTo.label) == 0) ||
            (strcmp(aFrom.label, bTo.label) == 0 && strcmp(aTo.label, bFrom.label) == 0));
}

static bool route_is_strong_endpoint_match(const RouteEndpoint &from, const RouteEndpoint &to,
                                           float lat, float lon) {
    if (!from.hasCoord || !to.hasCoord) return false;
    const float ao = (float)geo::haversineKm(from.lat, from.lon, lat, lon);
    const float ad = (float)geo::haversineKm(lat, lon, to.lat, to.lon);
    const float excess = route_excess_km(from, to, lat, lon);
    return excess < 180.0f && (ao < 150.0f || ad < 150.0f);
}

static bool route_learn_get(const char *callsign, LearnedRoute &out) {
    out.valid = false;
    if (!is_commercial_callsign(callsign)) return false;
    char key[12];
    route_learn_key(callsign, key, sizeof(key));
    Preferences p;
    if (!p.begin("routelearn", true)) return false;
    String v = p.getString(key, "");
    p.end();
    if (v.length() == 0) return false;
    const int partsMax = 10;
    int idx[partsMax] = {0};
    int partCount = 0;
    for (int i = 0; i < v.length() && partCount < partsMax - 1; ++i) if (v[i] == '|') idx[partCount++] = i;
    if (partCount < 9) return false;
    auto part = [&](int n) -> String {
        const int start = (n == 0) ? 0 : idx[n - 1] + 1;
        const int end = (n < partCount) ? idx[n] : (int)v.length();
        return v.substring(start, end);
    };
    out.stamp = (uint32_t)part(0).toInt();
    out.score = part(1).toInt();
    snprintf(out.from.label, sizeof(out.from.label), "%s", part(2).c_str());
    snprintf(out.to.label, sizeof(out.to.label), "%s", part(3).c_str());
    out.from.lat = part(4).toFloat();
    out.from.lon = part(5).toFloat();
    out.to.lat = part(6).toFloat();
    out.to.lon = part(7).toFloat();
    snprintf(out.from.code, sizeof(out.from.code), "%s", part(8).c_str());
    snprintf(out.to.code, sizeof(out.to.code), "%s", part(9).c_str());
    out.from.hasCoord = fabsf(out.from.lat) > 0.01f || fabsf(out.from.lon) > 0.01f;
    out.to.hasCoord = fabsf(out.to.lat) > 0.01f || fabsf(out.to.lon) > 0.01f;
    const uint32_t now = (uint32_t)time(nullptr);
    if (now > 1700000000UL && out.stamp > 1700000000UL && (now - out.stamp) > ROUTE_LEARN_TTL_S) return false;
    out.valid = true;
    return true;
}

static void route_learn_put(const char *callsign, const RouteEndpoint &from, const RouteEndpoint &to, int score) {
    if (!is_commercial_callsign(callsign) || !from.label[0] || !to.label[0]) return;
    char key[12];
    route_learn_key(callsign, key, sizeof(key));
    Preferences p;
    if (!p.begin("routelearn", false)) return;
    int n = p.getInt("__n", 0);
    if (n >= ROUTE_LEARN_MAX) { p.clear(); p.putUChar("__v", ROUTE_LEARN_FMT_VER); n = 0; }
    String v = String((uint32_t)time(nullptr)) + "|" + String(score) + "|" +
               String(from.label) + "|" + String(to.label) + "|" +
               String(from.lat, 4) + "|" + String(from.lon, 4) + "|" +
               String(to.lat, 4) + "|" + String(to.lon, 4) + "|" +
               String(from.code) + "|" + String(to.code);
    if (p.putString(key, v) > 0) p.putInt("__n", n + 1);
    p.end();
}

bool route_fetch(const char *hex, const char *callsign, float targetLat, float targetLon, float targetTrack,
                 char *line1, size_t l1n, char *line2, size_t l2n) {
    route_fetch_reset_debug();
    if (l1n) line1[0] = 0;
    if (l2n) line2[0] = 0;
    if ((!hex || !hex[0]) && (!callsign || !callsign[0])) return false;
    if (WiFi.status() != WL_CONNECTED) return false;
    if (route_transport_cooling("lookup")) {
        snprintf(s_lastFetchMode, sizeof(s_lastFetchMode), "%s", "cooldown");
        s_lastFetchStatus = -1;
        return false;
    }
    if (!route_memory_ok("lookup", ROUTE_MIN_TLS_BLOCK_BYTES)) {
        snprintf(s_lastFetchMode, sizeof(s_lastFetchMode), "%s", "lowmem");
        s_lastFetchStatus = -1;
        return false;
    }

    char cs[12] = "", hs[12] = "";
    normalize_token(callsign, cs, sizeof(cs));
    normalize_token(hex, hs, sizeof(hs));
    if (!hs[0] && !cs[0]) return false;

    if (cs[0] && route_fetch_flightaware(cs, line1, l1n, line2, l2n)) return true;

    char url[128];
    if (hs[0] && cs[0]) {
        snprintf(url, sizeof(url), "https://api.adsbdb.com/v0/aircraft/%s?callsign=%s", hs, cs);
        snprintf(s_lastFetchMode, sizeof(s_lastFetchMode), "%s", "adsbdb-air");
    } else {
        snprintf(url, sizeof(url), "https://api.adsbdb.com/v0/callsign/%s", cs);
        snprintf(s_lastFetchMode, sizeof(s_lastFetchMode), "%s", "adsbdb-call");
    }
    snprintf(s_lastFetchUrl, sizeof(s_lastFetchUrl), "%s", url);

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(3000);   // short: runs on the feed task, don't stall the live poll
    http.setTimeout(6000);
    if (!http.begin(client, url)) return false;
    http.addHeader("User-Agent", ADSB_USER_AGENT);

    const int code = http.GET();
    s_lastFetchStatus = code;
    if (code != 200) {
        if (code < 0) route_note_transport_failure("adsbdb", code);
        http.end();
        return false;
    }

    JsonDocument filter(&s_jsonPsram);
    filter["response"]["flightroute"]["origin"]["municipality"] = true;
    filter["response"]["flightroute"]["origin"]["iata_code"] = true;
    filter["response"]["flightroute"]["origin"]["icao_code"] = true;
    filter["response"]["flightroute"]["origin"]["name"] = true;
    filter["response"]["flightroute"]["origin"]["latitude"] = true;
    filter["response"]["flightroute"]["origin"]["longitude"] = true;
    filter["response"]["flightroute"]["destination"]["municipality"] = true;
    filter["response"]["flightroute"]["destination"]["iata_code"] = true;
    filter["response"]["flightroute"]["destination"]["icao_code"] = true;
    filter["response"]["flightroute"]["destination"]["name"] = true;
    filter["response"]["flightroute"]["destination"]["latitude"] = true;
    filter["response"]["flightroute"]["destination"]["longitude"] = true;

    JsonDocument doc(&s_jsonPsram);
    DeserializationError err = deserializeJson(doc, http.getStream(),
                                               DeserializationOption::Filter(filter));
    http.end();
    if (err) return false;

    JsonObjectConst fr = doc["response"]["flightroute"].as<JsonObjectConst>();
    if (fr.isNull()) return false;   // "unknown callsign" etc.

    RouteEndpoint rawFrom = {}, rawTo = {};
    fill_endpoint(fr["origin"].as<JsonObjectConst>(), rawFrom);
    fill_endpoint(fr["destination"].as<JsonObjectConst>(), rawTo);
    RouteEndpoint bestFrom = rawFrom, bestTo = rawTo;
    bool flipped = maybe_flip_direction(bestFrom, bestTo, targetLat, targetLon, targetTrack);
    int bestScore = route_confidence_score(bestFrom, bestTo, targetLat, targetLon, targetTrack);

    LearnedRoute learned = {};
    if (route_learn_get(callsign, learned)) {
        RouteEndpoint learnFrom = learned.from, learnTo = learned.to;
        maybe_flip_direction(learnFrom, learnTo, targetLat, targetLon, targetTrack);
        int learnedScore = route_confidence_score(learnFrom, learnTo, targetLat, targetLon, targetTrack);
        learnedScore += (learned.score >= ROUTE_SCORE_STRONG) ? 2 : 1;
        if ((!same_pair(rawFrom, rawTo, learnFrom, learnTo) && learnedScore >= bestScore + 3) ||
            (bestScore < 1 && learnedScore > bestScore)) {
            bestFrom = learnFrom;
            bestTo = learnTo;
            bestScore = learnedScore;
            snprintf(s_lastFetchMode, sizeof(s_lastFetchMode), "%s", "adsbdb-learn");
        }
    }

    if (bestScore < 1) {
        format_route_lines(bestFrom, bestTo, false, true, line1, l1n, line2, l2n);
        snprintf(s_lastFetchMode, sizeof(s_lastFetchMode), "%s", "adsbdb-weak");
        return (line1[0] || line2[0]);
    }

    if (flipped && strcmp(s_lastFetchMode, "adsbdb-learn") != 0) {
        snprintf(s_lastFetchMode, sizeof(s_lastFetchMode), "%s", "adsbdb-flip");
    }

    if (bestScore >= ROUTE_SCORE_PERSIST &&
        (route_is_strong_endpoint_match(bestFrom, bestTo, targetLat, targetLon) ||
         bestScore >= ROUTE_SCORE_STRONG)) {
        route_learn_put(callsign, bestFrom, bestTo, bestScore > 9 ? 9 : bestScore);
    }

    format_route_lines(bestFrom, bestTo, false, false, line1, l1n, line2, l2n);
    return (line1[0] || line2[0]);
}

const char *route_fetch_last_mode() { return s_lastFetchMode; }
const char *route_fetch_last_url() { return s_lastFetchUrl; }
int route_fetch_last_status() { return s_lastFetchStatus; }
