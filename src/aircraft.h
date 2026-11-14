#pragma once
// Aircraft data model + small presentation helpers.
#if defined(ARDUINO)
  #include <Arduino.h>
#else
  // Native build (SDL simulator): minimal shim so the data model compiles off-device.
  #include <string>
  #include <cstdint>
  using String = std::string;
#endif
#include <stdint.h>

struct Aircraft {
    String   hex;            // ICAO 24-bit id (stable key)
    String   flight;         // callsign
    String   type;           // e.g. "B738" (when available)
    double   lat = 0, lon = 0;
    float    altBaro = 0;    // ft (NAN if on ground)
    bool     onGround = false;
    float    track = NAN;    // ground track deg (fallback to true_heading)
    float    gs = NAN;       // ground speed kt
    float    baroRate = NAN; // vertical rate fpm
    int      squawk = -1;
    uint32_t seenPos = 0;    // s since last position
    bool     military = false;
    uint8_t  sizeClass = 0;  // AC_CLASS_*, from the ADS-B emitter category ("A3"); 0 = not sent
    uint32_t lastUpdateMs = 0; // millis() when we last saw it (for trails/expiry)
};

// What the aircraft is, by size, for the radar's symbol. From the emitter category the
// transponder itself broadcasts (DO-260B), which adsb.lol and adsb.fi pass on as "category".
enum : uint8_t {
    AC_CLASS_NONE = 0,
    AC_CLASS_LIGHT,     // A1 light (< 15,500 lb), B1 glider, B4 ultralight, B6 drone
    AC_CLASS_SMALL,     // A2 small (15,500-75,000 lb), A6 high performance
    AC_CLASS_LARGE,     // A3 large (75,000-300,000 lb): A320, 737
    AC_CLASS_HEAVY,     // A4 B757-class, A5 heavy (> 300,000 lb): 777, A350
    AC_CLASS_HELI,      // A7 rotorcraft
};
inline uint8_t acClassFromCategory(const char *c) {
    if (!c || !c[0] || !c[1]) return AC_CLASS_NONE;
    if (c[0] == 'A') switch (c[1]) {
        case '1': return AC_CLASS_LIGHT;
        case '2': case '6': return AC_CLASS_SMALL;
        case '3': return AC_CLASS_LARGE;
        case '4': case '5': return AC_CLASS_HEAVY;
        case '7': return AC_CLASS_HELI;
    }
    if (c[0] == 'B' && (c[1] == '1' || c[1] == '4' || c[1] == '6')) return AC_CLASS_LIGHT;
    return AC_CLASS_NONE;
}

inline bool acIsEmergency(int squawk) {
    return squawk == 7500 || squawk == 7600 || squawk == 7700;
}

// Pack RGB into RGB565 for the panel.
inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

// Altitude color map (matches the mockup legend).
inline uint16_t altitudeColor565(float altFt, bool onGround) {
    if (onGround)        return rgb565(0x88, 0x88, 0x88);
    if (altFt <  3000)   return rgb565(0xFF, 0x5A, 0x3C); // red
    if (altFt < 10000)   return rgb565(0xFF, 0xB2, 0x3C); // amber
    if (altFt < 20000)   return rgb565(0xC8, 0xFF, 0x3C); // lime
    if (altFt < 30000)   return rgb565(0x39, 0xFF, 0x14); // green
    return                      rgb565(0x3C, 0xE0, 0xFF); // cyan
}
