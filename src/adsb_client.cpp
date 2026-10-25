// Fetch nearby aircraft over HTTPS from adsb.lol (fallback adsb.fi) and parse the
// readsb JSON into a vector<Aircraft>.
//
// Memory safety (important on the ESP32): we parse straight from the HTTP stream
// (no full-body String), use an ArduinoJson field filter so only the ~12 fields we
// need are kept. The nearest ADSB_MAX_AIRCRAFT contacts are retained for display.
#include "adsb_client.h"
#include "config.h"
#include "geo.h"           // haversineKm — keep the nearest N aircraft
#include "net_fetch.h"     // NetTlsPermit: this fork gives TLS its PSRAM only by permit
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>   // v7
#include <StreamUtils.h>
#include <esp_heap_caps.h>
#include <algorithm>

#if !ADSB_HTTPS_INSECURE
extern const uint8_t rootBundleStart[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t rootBundleEnd[] asm("_binary_x509_crt_bundle_end");
#endif

// Parse the JSON in PSRAM, not internal RAM. Otherwise the per-poll JSON alloc/free
// churn fragments the internal heap and, after a while, mbedTLS can't find a large
// enough contiguous block for the TLS handshake (-32512), freezing the feed.
struct PsramJsonAllocator : ArduinoJson::Allocator {
    void* allocate(size_t n) override { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
    void  deallocate(void* p) override { heap_caps_free(p); }
    void* reallocate(void* p, size_t n) override { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM); }
};
static PsramJsonAllocator s_jsonPsram;

// NetworkClient::readBytes() treats a transient negative TLS read as end-of-input,
// which makes ArduinoJson intermittently report IncompleteInput. Deliberately wrap
// the client without overriding readBytes(): Stream's timed byte reader retries
// temporary no-data reads until the configured timeout.
//
// BUFFERED as of 2026-08-22. ArduinoJson pulls its input one byte at a time, so parsing a
// ~40 KB aircraft response meant ~40,000 separate single-byte reads straight into the
// socket, every poll, forever. Each of those goes through lwIP, whose buffers live in the
// internal RAM this feed is starved for — a far better explanation for the measured
// fragmentation than the parsed document itself, which is already allocated in PSRAM (see
// PsramJsonAllocator above, and note it was added for exactly this class of problem).
//
// The buffer refills in 1 KB chunks and ArduinoJson is served from it. Same bytes, same
// order, same reliability contract; roughly a thousandth of the socket calls.
class ReliableJsonStream : public Stream {
public:
    explicit ReliableJsonStream(Stream& source) : _source(source) {}
    int available() override { return (int)(_len - _pos) + _source.available(); }
    int read() override {
        if (_pos >= _len && !refill()) return -1;
        ++_bytesRead;
        return _buf[_pos++];
    }
    // peek() must not consume, but it may legitimately need to pull the next chunk in to
    // answer at all — the parser peeks across a buffer boundary like any other position.
    int peek() override {
        if (_pos >= _len && !refill()) return -1;
        return _buf[_pos];
    }
    void flush() override { _source.flush(); }
    size_t write(uint8_t) override { return 0; }
    size_t bytesRead() const { return _bytesRead; }

private:
    // Keeps the original contract: wait for bytes rather than treating a momentary empty
    // socket as end-of-input, which is the bug the single-byte version existed to dodge.
    // Returns false only on a real timeout or a closed connection with nothing left.
    bool refill() {
        _pos = _len = 0;
        const uint32_t started = millis();
        for (;;) {
            const int avail = _source.available();
            if (avail > 0) {
                const size_t want = avail < (int)sizeof(_buf) ? (size_t)avail : sizeof(_buf);
                const int got = _source.readBytes(_buf, want);
                if (got > 0) { _len = (size_t)got; return true; }
            }
            // Nothing right now. Give up only on the same timeout the single-byte reader
            // used. Deliberately NOT down-casting _source to ask whether the peer is still
            // connected: this holds a Stream&, and quietly assuming it is really a
            // NetworkClient would be undefined behaviour the moment anything else is passed
            // in. The timeout is the honest, type-safe stopping condition, and it is exactly
            // what the previous implementation relied on.
            if (millis() - started > 8000) return false;
            delay(1);
        }
    }

    uint8_t _buf[1024];
    size_t  _pos = 0;
    size_t  _len = 0;
    Stream& _source;
    size_t _bytesRead = 0;
};

void AdsbClient::begin(double homeLat, double homeLon, float rangeKm) {
    _lat = homeLat; _lon = homeLon; _rangeKm = rangeKm;
}

// One attempt per provider per poll. Cooldowns apply to the whole provider,
// including HTTP refusals: do not rotate IP addresses to evade a rate limit.
namespace {
constexpr uint32_t PROVIDER_RETRY_MS = 60000;
constexpr uint32_t REFUSED_RETRY_MS = 300000;
constexpr uint32_t MIN_REQUEST_MS = 1100; // adsb.fi permits at most 1 request/sec/IP
bool ready(uint32_t until) {
    return until == 0 || (int32_t)(millis() - until) >= 0;
}
}

bool AdsbClient::poll(std::vector<Aircraft>& out) {
    if (WiFi.status() != WL_CONNECTED) return false;
    _refused = false;
    _lastStatus = 0;
    _refusedStatus = 0;
    for (uint8_t provider = 0; provider < 2; ++provider) {
        if (!ready(_nextTryMs[provider])) continue;
        // Set the minimum spacing even on success. Usually the normal poll
        // interval is longer, but range changes can request an immediate poll.
        _nextTryMs[provider] = millis() + MIN_REQUEST_MS;
        if (fetchFrom(provider, out)) {
            _refused = false;
            _refusedStatus = 0;
            _lastStatus = 200;
            return true;
        }
        const bool refused = _lastStatus >= 400 && _lastStatus < 500;
        _nextTryMs[provider] = millis() + (refused ? REFUSED_RETRY_MS : PROVIDER_RETRY_MS);
        _providerStatus[provider] = _lastStatus;
    }
    // Preserve a server refusal while that provider is cooling down, so the
    // caller keeps its refusal backoff instead of treating a skipped request
    // as a TLS/heap failure. A successful fallback above clears it immediately.
    for (uint8_t provider = 0; provider < 2; ++provider) {
        const int status = _providerStatus[provider];
        if (!ready(_nextTryMs[provider]) && status >= 400 && status < 500) {
            _refused = true;
            _refusedStatus = status;
        }
    }
    return false;
}

bool AdsbClient::fetchFrom(uint8_t provider, std::vector<Aircraft>& out) {
    const char *host = provider == 0 ? ADSB_PRIMARY_HOST : ADSB_FALLBACK_HOST;
    const double nm = std::min(250.0, std::max(1.0, _rangeKm * 0.539957));
    char url[192];
    if (provider == 0)
        snprintf(url, sizeof(url), "https://%s/v2/point/%.4f/%.4f/%.0f", host, _lat, _lon, nm);
    else
        snprintf(url, sizeof(url), "https://%s/api/v3/lat/%.4f/lon/%.4f/dist/%.0f", host, _lat, _lon, nm);

    // Keep only one TLS connection alive at a time, and release its PSRAM before
    // the weather/photo clients run on this same task. No IP pinning: connecting
    // by hostname also supplies the correct TLS SNI name.
    //
    // This fork: mbedTLS gets PSRAM only while a caller holds a permit (net_fetch.h), so the
    // permit is taken before the client exists and released after it is gone. Declared
    // first so it is destroyed last.
    NetTlsPermit tls;
    WiFiClientSecure client;
#if ADSB_HTTPS_INSECURE
    client.setInsecure(); // preserves existing public-data certificate policy
#else
    client.setCACertBundle(rootBundleStart, (size_t)(rootBundleEnd - rootBundleStart));
#endif
    client.setHandshakeTimeout(10);
    HTTPClient http;
    // adsb.lol rejects HTTP/1.0 with 426 Upgrade Required. HTTP/1.1 may
    // return chunks, which are decoded below before ArduinoJson sees them.
    http.useHTTP10(false);
    http.setReuse(false);
    http.setConnectTimeout(ADSB_CONNECT_MS);
    http.setTimeout(ADSB_READ_MS);
    if (!http.begin(client, url)) { _lastStatus = -1; return false; }
    http.setUserAgent(ADSB_USER_AGENT);
    http.addHeader("Accept", "application/json");
    http.setAcceptEncoding("identity");
    const char *headers[] = {"Transfer-Encoding"};
    http.collectHeaders(headers, 1);
    const int code = http.GET();
    _lastStatus = code;
    _providerStatus[provider] = code;
    Serial.printf("[adsb] HTTPS %s -> %d internal_largest=%u psram_free=%u psram_largest=%u\n",
                  host, code,
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    if (code != 200) {
        if (code < 0) {
            char error[128] = {};
            const int tlsError = client.lastError(error, sizeof(error));
            Serial.printf("[adsb] TLS error %d: %s\n", tlsError, error);
        }
        http.end();
        client.stop();
        return false;
    }

    // Only keep the fields we use -> much smaller parsed document.
    JsonDocument filter(&s_jsonPsram);
    const char* keys[] = { "ac", "aircraft" };
    const char* flds[] = { "hex", "flight", "t", "lat", "lon", "alt_baro",
                           "track", "true_heading", "gs", "baro_rate",
                           "squawk", "seen_pos", "dbFlags" };
    for (const char* k : keys)
        for (const char* f : flds)
            filter[k][0][f] = true;

    JsonDocument doc(&s_jsonPsram);
    ReliableJsonStream jsonStream(http.getStream());
    ChunkDecodingStream decoded(jsonStream);
    Stream &body = http.header("Transfer-Encoding").equalsIgnoreCase("chunked")
        ? static_cast<Stream&>(decoded) : static_cast<Stream&>(jsonStream);
    DeserializationError err = deserializeJson(doc, body,
                                               DeserializationOption::Filter(filter));
    // Release handshake/session buffers before building the display vector.
    http.end();
    client.stop();
    if (err) {
        Serial.printf("[adsb] %s parse failed: %s; read=%u\n",
                      host, err.c_str(), (unsigned)jsonStream.bytesRead());
        _lastStatus = -2;
        return false;
    }

    JsonArrayConst arr = doc["ac"].as<JsonArrayConst>();
    if (arr.isNull()) arr = doc["aircraft"].as<JsonArrayConst>();
    if (arr.isNull()) { _lastStatus = -2; return false; }

    // Keep the ADSB_MAX_AIRCRAFT *nearest* aircraft (not just the first ones the feed happens to
    // list), so busy areas still show the traffic closest to you. We gate by distance BEFORE
    // parsing the strings, so the hundreds of far-away aircraft never allocate anything.
    std::vector<Aircraft> tmp;
    std::vector<float>     dist;             // parallel array: km from home for each kept aircraft
    tmp.reserve(ADSB_MAX_AIRCRAFT);
    dist.reserve(ADSB_MAX_AIRCRAFT);
    const uint32_t now = millis();
    for (JsonObjectConst a : arr) {
        if (a["lat"].isNull() || a["lon"].isNull()) continue;   // need a position
        const double lat = a["lat"].as<double>();
        const double lon = a["lon"].as<double>();

        // alt_baro is the string "ground" for aircraft on the ground; skip them if hide-ground is on.
        const bool  onGround = a["alt_baro"].is<const char*>();
        const float altFt    = onGround ? 0.0f : (a["alt_baro"] | 0.0f);
        if (_hideGround && onGround) continue;
        // optional filters (applied before the cap, so slots only go to matching aircraft)
        if (_minAltFt > 0.0f && (onGround || altFt < _minAltFt)) continue;
        if (_milOnly && (((a["dbFlags"] | 0u) & 0x1) == 0)) continue;

        const float d = (float)geo::haversineKm(_lat, _lon, lat, lon);

        // Center dead zone: traffic this close projects into the middle of the
        // scope, underneath whatever the theme puts there (a hub, a gear, a hand
        // pivot), where a cluster of blips just reads as clutter. Dropped here,
        // before the nearest-N gate below, so the slots go to aircraft that will
        // actually be visible. Set from the design's own Scope card (0 = off).
        if (_minDistKm > 0.0f && d < _minDistKm) continue;

        // nearest-N gate: if the buffer is full and this one isn't closer than the farthest kept,
        // drop it now — before any string allocation.
        int farIdx = -1;
        if ((int)tmp.size() >= ADSB_MAX_AIRCRAFT) {
            farIdx = 0;
            for (int i = 1; i < (int)dist.size(); ++i) if (dist[i] > dist[farIdx]) farIdx = i;
            if (d >= dist[farIdx]) continue;
        }

        Aircraft ac;
        ac.hex = (const char*)(a["hex"] | "");
        if (ac.hex.length() == 0) continue;
        ac.flight = String((const char*)(a["flight"] | "")); ac.flight.trim();
        ac.type   = (const char*)(a["t"] | "");
        ac.lat = lat; ac.lon = lon;
        ac.onGround = onGround;
        ac.altBaro  = altFt;
        ac.track    = a["track"].is<float>() ? a["track"].as<float>() : (a["true_heading"] | NAN);
        ac.gs       = a["gs"] | NAN;
        ac.baroRate = a["baro_rate"] | NAN;
        ac.squawk   = a["squawk"].is<const char*>() ? atoi(a["squawk"]) : (a["squawk"] | -1);
        ac.seenPos  = a["seen_pos"] | 0;
        ac.military = ((a["dbFlags"] | 0u) & 0x1) != 0;
        ac.lastUpdateMs = now;

        if (farIdx >= 0) { tmp[farIdx] = std::move(ac); dist[farIdx] = d; }   // replace the farthest kept
        else             { tmp.push_back(std::move(ac)); dist.push_back(d); }
    }

    out.swap(tmp);
    _lastOkMs = now;
    Serial.printf("[adsb] source=%s aircraft=%u\n", host, (unsigned)out.size());
    return true;
}
