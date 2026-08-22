#pragma once

#include <gtk/gtk.h>
#include <nlohmann/json.hpp>
#include <string>

namespace tether::ui {

    // Conversations from the iPhone over MAP: a thread list beside the selected
    // conversation, with a composer that stays disabled until sending exists.
    GtkWidget* messages_view_new();

    // Returns true when the event belonged to this view.
    bool messages_view_handle_event(const nlohmann::json& event);

    // Selects a conversation by thread key, as the notification Reply action and
    // the --thread option do. Switching to the messages view is the caller's job.
    void messages_view_open_thread(const std::string& thread_key);

    // Threads are only pulled while someone is looking at them, so the view has
    // to be told when it comes and goes.
    void messages_view_set_visible(bool visible);

    // Drops anything that was waiting on a daemon reply. Without this a send in
    // flight when the daemon goes away leaves the composer disabled for good.
    void messages_view_handle_disconnect();

} // namespace tether::ui
