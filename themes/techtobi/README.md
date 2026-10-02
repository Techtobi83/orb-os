# Tagesring theme (folder: techtobi)

By TechTobi. An Orb theme folder, ready to copy to the SD card as `/themes/techtobi/`. Built by hand rather
than in Orb Studio, because it uses firmware features Studio has no controls for yet:

- `clock_style.json`: `"dayRing"`, `"dateDE"` and `"swing1"` (firmware 2.22.3 or later).
- `weather_style.json`: a `{city}` text slot.

A Studio push of another theme leaves this folder alone. Re-pushing *this* theme from Studio
is not possible, since Studio never had it.

## Where the files come from

| Files | Source |
|---|---|
| `clock_plate.png`, `clock_hand_*.png`, `clock_shadow_*.png`, `clock_static1.png` | drawn by `tools/theme_techtobi.py` (the "Tagesring-Chronometer") |
| `splash.png` | drawn by `tools/theme_techtobi.py`: the owner's IRON ORBIT logo (`assets/iron_orbit_logo.png`) rebuilt clean on the chronometer dial; its letters are cut from the artwork into `assets/iron_orbit_letters.png` |
| `settings_plate.png`, `menu_plate.png` | drawn by `tools/theme_techtobi.py`: the dial without the clock's furniture |
| `font_settings.bin`, `font_settings_sel.bin`, `font_menu_current.bin` | Sora (OFL, see NOTICE), written by `tools/gen_lv_font.py` in LVGL's binary format |
| radar art, `chime.pcm`, `font_radar*.bin` | the Cold War 1983 theme, Zion Brock, CC0 1.0 |
| `font_weather1.bin` | a copy of Cold War's `font_radar_loc.bin`, so the weather map's town matches the Flight Tracker's |
| `radar_blip.png` | the Modern theme, Zion Brock, CC0 1.0, tinted green by `blipImageTint` |

## Installing

Copy every file to `/themes/techtobi/` on the card, `theme.json` and `_installed` last: the
firmware lists a theme only once `_installed` exists. Then choose it under Settings > Theme,
or send `?orb theme techtobi` over USB.

After changing any picture, recompute `assetsHash` in `theme.json` (FNV-1a over the bytes of
every file in `assets`, in that order). The Orb keys its baked copy of the art on that number,
and an unchanged number means it keeps showing the old pictures.
