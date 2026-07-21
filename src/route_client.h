#pragma once
// Look up a flight's origin/destination from FlightAware's public flight page
// first, with ADS-B DB as fallback. Device-only (uses WiFi/HTTPS).
#include <stddef.h>

bool route_fetch(const char *hex, const char *callsign, float targetLat, float targetLon, float targetTrack,
                 char *line1, size_t l1n, char *line2, size_t l2n);
const char *route_fetch_last_mode();
const char *route_fetch_last_url();
int route_fetch_last_status();

// NVS route cache (avoids re-querying adsbdb for the same flight across reboots).
void route_cache_begin();   // call once at boot; clears the cache if the label format changed
bool route_cache_get(const char *hex, const char *callsign,
                     char *line1, size_t l1n, char *line2, size_t l2n);
void route_cache_put(const char *hex, const char *callsign,
                     const char *line1, const char *line2);
