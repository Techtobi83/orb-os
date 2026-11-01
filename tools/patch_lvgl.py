# PlatformIO pre-build script: corrects one line of LVGL 8.4.0, in every environment.
#
# get_glyph_dsc_id() in lv_font_fmt_txt.c tests a character against each range with
#     if(rcp > fdsc->cmaps[i].range_length) continue;
# which also accepts the first character PAST the range. Next to another range that draws the
# wrong letter. After the font's last range it reads beyond the glyph table and takes whatever
# is in memory for a character's size and bitmap. The English date window hit exactly that:
# font_sora_26 had no T, so TU and TH overran the table, and only on those two days did the clock
# show garbage (found by jbat66, 2026-10-30). Every font in the firmware has such a character,
# and so does every font a theme brings, so this is corrected where they all pass through.
#
# The script refuses to build if the line is neither the faulty one nor the corrected one. A
# different LVGL must be looked at by a person, not silently left unprotected.
Import("env")
import os

OLD = "if(rcp > fdsc->cmaps[i].range_length) continue;"
NEW = "if(rcp >= fdsc->cmaps[i].range_length) continue;"

path = os.path.join(env.subst("$PROJECT_LIBDEPS_DIR"), env.subst("$PIOENV"),
                    "lvgl", "src", "font", "lv_font_fmt_txt.c")
if not os.path.isfile(path):
    print("patch_lvgl: %s not found" % path)
    env.Exit(1)
with open(path, encoding="utf-8", newline="") as f:
    src = f.read()
if src.count(NEW) == 1 and OLD not in src:
    pass                                    # corrected by an earlier build
elif src.count(OLD) == 1:
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(src.replace(OLD, NEW))
    print("patch_lvgl: corrected the range check in lv_font_fmt_txt.c")
else:
    print("patch_lvgl: the range check in lv_font_fmt_txt.c is not as expected; look at this LVGL")
    env.Exit(1)
