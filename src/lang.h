#pragma once
// The language the Orb's own words are in: English or German (Settings > Language).
//
// Read once at boot, before any screen builds its labels, and changed only by a restart, the
// same way a theme is: most of the text on this device is set once when a screen is built,
// so a live switch would mean every screen re-setting every label, and one forgotten label
// would leave a half-translated screen. A restart cannot forget one.
//
// The words themselves stay at the call site, both languages side by side, through tr():
//     lv_label_set_text(title, tr("Brightness", "Helligkeit"));
// so a translation can never drift away from the English it translates, and a screen added
// later shows its English rather than nothing when nobody wrote the German.
namespace lang {

enum Lang { EN = 0, DE = 1, COUNT = 2 };

void init();           // read the saved choice; call at the top of setup()
int  get();            // EN or DE
bool de();             // get() == DE, for tr()
void set(int l);       // save, then restart into it (device); the caller shows its notice first

}  // namespace lang

inline const char *tr(const char *en, const char *de) { return lang::de() ? de : en; }
