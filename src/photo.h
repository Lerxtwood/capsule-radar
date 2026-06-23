#pragma once
// Shared aircraft-photo state (portable). The actual fetch+JPEG decode lives in
// photo_client (device-only); this just holds the decoded RGB565 image + request
// handshake, guarded for cross-thread access (like route.*).
#include <lvgl.h>
#include <stddef.h>

void photo_request(const char *hex, const char *type = nullptr);  // UI: exact photo, then optional type fallback
void photo_prefetch(const char *hex, const char *type = nullptr); // background: queue behind UI requests
void photo_cancel_prefetches();                      // discard queued background requests
void photo_reset_fallback_cache();                   // clear generic and negative cached results
bool photo_pending(char *hexOut, size_t n, char *typeOut = nullptr, size_t typeN = 0);
lv_color_t *photo_buffer(int *maxW, int *maxH);      // PSRAM RGB565 buffer to decode into
lv_color_t *photo_pixels(const char *hex);           // decoded pixels for a cached aircraft
bool photo_use_cached_generic(const char *type);     // copy a cached type image into the active request
void photo_commit(int w, int h, const char *hex, const char *credit);  // ready (w=0 => none)
bool photo_get(const char *hex, int *w, int *h, char *credit, size_t cn);  // ready & matches?
bool photo_done(const char *hex);                    // fetch finished for this hex (with or without a photo)
