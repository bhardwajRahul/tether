#pragma once

#include <nlohmann/json.hpp>

namespace tether::ui {

    // ~/.config/tether/gtk.json, read once on first use. Unreadable or corrupt means defaults.
    nlohmann::json& prefs();

    void prefs_save();

} // namespace tether::ui
