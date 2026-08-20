#pragma once

#include <gtk/gtk.h>
#include <nlohmann/json.hpp>

namespace tether::ui {

    // The device manager: discovered, remembered and connected devices beside a
    // pane for acting on the selected one.
    GtkWidget* devices_view_new();

    // Returns true when the event belonged to this view.
    bool devices_view_handle_event(const nlohmann::json& event);

    // Drops the queue of files waiting on a daemon that is no longer there.
    void devices_view_handle_disconnect();

    void devices_view_refresh();
    void devices_view_trigger_discovery();

} // namespace tether::ui
