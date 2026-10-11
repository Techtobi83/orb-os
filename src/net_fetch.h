#pragma once
// HTTPS GET into a PSRAM buffer. When the server sends Content-Length the body is
// streamed straight into PSRAM, so large binaries (weather tiles, satellite JPEGs)
// never sit on the scarce internal heap — the contiguous block mbedTLS needs for the
// live-feed TLS handshake lives there, and a 100+ KB String can starve it.
// Chunked responses fall back to getString() (small payloads only reach that path).
// On success the caller owns *out and must release it with heap_caps_free().
#include <stddef.h>
#include <stdint.h>

bool net_fetch_psram(const char *url, const char *userAgent,
                     uint8_t **out, size_t *outLen, size_t maxLen,
                     int connectTimeoutMs, int totalTimeoutMs);

// TLS is permitted, not assumed. Call once at boot (setup) to put mbedTLS's allocator in
// place; from then on a handshake can only get its memory while a caller holds a permit.
//
// Why a permit and not simply "TLS works now": giving mbedTLS PSRAM made every https:// on
// the board start succeeding (2.33.0), the flight photos included, which until then had
// always failed quietly. Their sessions then cost internal RAM the network stack needs, and in
// a 35-minute soak the weather radar's plain-HTTP requests went unanswered 7 times in 8,
// against 1 in 10 before (2026-10-11). So only a caller that asks for TLS gets it; everyone
// else sees exactly the TLS this board always had, which is none.
void net_tls_install();
void net_tls_permit(bool on);          // nests; pair every true with a false

// Holds a permit for its scope.
struct NetTlsPermit {
    NetTlsPermit()  { net_tls_permit(true); }
    ~NetTlsPermit() { net_tls_permit(false); }
};
