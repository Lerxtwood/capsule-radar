#pragma once
// Shared route state (origin -> destination by aircraft identity). Portable:
// the UI thread requests a lookup, a network task fulfils it, the UI reads the
// result. Prefer hex + callsign together so reused callsigns don't collide.
#include <stddef.h>

void route_request(const char *hex, const char *callsign, float lat, float lon, float track);    // UI: want a route for this aircraft
void route_prefetch(const char *hex, const char *callsign, float lat, float lon, float track);   // background: queue behind UI requests
void route_cancel_prefetches();                               // discard queued background requests
bool route_pending(char *hexOut, size_t hn, char *callOut, size_t cn, float *latOut, float *lonOut, float *trackOut);      // task: is a lookup needed?
void route_store(const char *hex, const char *callsign,
                 const char *line1, const char *line2);      // task/sim: store formatted two-line result
bool route_get(const char *hex, const char *callsign,
               char *line1, size_t l1n, char *line2, size_t l2n);  // UI: read result
