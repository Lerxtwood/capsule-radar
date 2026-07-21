#pragma once
// Look up a flight's origin/destination via adsbdb.com (free, no key).
// Device-only (uses WiFi/HTTPS). City names are returned in English.
#include <stddef.h>

bool route_fetch(const char *hex, const char *callsign, float targetLat, float targetLon, float targetTrack,
                 char *from, size_t fn, char *to, size_t tn);
const char *route_fetch_last_mode();
const char *route_fetch_last_url();
int route_fetch_last_status();

// NVS route cache (avoids re-querying adsbdb for the same flight across reboots).
void route_cache_begin();   // call once at boot; clears the cache if the label format changed
bool route_cache_get(const char *hex, const char *callsign,
                     char *from, size_t fn, char *to, size_t tn);
void route_cache_put(const char *hex, const char *callsign,
                     const char *from, const char *to);
