// Button icons for the player's controller (settings: controls.glyphs = auto, switch, xbox or playstation).
//
// ACNH draws every button prompt (dialogs, menus, the key guide) as private-use characters of ParkExt.bfttf, its
// own extension font in Font/ScalableFont.sarc.zs, which every combined font in Font/Fcpx.sarc.zs lists first; the
// shared system fonts (hle_pl.cpp) play no part in them. tools/make_button_glyphs.py writes a copy of that archive
// with the icons redrawn for each style and face-button layout (input.layout, which moves the game's A to the
// bottom button), under <data root>/fonts/glyphs_<style>/<layout>/romfs/. This picks the copy to serve.
//
// The game opens the archive once, while booting, so a change of style or layout shows at the next start; "auto"
// asks which kind of controller is connected at that moment (none: the original icons).
//
// Entry point, for resolve() in hle_fs.cpp on the RomFS mount (path relative to the RomFS, '/' separated):
//   std::string glyph_override(const std::string& romfs_path);  // host path to serve instead, or "" for the RomFS
#include <filesystem>
#include <mutex>
#include <string>

#include <nx/runtime.h>

#include "config.h"
#include "platform.h"

namespace fs = std::filesystem;

namespace nx {

std::string glyph_override(const std::string& romfs_path) {
    if (romfs_path != "Font/ScalableFont.sarc.zs") return {};
    std::string style = config_get("controls.glyphs", "auto");
    if (style == "auto") {
        style = plat::controller_kind();
        // Pads SDL cannot name mostly follow the Xbox layout and labels (XInput); no pad: the keyboard, original icons.
        if (style == "generic") style = "xbox";
    }
    if (style != "xbox" && style != "playstation" && style != "switch") style = "switch";
    std::string layout = config_get("input.layout", "console") == "xbox" ? "xbox" : "console";

    static std::mutex lock;
    static std::string logged;  // log each choice once: the game may open the archive more than once
    std::lock_guard lk(lock);
    std::string choice = style + "/" + layout, path;
    if (style != "switch" || layout != "console") {  // switch on the Switch layout is the RomFS's own archive
        fs::path p = fs::u8path(plat::data_root()) / "fonts" / ("glyphs_" + style) / layout / "romfs" /
                     fs::u8path(romfs_path);
        std::error_code ec;
        if (layout == "console" && !fs::is_regular_file(p, ec)) {  // made before the layout was named "console"
            fs::path old = fs::u8path(plat::data_root()) / "fonts" / ("glyphs_" + style) / "nintendo" / "romfs" /
                           fs::u8path(romfs_path);
            if (fs::is_regular_file(old, ec)) p = old;
        }
        if (fs::is_regular_file(p, ec)) {
            auto s = p.u8string();
            path.assign(s.begin(), s.end());
        } else if (logged != choice) {
            log("glyphs: no %s icons (%s missing: run tools/make_button_glyphs.py); the original ones are shown",
                choice.c_str(), p.string().c_str());
        }
    }
    if (logged != choice && !path.empty()) log("glyphs: %s button icons <- %s", choice.c_str(), path.c_str());
    logged = choice;
    return path;
}

}  // namespace nx
