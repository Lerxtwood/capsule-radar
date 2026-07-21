// Shared route state. std::mutex works on both ESP32 (Arduino/FreeRTOS) and the
// native simulator, so the same code guards the cross-thread access on the device.
#include "route.h"
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <mutex>
#include <chrono>

static std::mutex s_m;
#define ROUTE_RESULTS 16
#define ROUTE_QUEUE   16
struct RouteItem { char key[24], hex[12], call[12]; float lat, lon, track; };
struct RouteResult { char key[24], line1[96], line2[96]; uint32_t stamp; uint64_t fetchedMs; };
static RouteResult s_results[ROUTE_RESULTS] = {};
static RouteItem s_queue[ROUTE_QUEUE] = {};
static RouteItem s_priorityItem = {};
static int s_qCount = 0;
static uint32_t s_stamp = 0;
static constexpr uint64_t ROUTE_RESULT_TTL_MS = 15ULL * 60ULL * 1000ULL;

static uint64_t monotonic_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

static bool route_result_expired(const RouteResult &result) {
    return result.fetchedMs != 0 && (monotonic_ms() - result.fetchedMs) > ROUTE_RESULT_TTL_MS;
}

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

static void route_identity_key(const char *hex, const char *callsign, char *out, size_t on) {
    char normHex[12] = "", normCall[12] = "";
    normalize_token(hex, normHex, sizeof(normHex));
    normalize_token(callsign, normCall, sizeof(normCall));
    if (normHex[0] && normCall[0]) snprintf(out, on, "%s/%s", normHex, normCall);
    else if (normHex[0])          snprintf(out, on, "%s", normHex);
    else                          snprintf(out, on, "%s", normCall);
}

static int find_result(const char *key) {
    for (int i = 0; i < ROUTE_RESULTS; ++i)
        if (s_results[i].key[0] && strcmp(key, s_results[i].key) == 0) return i;
    return -1;
}

static void queue_request(const char *hex, const char *callsign, float lat, float lon, float track, bool priority) {
    char key[24] = "";
    route_identity_key(hex, callsign, key, sizeof(key));
    if (!key[0]) return;
    std::lock_guard<std::mutex> g(s_m);
    if (priority) {
        snprintf(s_priorityItem.key, sizeof(s_priorityItem.key), "%s", key);
        snprintf(s_priorityItem.hex, sizeof(s_priorityItem.hex), "%s", hex ? hex : "");
        snprintf(s_priorityItem.call, sizeof(s_priorityItem.call), "%s", callsign ? callsign : "");
        s_priorityItem.lat = lat;
        s_priorityItem.lon = lon;
        s_priorityItem.track = track;
    }
    const int ri = find_result(key);
    if (ri >= 0) {
        if (!route_result_expired(s_results[ri])) {
            s_results[ri].stamp = ++s_stamp;
            return;
        }
        s_results[ri].key[0] = 0;
            s_results[ri].line1[0] = 0;
            s_results[ri].line2[0] = 0;
            s_results[ri].fetchedMs = 0;
    }
    for (int i = 0; i < s_qCount; ++i) {
        if (strcmp(key, s_queue[i].key) != 0) continue;
        if (priority && i > 0) {
            RouteItem tmp = s_queue[i];
            for (int j = i; j > 0; --j) s_queue[j] = s_queue[j - 1];
            s_queue[0] = tmp;
        }
        return;
    }
    if (priority) {
        if (s_qCount < ROUTE_QUEUE) ++s_qCount;
        for (int i = s_qCount - 1; i > 0; --i) s_queue[i] = s_queue[i - 1];
        snprintf(s_queue[0].key, sizeof(s_queue[0].key), "%s", key);
        snprintf(s_queue[0].hex, sizeof(s_queue[0].hex), "%s", hex ? hex : "");
        snprintf(s_queue[0].call, sizeof(s_queue[0].call), "%s", callsign ? callsign : "");
        s_queue[0].lat = lat;
        s_queue[0].lon = lon;
        s_queue[0].track = track;
    } else if (s_qCount < ROUTE_QUEUE) {
        snprintf(s_queue[s_qCount].key, sizeof(s_queue[s_qCount].key), "%s", key);
        snprintf(s_queue[s_qCount].hex, sizeof(s_queue[s_qCount].hex), "%s", hex ? hex : "");
        snprintf(s_queue[s_qCount].call, sizeof(s_queue[s_qCount].call), "%s", callsign ? callsign : "");
        s_queue[s_qCount].lat = lat;
        s_queue[s_qCount].lon = lon;
        s_queue[s_qCount].track = track;
        ++s_qCount;
    }
}

void route_request(const char *hex, const char *callsign, float lat, float lon, float track) {
    queue_request(hex, callsign, lat, lon, track, true);
}
void route_prefetch(const char *hex, const char *callsign, float lat, float lon, float track) { queue_request(hex, callsign, lat, lon, track, false); }
void route_cancel_prefetches() {
    std::lock_guard<std::mutex> g(s_m);
    int keep = -1;
    for (int i = 0; i < s_qCount; ++i)
        if (s_priorityItem.key[0] && strcmp(s_queue[i].key, s_priorityItem.key) == 0) { keep = i; break; }
    if (keep >= 0) {
        s_queue[0] = s_queue[keep];
        s_qCount = 1;
    } else s_qCount = 0;
}

