#pragma once

#include <nlohmann/json.hpp>

namespace tether::ui {

    // Presents the settings window, building it on first use.
    void settings_window_show();

    // Applies a daemon bt_status broadcast to the controls. Never consumes the
    // event, views downstream still need to see it.
    void settings_handle_event(const nlohmann::json& event);

} // namespace tether::ui
