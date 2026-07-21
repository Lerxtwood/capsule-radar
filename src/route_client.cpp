// Route lookup via adsbdb.com (free, no API key).
#include "route_client.h"
#include "config.h"
#include "geo.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
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

static char s_lastFetchMode[16] = "none";
static char s_lastFetchUrl[128] = "";
static int  s_lastFetchStatus = 0;

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

#define ROUTE_FMT_VER 7   // bump to invalidate cache after adding route correlation heuristics
#define ROUTE_LEARN_FMT_VER 1

static void route_fetch_reset_debug() {
    snprintf(s_lastFetchMode, sizeof(s_lastFetchMode), "%s", "none");
    s_lastFetchUrl[0] = 0;
    s_lastFetchStatus = 0;
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

bool route_cache_get(const char *hex, const char *callsign, char *from, size_t fn, char *to, size_t tn) {
    if (fn) from[0] = 0;
    if (tn) to[0] = 0;
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
    snprintf(from, fn, "%s", rest.substring(0, b2).c_str());
    snprintf(to, tn, "%s", rest.substring(b2 + 1).c_str());
    return true;
}

void route_cache_put(const char *hex, const char *callsign, const char *from, const char *to) {
    if ((!hex || !hex[0]) && (!callsign || !callsign[0])) return;
    char key[12];
    route_cache_key(hex, callsign, key, sizeof(key));
    if (!key[0]) return;
    Preferences p;
    if (!p.begin("routes", false)) return;
    int n = p.getInt("__n", 0);
    if (n >= ROUTE_CACHE_MAX) { p.clear(); n = 0; }   // wrap to bound NVS usage
    String v = String((uint32_t)time(nullptr)) + "|" + String(from ? from : "") + "|" + String(to ? to : "");
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
    snprintf(ep.code, sizeof(ep.code), "%s", iata[0] ? iata : icao);
    if (!ap["latitude"].isNull() && !ap["longitude"].isNull()) {
        ep.lat = ap["latitude"].as<float>();
        ep.lon = ap["longitude"].as<float>();
        ep.hasCoord = true;
    }
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
                 char *from, size_t fn, char *to, size_t tn) {
    route_fetch_reset_debug();
    if (fn) from[0] = 0;
    if (tn) to[0] = 0;
    if ((!hex || !hex[0]) && (!callsign || !callsign[0])) return false;
    if (WiFi.status() != WL_CONNECTED) return false;

    char cs[12] = "", hs[12] = "";
    normalize_token(callsign, cs, sizeof(cs));
    normalize_token(hex, hs, sizeof(hs));
    if (!hs[0] && !cs[0]) return false;

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
    if (code != 200) { http.end(); return false; }

    JsonDocument filter;
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

    JsonDocument doc;
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
        snprintf(from, fn, "%s", bestFrom.label);
        if (bestTo.label[0]) snprintf(to, tn, "%s (old)", bestTo.label);
        else                 snprintf(to, tn, "%s", "(old)");
        snprintf(s_lastFetchMode, sizeof(s_lastFetchMode), "%s", "adsbdb-weak");
        return (from[0] || to[0]);
    }

    if (flipped && strcmp(s_lastFetchMode, "adsbdb-learn") != 0) {
        snprintf(s_lastFetchMode, sizeof(s_lastFetchMode), "%s", "adsbdb-flip");
    }

    if (bestScore >= ROUTE_SCORE_PERSIST &&
        (route_is_strong_endpoint_match(bestFrom, bestTo, targetLat, targetLon) ||
         bestScore >= ROUTE_SCORE_STRONG)) {
        route_learn_put(callsign, bestFrom, bestTo, bestScore > 9 ? 9 : bestScore);
    }

    snprintf(from, fn, "%s", bestFrom.label);
    snprintf(to, tn, "%s", bestTo.label);
    return (from[0] || to[0]);
}

const char *route_fetch_last_mode() { return s_lastFetchMode; }
const char *route_fetch_last_url() { return s_lastFetchUrl; }
int route_fetch_last_status() { return s_lastFetchStatus; }