bool route_pending(char *hexOut, size_t hn, char *callOut, size_t cn, float *latOut, float *lonOut, float *trackOut) {
    std::lock_guard<std::mutex> g(s_m);
    if (s_qCount > 0) {
        if (hn) snprintf(hexOut, hn, "%s", s_queue[0].hex);
        if (cn) snprintf(callOut, cn, "%s", s_queue[0].call);
        if (latOut) *latOut = s_queue[0].lat;
        if (lonOut) *lonOut = s_queue[0].lon;
        if (trackOut) *trackOut = s_queue[0].track;
        return true;
    }
    if (hn) hexOut[0] = 0;
    if (cn) callOut[0] = 0;
    if (latOut) *latOut = 0.0f;
    if (lonOut) *lonOut = 0.0f;
    if (trackOut) *trackOut = 0.0f / 0.0f;
    return false;
}

// Fold UTF-8 Latin accents (á, ñ, ü...) to ASCII so the LVGL font can render them
// (Montserrat has no accented glyphs -> they would show as a missing-glyph box).
static void ascii_fold(const char *in, char *out, size_t n) {
    size_t o = 0;
    for (size_t i = 0; in && in[i] && o + 1 < n;) {
        const unsigned char c = (unsigned char)in[i];
        if (c < 0x80) { out[o++] = in[i++]; continue; }
        if (c == 0xC3 && in[i + 1]) {            // Latin-1 Supplement
            const unsigned char d = (unsigned char)in[i + 1];
            char r;
            if      (d >= 0x80 && d <= 0x85) r = 'A';
            else if (d >= 0xA0 && d <= 0xA5) r = 'a';
            else if (d == 0x87)              r = 'C';
            else if (d == 0xA7)              r = 'c';
            else if (d >= 0x88 && d <= 0x8B) r = 'E';
            else if (d >= 0xA8 && d <= 0xAB) r = 'e';
            else if (d >= 0x8C && d <= 0x8F) r = 'I';
            else if (d >= 0xAC && d <= 0xAF) r = 'i';
            else if (d == 0x91)              r = 'N';
            else if (d == 0xB1)              r = 'n';
            else if (d >= 0x92 && d <= 0x96) r = 'O';
            else if (d >= 0xB2 && d <= 0xB6) r = 'o';
            else if (d >= 0x99 && d <= 0x9C) r = 'U';
            else if (d >= 0xB9 && d <= 0xBC) r = 'u';
            else if (d == 0x9F)              r = 's';   // ß
            else                             r = '?';
            out[o++] = r; i += 2; continue;
        }
        ++i;                                     // other multibyte: skip the sequence
        while ((unsigned char)in[i] >= 0x80 && (unsigned char)in[i] < 0xC0) ++i;
    }
    out[o] = 0;
}

void route_store(const char *hex, const char *callsign, const char *line1, const char *line2) {
    std::lock_guard<std::mutex> g(s_m);
    char key[24] = "";
    route_identity_key(hex, callsign, key, sizeof(key));
    if (!key[0]) return;
    int slot = find_result(key);
    if (slot < 0) {
        slot = 0;
        for (int i = 0; i < ROUTE_RESULTS; ++i) {
            if (!s_results[i].key[0]) { slot = i; break; }
            if (s_results[i].stamp < s_results[slot].stamp) slot = i;
        }
    }
    snprintf(s_results[slot].key, sizeof(s_results[slot].key), "%s", key);
    ascii_fold(line1, s_results[slot].line1, sizeof(s_results[slot].line1));
    ascii_fold(line2, s_results[slot].line2, sizeof(s_results[slot].line2));
    s_results[slot].stamp = ++s_stamp;
    s_results[slot].fetchedMs = monotonic_ms();
    for (int i = 0; i < s_qCount;) {
        if (strcmp(key, s_queue[i].key) == 0) {
            for (int j = i; j + 1 < s_qCount; ++j) s_queue[j] = s_queue[j + 1];
            --s_qCount;
        } else ++i;
    }
}

bool route_get(const char *hex, const char *callsign, char *line1, size_t l1n, char *line2, size_t l2n) {
    std::lock_guard<std::mutex> g(s_m);
    char key[24] = "";
    route_identity_key(hex, callsign, key, sizeof(key));
    const int i = key[0] ? find_result(key) : -1;
    if (i >= 0) {
        if (route_result_expired(s_results[i])) {
            s_results[i].key[0] = 0;
            s_results[i].line1[0] = 0;
            s_results[i].line2[0] = 0;
            s_results[i].fetchedMs = 0;
            return false;
        }
        snprintf(line1, l1n, "%s", s_results[i].line1);
        snprintf(line2, l2n, "%s", s_results[i].line2);
        s_results[i].stamp = ++s_stamp;
        return true;
    }
    return false;
}
