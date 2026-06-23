// Shared route state. std::mutex works on both ESP32 (Arduino/FreeRTOS) and the
// native simulator, so the same code guards the cross-thread access on the device.
#include "route.h"
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <mutex>

static std::mutex s_m;
#define ROUTE_RESULTS 16
#define ROUTE_QUEUE   16
struct RouteResult { char call[12], from[40], to[40]; uint32_t stamp; };
static RouteResult s_results[ROUTE_RESULTS] = {};
static char s_queue[ROUTE_QUEUE][12] = {};
static char s_priorityCall[12] = "";
static int s_qCount = 0;
static uint32_t s_stamp = 0;

static int find_result(const char *call) {
    for (int i = 0; i < ROUTE_RESULTS; ++i)
        if (s_results[i].call[0] && strcmp(call, s_results[i].call) == 0) return i;
    return -1;
}

static void queue_request(const char *callsign, bool priority) {
    if (!callsign || !callsign[0]) return;
    std::lock_guard<std::mutex> g(s_m);
    if (priority) snprintf(s_priorityCall, sizeof(s_priorityCall), "%s", callsign);
    const int ri = find_result(callsign);
    if (ri >= 0) { s_results[ri].stamp = ++s_stamp; return; }
    for (int i = 0; i < s_qCount; ++i) {
        if (strcmp(callsign, s_queue[i]) != 0) continue;
        if (priority && i > 0) {
            char tmp[12]; snprintf(tmp, sizeof(tmp), "%s", s_queue[i]);
            for (int j = i; j > 0; --j) snprintf(s_queue[j], sizeof(s_queue[j]), "%s", s_queue[j - 1]);
            snprintf(s_queue[0], sizeof(s_queue[0]), "%s", tmp);
        }
        return;
    }
    if (priority) {
        if (s_qCount < ROUTE_QUEUE) ++s_qCount;
        for (int i = s_qCount - 1; i > 0; --i) snprintf(s_queue[i], sizeof(s_queue[i]), "%s", s_queue[i - 1]);
        snprintf(s_queue[0], sizeof(s_queue[0]), "%s", callsign);
    } else if (s_qCount < ROUTE_QUEUE) {
        snprintf(s_queue[s_qCount++], sizeof(s_queue[0]), "%s", callsign);
    }
}

void route_request(const char *callsign) {
    queue_request(callsign, true);
}
void route_prefetch(const char *callsign) { queue_request(callsign, false); }
void route_cancel_prefetches() {
    std::lock_guard<std::mutex> g(s_m);
    int keep = -1;
    for (int i = 0; i < s_qCount; ++i)
        if (s_priorityCall[0] && strcmp(s_queue[i], s_priorityCall) == 0) { keep = i; break; }
    if (keep >= 0) {
        snprintf(s_queue[0], sizeof(s_queue[0]), "%s", s_queue[keep]);
        s_qCount = 1;
    } else s_qCount = 0;
}

bool route_pending(char *callOut, size_t n) {
    std::lock_guard<std::mutex> g(s_m);
    if (s_qCount > 0) {
        snprintf(callOut, n, "%s", s_queue[0]);
        return true;
    }
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

void route_store(const char *callsign, const char *from, const char *to) {
    std::lock_guard<std::mutex> g(s_m);
    if (!callsign || !callsign[0]) return;
    int slot = find_result(callsign);
    if (slot < 0) {
        slot = 0;
        for (int i = 0; i < ROUTE_RESULTS; ++i) {
            if (!s_results[i].call[0]) { slot = i; break; }
            if (s_results[i].stamp < s_results[slot].stamp) slot = i;
        }
    }
    snprintf(s_results[slot].call, sizeof(s_results[slot].call), "%s", callsign);
    ascii_fold(from, s_results[slot].from, sizeof(s_results[slot].from));
    ascii_fold(to,   s_results[slot].to,   sizeof(s_results[slot].to));
    s_results[slot].stamp = ++s_stamp;
    for (int i = 0; i < s_qCount;) {
        if (strcmp(callsign, s_queue[i]) == 0) {
            for (int j = i; j + 1 < s_qCount; ++j) snprintf(s_queue[j], sizeof(s_queue[j]), "%s", s_queue[j + 1]);
            --s_qCount;
        } else ++i;
    }
}

bool route_get(const char *callsign, char *from, size_t fn, char *to, size_t tn) {
    std::lock_guard<std::mutex> g(s_m);
    const int i = callsign ? find_result(callsign) : -1;
    if (i >= 0) {
        snprintf(from, fn, "%s", s_results[i].from);
        snprintf(to, tn, "%s", s_results[i].to);
        s_results[i].stamp = ++s_stamp;
        return true;
    }
    return false;
}
