#pragma once

#include <functional>
#include <nlohmann/json.hpp>

namespace tether::ui {

    using DaemonEventFn = std::function<void(const nlohmann::json&)>;

    // Connects to unix socket, subscribes to the local event feed, and dispatches each nd event. Starts the daemon once
    // if it is not running, and reconnects on its own after it goes away.
    void daemon_client_start(DaemonEventFn on_event);
    void daemon_client_stop();

    // Returns false when the command could not be handed to the daemon, so a
    // caller that latches UI state on having sent something can tell.
    bool daemon_send(const nlohmann::json& message);
    bool daemon_connected();

    // Called after the event feed goes away, so views can drop any state that
    // was waiting on a reply that is never going to arrive.
    using DaemonDisconnectFn = std::function<void()>;
    void daemon_client_on_disconnect(DaemonDisconnectFn on_disconnect);

} // namespace tether::ui
