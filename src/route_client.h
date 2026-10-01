#pragma once
// Look up a flight's origin/destination by callsign via adsbdb.com (free, no key).
// Device-only (uses WiFi/HTTPS). City names are returned in English.
#include <stddef.h>

// 1 = route found, 0 = the service answered and knows no route for this callsign (final),
// -1 = the lookup itself failed (no WiFi, timeout, HTTP error, bad reply): worth asking again.
enum RouteResult { ROUTE_FAILED = -1, ROUTE_UNKNOWN = 0, ROUTE_FOUND = 1 };
RouteResult route_fetch(const char *callsign, char *from, size_t fn, char *to, size_t tn);

// NVS route cache (avoids re-querying adsbdb for the same flight across reboots).
void route_cache_begin();   // call once at boot; clears the cache if the label format changed
bool route_cache_get(const char *callsign, char *from, size_t fn, char *to, size_t tn);
void route_cache_put(const char *callsign, const char *from, const char *to);
