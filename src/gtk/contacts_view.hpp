#pragma once

#include <gtk/gtk.h>
#include <nlohmann/json.hpp>
#include <string>

namespace tether::ui {

    // The iPhone's address book, pulled over PBAP.
    GtkWidget* contacts_view_new(void (*open_thread)(const std::string& thread_key));

    // Returns true when the event belonged to this view.
    bool contacts_view_handle_event(const nlohmann::json& event);

    // Contacts are only pulled while the view is on screen.
    void contacts_view_set_visible(bool visible);

} // namespace tether::ui
