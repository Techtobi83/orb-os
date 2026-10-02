#include "lang.h"

#ifdef ARDUINO
#include <Arduino.h>
#include <Preferences.h>
#endif

namespace {
int s_lang = lang::EN;
}

namespace lang {

void init() {
#ifdef ARDUINO
    Preferences p;
    p.begin("capsuleradar", true);
    const int l = p.getInt("lang", EN);
    p.end();
    s_lang = (l == DE) ? DE : EN;
    Serial.printf("[lang] %s\n", s_lang == DE ? "Deutsch" : "English");
#endif
}

int  get() { return s_lang; }
bool de()  { return s_lang == DE; }

void set(int l) {
    l = (l == DE) ? DE : EN;
#ifdef ARDUINO
    Preferences p;
    p.begin("capsuleradar", false);
    p.putInt("lang", l);
    p.end();
    delay(700);       // hold the notice on screen long enough to read, as a theme switch does
    ESP.restart();
#else
    s_lang = l;       // the simulator has no restart; the next screen built picks it up
#endif
}

}  // namespace lang
