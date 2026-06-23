#include "photo.h"
#include <mutex>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#if defined(ESP_PLATFORM)
#include <esp_heap_caps.h>
#endif

#define PH_MAXW 232
#define PH_MAXH 156
#define PH_SLOTS 6
#define PH_QUEUE 12

struct PhotoSlot {
    lv_color_t *buf;
    char hex[10], type[8], credit[72];
    int w, h;
    bool done, ready;
    uint32_t stamp, failedAtMs;
};
static std::mutex s_m;
static PhotoSlot s_slots[PH_SLOTS] = {};
struct PhotoRequest { char hex[10], type[8]; };
static PhotoRequest s_queue[PH_QUEUE] = {};
static char s_priorityHex[10] = "";
static int s_qCount = 0, s_loading = -1;
static uint32_t s_stamp = 0;

static lv_color_t *ensure_buf(int slot) {
    if (slot < 0 || slot >= PH_SLOTS) return nullptr;
    if (!s_slots[slot].buf) {
        const size_t sz = (size_t)PH_MAXW * PH_MAXH * sizeof(lv_color_t);
#if defined(ESP_PLATFORM)
        s_slots[slot].buf = (lv_color_t *)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
#else
        s_slots[slot].buf = (lv_color_t *)malloc(sz);
#endif
    }
    return s_slots[slot].buf;
}

static int find_slot(const char *hex) {
    for (int i = 0; i < PH_SLOTS; ++i)
        if (s_slots[i].hex[0] && strcmp(hex, s_slots[i].hex) == 0) return i;
    return -1;
}

static void queue_request(const char *hex, const char *type, bool priority) {
    if (!hex || !hex[0]) return;
    std::lock_guard<std::mutex> g(s_m);
    if (priority) snprintf(s_priorityHex, sizeof(s_priorityHex), "%s", hex);
    const int si = find_slot(hex);
    if (si >= 0) {
        const bool negative = s_slots[si].done && !s_slots[si].ready;
        const bool learnedType = negative && type && type[0] && strcmp(type, s_slots[si].type) != 0;
        const bool retryDue = negative && priority &&
            (lv_tick_get() - s_slots[si].failedAtMs >= 15000U);
        if (learnedType || retryDue) {
            s_slots[si].hex[0] = s_slots[si].type[0] = s_slots[si].credit[0] = 0;
            s_slots[si].done = s_slots[si].ready = false;
        } else { s_slots[si].stamp = ++s_stamp; return; }
    }
    if (s_loading >= 0 && strcmp(hex, s_slots[s_loading].hex) == 0) return;
    for (int i = 0; i < s_qCount; ++i) {
        if (strcmp(hex, s_queue[i].hex) != 0) continue;
        if (type && type[0]) snprintf(s_queue[i].type, sizeof(s_queue[i].type), "%s", type);
        if (priority && i > 0) {
            PhotoRequest tmp = s_queue[i];
            for (int j = i; j > 0; --j) s_queue[j] = s_queue[j - 1];
            s_queue[0] = tmp;
        }
        return;
    }
    if (priority) {
        if (s_qCount < PH_QUEUE) ++s_qCount;
        for (int i = s_qCount - 1; i > 0; --i) s_queue[i] = s_queue[i - 1];
        snprintf(s_queue[0].hex, sizeof(s_queue[0].hex), "%s", hex);
        snprintf(s_queue[0].type, sizeof(s_queue[0].type), "%s", type ? type : "");
    } else if (s_qCount < PH_QUEUE) {
        snprintf(s_queue[s_qCount].hex, sizeof(s_queue[0].hex), "%s", hex);
        snprintf(s_queue[s_qCount].type, sizeof(s_queue[0].type), "%s", type ? type : "");
        ++s_qCount;
    }
}
void photo_request(const char *hex, const char *type) { queue_request(hex, type, true); }
void photo_prefetch(const char *hex, const char *type) { queue_request(hex, type, false); }
void photo_cancel_prefetches() {
    std::lock_guard<std::mutex> g(s_m);
    int keep = -1;
    for (int i = 0; i < s_qCount; ++i)
        if (s_priorityHex[0] && strcmp(s_queue[i].hex, s_priorityHex) == 0) { keep = i; break; }
    if (keep >= 0) {
        s_queue[0] = s_queue[keep];
        s_qCount = 1;
    } else s_qCount = 0;
}

void photo_reset_fallback_cache() {
    std::lock_guard<std::mutex> g(s_m);
    for (int i = 0; i < PH_SLOTS; ++i) {
        if (i == s_loading) continue;
        const bool generic = strncmp(s_slots[i].credit, "Generic ", 8) == 0;
        if (!generic && (!s_slots[i].done || s_slots[i].ready)) continue;
        s_slots[i].hex[0] = s_slots[i].type[0] = s_slots[i].credit[0] = 0;
        s_slots[i].w = s_slots[i].h = 0;
        s_slots[i].done = s_slots[i].ready = false;
        s_slots[i].failedAtMs = 0;
    }
}

