#pragma once

#include <gtk/gtk.h>
#include <nlohmann/json.hpp>
#include <string>

namespace tether::ui {

    // Which of the phonebook's addresses an entry can be completed with.
    enum class ContactKind { Any, Tel };

    // Attaches a completion over the shared contacts model.
    void attach_contact_completion(GtkWidget* entry, ContactKind kind);

    // Asks the daemon for the address book.
    void contact_completion_request();

    // Fills the shared model from a bt_contacts event. Ignores every other
    // command, and does nothing when the payload has not changed.
    void contact_completion_update(const nlohmann::json& event);

    // The phonebook's name for a thread key, empty when it knows none.
    std::string contact_name_for(const std::string& thread_key);

} // namespace tether::ui
