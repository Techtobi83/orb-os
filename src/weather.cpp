#include "weather.h"
#include "lang.h"
#include <mutex>
#include <string.h>
#include <stdio.h>

static std::mutex s_mutex;
static WeatherSnapshot s_snapshot = {};

void weather_store(const WeatherSnapshot &snapshot) {
    std::lock_guard<std::mutex> lock(s_mutex);
    s_snapshot = snapshot;
}

bool weather_get(WeatherSnapshot &snapshot) {
    std::lock_guard<std::mutex> lock(s_mutex);
    snapshot = s_snapshot;
    return snapshot.valid;
}

const char *weather_condition(int code) {
    if (code == 0) return tr("Clear", "Klar");
    if (code <= 2) return tr("Partly cloudy", "Teils bewölkt");
    if (code == 3) return tr("Overcast", "Bedeckt");
    if (code == 45 || code == 48) return tr("Fog", "Nebel");
    if (code >= 51 && code <= 57) return tr("Drizzle", "Niesel");
    if (code >= 61 && code <= 67) return tr("Rain", "Regen");
    if (code >= 71 && code <= 77) return tr("Snow", "Schnee");
    if (code >= 80 && code <= 82) return tr("Showers", "Schauer");
    if (code >= 85 && code <= 86) return tr("Snow showers", "Schneeschauer");
    if (code >= 95) return tr("Thunderstorm", "Gewitter");
    return tr("Unknown", "Unbekannt");
}

const char *weather_day_name(const char *isoDate) {
    static const char *names_en[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *names_de[] = {"So", "Mo", "Di", "Mi", "Do", "Fr", "Sa"};
    const char *const *names = lang::de() ? names_de : names_en;
    if (!isoDate || strlen(isoDate) < 10) return "---";
    int y = 0, m = 0, d = 0;
    if (sscanf(isoDate, "%d-%d-%d", &y, &m, &d) != 3) return "---";
    if (m < 3) { m += 12; --y; }
    const int k = y % 100, j = y / 100;
    const int h = (d + (13 * (m + 1)) / 5 + k + k / 4 + j / 4 + 5 * j) % 7;
    return names[(h + 6) % 7];
}