bool photo_pending(char *o, size_t n, char *typeOut, size_t typeN) {
    std::lock_guard<std::mutex> g(s_m);
    if (s_loading >= 0 || s_qCount == 0) return false;
    PhotoRequest req = s_queue[0];
    for (int i = 0; i + 1 < s_qCount; ++i) s_queue[i] = s_queue[i + 1];
    --s_qCount;

    int slot = -1;
    for (int i = 0; i < PH_SLOTS; ++i) if (!s_slots[i].hex[0]) { slot = i; break; }
    if (slot < 0) {
        for (int i = 0; i < PH_SLOTS; ++i) {
            if (strcmp(s_slots[i].hex, s_priorityHex) == 0) continue;
            if (slot < 0 || s_slots[i].stamp < s_slots[slot].stamp) slot = i;
        }
        if (slot < 0) slot = 0;
    }
    snprintf(s_slots[slot].hex, sizeof(s_slots[slot].hex), "%s", req.hex);
    snprintf(s_slots[slot].type, sizeof(s_slots[slot].type), "%s", req.type);
    s_slots[slot].credit[0] = 0;
    s_slots[slot].w = s_slots[slot].h = 0;
    s_slots[slot].done = s_slots[slot].ready = false;
    s_slots[slot].stamp = ++s_stamp;
    s_loading = slot;
    snprintf(o, n, "%s", req.hex);
    if (typeOut && typeN) snprintf(typeOut, typeN, "%s", req.type);
    return true;
}

lv_color_t *photo_buffer(int *mw, int *mh) {
    std::lock_guard<std::mutex> g(s_m);
    if (mw) *mw = PH_MAXW;
    if (mh) *mh = PH_MAXH;
    return ensure_buf(s_loading);
}

lv_color_t *photo_pixels(const char *hex) {
    std::lock_guard<std::mutex> g(s_m);
    const int i = hex ? find_slot(hex) : -1;
    if (i < 0 || !s_slots[i].ready) return nullptr;
    s_slots[i].stamp = ++s_stamp;
    return s_slots[i].buf;
}

bool photo_use_cached_generic(const char *type) {
    if (!type || !type[0]) return false;
    std::lock_guard<std::mutex> g(s_m);
    if (s_loading < 0) return false;
    int src = -1;
    for (int i = 0; i < PH_SLOTS; ++i) {
        if (i == s_loading || !s_slots[i].ready) continue;
        if (strcmp(type, s_slots[i].type) == 0 &&
            strncmp(s_slots[i].credit, "Generic ", 8) == 0) { src = i; break; }
    }
    if (src < 0) return false;
    lv_color_t *dst = ensure_buf(s_loading);
    if (!dst || !s_slots[src].buf) return false;
    memcpy(dst, s_slots[src].buf,
           (size_t)s_slots[src].w * s_slots[src].h * sizeof(lv_color_t));
    snprintf(s_slots[s_loading].credit, sizeof(s_slots[s_loading].credit), "%s", s_slots[src].credit);
    s_slots[s_loading].w = s_slots[src].w;
    s_slots[s_loading].h = s_slots[src].h;
    s_slots[s_loading].ready = s_slots[s_loading].done = true;
    s_slots[s_loading].failedAtMs = 0;
    s_slots[s_loading].stamp = ++s_stamp;
    s_slots[src].stamp = ++s_stamp;
    s_loading = -1;
    return true;
}

void photo_commit(int w, int h, const char *hex, const char *credit) {
    std::lock_guard<std::mutex> g(s_m);
    int i = (s_loading >= 0 && hex && strcmp(hex, s_slots[s_loading].hex) == 0)
                ? s_loading : (hex ? find_slot(hex) : -1);
    if (i >= 0) {
        snprintf(s_slots[i].credit, sizeof(s_slots[i].credit), "%s", credit ? credit : "");
        s_slots[i].w = w; s_slots[i].h = h;
        s_slots[i].ready = (w > 0 && h > 0 && s_slots[i].buf);
        s_slots[i].done = true;
        s_slots[i].failedAtMs = s_slots[i].ready ? 0 : lv_tick_get();
        s_slots[i].stamp = ++s_stamp;
    }
    s_loading = -1;
}

bool photo_get(const char *hex, int *w, int *h, char *credit, size_t cn) {
    std::lock_guard<std::mutex> g(s_m);
    const int i = hex ? find_slot(hex) : -1;
    if (i >= 0 && s_slots[i].ready) {
        if (w) *w = s_slots[i].w;
        if (h) *h = s_slots[i].h;
        if (credit) snprintf(credit, cn, "%s", s_slots[i].credit);
        s_slots[i].stamp = ++s_stamp;
        return true;
    }
    return false;
}

bool photo_done(const char *hex) {
    std::lock_guard<std::mutex> g(s_m);
    const int i = hex ? find_slot(hex) : -1;
    return i >= 0 && s_slots[i].done;   // committed (photo or not)
}
