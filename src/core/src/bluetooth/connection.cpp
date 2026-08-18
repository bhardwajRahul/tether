#include "tether/bluetooth/connection.hpp"
#include "tether/bluetooth/ancs/client.hpp"
#include "tether/bluetooth/config.hpp"
#include "tether/bluetooth/contacts.hpp"
#include "tether/bluetooth/groups.hpp"
#include "tether/bluetooth/journal.hpp"
#include "tether/bluetooth/map_session.hpp"
#include "tether/bluetooth/monitor.hpp"
#include "tether/bluetooth/pbap_session.hpp"
#include "tether/log.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <gio/gio.h>
#include <mutex>
#include <thread>

namespace tether::bluetooth {

    ConnectionManager* g_bt_connections = nullptr;

    namespace {
        MessageStore g_messages;
        std::mutex g_messages_mutex;
        MessageJournal g_journal;
        std::mutex g_map_mutex;
        std::shared_ptr<MapSession> g_map_session;

        std::mutex g_group_mutex;
        GroupCorrelator g_correlator;
        GroupRoster g_rosters;
        std::map<std::string, GroupInfo> g_group_info;
        std::atomic<bool> g_group_replies_enabled{false};

        // MAP folders are navigated from the session root one level at a time.
        // The session parks on the message root and lists its children, so both
        // halves of a conversation can be pulled without walking between them.
        constexpr const char* MAP_MSG_ROOT = "telecom/msg";
        constexpr const char* MAP_OUTBOX = "telecom/msg/outbox";
        constexpr const char* MAP_FOLDER_INBOX = "inbox";
        constexpr const char* MAP_FOLDER_SENT = "sent";
    } // namespace

    MessageStore& message_store() { return g_messages; }
    std::mutex& message_store_mutex() { return g_messages_mutex; }

    bool send_message(const std::string& thread_key, const std::string& body, Message& sent_out, std::string& err_out) {
        if (body.empty()) {
            err_out = "Nothing to send.";
            return false;
        }

        std::vector<Recipient> recipients;
        if (thread_key.rfind("group:", 0) == 0) {
            GroupInfo info;
            GroupRoster rosters;
            {
                std::lock_guard<std::mutex> lock(g_group_mutex);
                auto found = g_group_info.find(thread_key);
                if (found != g_group_info.end())
                    info = found->second;
                rosters = g_rosters;
            }
            ReplyEligibility eligibility;
            {
                std::lock_guard<std::mutex> lock(g_messages_mutex);
                eligibility = resolve_group_recipients(
                    info, thread_key, contact_store(), rosters, g_group_replies_enabled, recipients, err_out);
            }
            if (eligibility != ReplyEligibility::Allowed)
                return false;
        } else {
            Recipient recipient;
            if (!recipient_from_thread_key(thread_key, recipient, err_out))
                return false;
            recipients.push_back(recipient);
        }

        std::shared_ptr<MapSession> session;
        {
            std::lock_guard<std::mutex> lock(g_map_mutex);
            session = g_map_session;
        }
        if (!session) {
            err_out = "Messages are not connected.";
            return false;
        }

        const std::string bmessage = build_bmessage(recipients, body);
        if (bmessage.empty()) {
            err_out = "Could not build a message for this conversation.";
            return false;
        }

        if (!session->push_message(MAP_OUTBOX, bmessage, err_out))
            return false;

        Message sent;
        sent.thread_key = thread_key;
        sent.peer_address = recipients.size() == 1 ? recipients.front().address : std::string{};
        sent.body = body;
        sent.timestamp = static_cast<int64_t>(std::time(nullptr));
        sent.outgoing = true;
        sent.read = true;
        sent.folder = "outbox";

        {
            std::lock_guard<std::mutex> lock(g_messages_mutex);
            sent.handle =
                LOCAL_HANDLE_PREFIX + std::to_string(std::time(nullptr)) + "-" + std::to_string(g_messages.size());
            g_messages.add(sent);
            g_journal.append(sent);
        }
        sent_out = sent;
        return true;
    }

    void observe_message_notification(const std::string& sender,
                                      const std::string& subtitle,
                                      const std::string& body,
                                      int64_t now) {
        std::lock_guard<std::mutex> lock(g_group_mutex);
        g_correlator.observe({sender, subtitle, body, now});
        g_correlator.expire(now);
    }

    ReplyEligibility group_reply_status(const std::string& thread_key, std::string& reason) {
        GroupInfo info;
        GroupRoster rosters;
        {
            std::lock_guard<std::mutex> lock(g_group_mutex);
            auto found = g_group_info.find(thread_key);
            if (found != g_group_info.end())
                info = found->second;
            rosters = g_rosters;
        }
        std::vector<Recipient> recipients;
        std::lock_guard<std::mutex> lock(g_messages_mutex);
        return resolve_group_recipients(
            info, thread_key, contact_store(), rosters, g_group_replies_enabled, recipients, reason);
    }

    void set_group_replies_enabled(bool enabled) {
        std::lock_guard<std::mutex> lock(g_group_mutex);
        g_group_replies_enabled = enabled;
    }

    void reload_group_rosters() {
        std::lock_guard<std::mutex> lock(g_group_mutex);
        g_rosters = load_rosters();
    }

    bool mark_message_read(const std::string& handle, bool read, std::string& err_out, bool* synced_out) {
        if (synced_out)
            *synced_out = false;

        // The stored path is what obexd currently answers on. A message replayed
        // from the journal has none, and one that has scrolled out of the listing
        // window never gets another. The desktop's own read state must still move
        // for those, or the unread badge can never reach zero.
        std::string object_path;
        bool known = false;
        {
            std::lock_guard<std::mutex> lock(g_messages_mutex);
            if (const Message* message = g_messages.find(handle)) {
                known = true;
                object_path = message->object_path;
            }
        }
        if (!known) {
            err_out = "That message is no longer in the conversation history.";
            return false;
        }

        std::shared_ptr<MapSession> session;
        {
            std::lock_guard<std::mutex> lock(g_map_mutex);
            session = g_map_session;
        }

        if (session && !object_path.empty()) {
            std::string sync_err;
            if (session->set_read(object_path, read, sync_err)) {
                if (synced_out)
                    *synced_out = true;
            } else {
                err_out = sync_err;
            }
        } else {
            err_out = "The iPhone is not serving this message right now; marked read on this computer only.";
        }

        std::lock_guard<std::mutex> lock(g_messages_mutex);
        if (g_messages.set_read(handle, read)) {
            // Re-append so the state survives a restart. Retention keeps the last
            // line for a handle, so the newer read state wins on the next load.
            if (const Message* updated = g_messages.find(handle))
                g_journal.append(*updated);
        }
        return true;
    }

    namespace {

        constexpr const char* BLUEZ_NAME = "org.bluez";
        constexpr const char* IFACE_DEVICE = "org.bluez.Device1";
        constexpr const char* IFACE_BEARER_LE = "org.bluez.Bearer.LE1";
        constexpr const char* IFACE_BEARER_BREDR = "org.bluez.Bearer.BREDR1";
        constexpr const char* IFACE_PROPS = "org.freedesktop.DBus.Properties";

        constexpr const char* OBEX_NAME = "org.bluez.obex";
        constexpr const char* OBEX_ROOT = "/org/bluez/obex";
        constexpr const char* OBEX_CLIENT = "org.bluez.obex.Client1";

        constexpr int CONNECT_TIMEOUT_MS = 30000;

        constexpr int MESSAGE_POLL_SECONDS = 15;
        // obexd can drop a MAP session while the Classic link stays up, and the
        // profile supervisor short-circuits once map_open is set, so nothing else
        // notices. Consecutive listing failures are the cheapest liveness signal.
        constexpr int MAP_FAILURES_BEFORE_REOPEN = 3;
        constexpr int SENT_EMPTY_LISTINGS_BEFORE_GIVING_UP = 3;
        constexpr int MESSAGE_LIST_MAX = 200;
        constexpr int SUPERVISOR_TICK_SECONDS = 1;

        // Whether the controller can carry ANCS right now. Re-read rather than
        // latched: see the comment in ConnectionManager::start().
        bool ancs_available() { return g_bluez && g_bluez->capability().mode == DeliveryMode::Full; }

        std::string normalize_address(std::string address) {
            for (char& c : address)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return address;
        }

        // Bearer operations backed by BlueZ on the system bus.
        class BluezBearerOps : public BearerOps {
        public:
            BluezBearerOps(BluezMonitor& monitor, std::string address)
                : monitor_(monitor), address_(normalize_address(std::move(address))) {}

            void set_address(const std::string& address) { address_ = normalize_address(address); }

            bool device_present() const override { return lookup().has_value(); }

            bool device_paired() const override {
                auto device = lookup();
                return device && device->paired;
            }

            bool classic_connected() const override {
                auto device = lookup();
                return device && device->classic_link_up();
            }

            bool le_bearer_available() const override {
                auto device = lookup();
                return device && device->has_le_bearer;
            }

            bool le_connected() const override {
                auto device = lookup();
                return device && device->le_connected;
            }

            void set_preferred_bearer(const std::string& bearer) override {
                auto device = lookup();
                if (!device)
                    return;
                GError* error = nullptr;
                GVariant* reply = g_dbus_connection_call_sync(
                    monitor_.connection(),
                    BLUEZ_NAME,
                    device->path.c_str(),
                    IFACE_PROPS,
                    "Set",
                    g_variant_new("(ssv)", IFACE_DEVICE, "PreferredBearer", g_variant_new_string(bearer.c_str())),
                    nullptr,
                    G_DBUS_CALL_FLAGS_NONE,
                    5000,
                    nullptr,
                    &error);
                if (reply)
                    g_variant_unref(reply);
                // Absent on older BlueZ; not having it is not an error.
                g_clear_error(&error);
            }

            bool connect_classic(std::string& err) override {
                auto device = lookup();
                if (!device) {
                    err = "device not present";
                    return false;
                }
                // bluez only exposes the per-bearer Connect while the device is connected on some bearer
                if (device->has_classic_bearer && device->connected) {
                    std::string bearer_err;
                    if (call(device->path, IFACE_BEARER_BREDR, "Connect", bearer_err))
                        return true;
                    // some versions publish the interface without the method
                    if (bearer_err.find("UnknownMethod") == std::string::npos) {
                        err = bearer_err;
                        return false;
                    }
                }
                return call(device->path, IFACE_DEVICE, "Connect", err);
            }

            bool connect_le(std::string& err) override {
                auto device = lookup();
                if (!device) {
                    err = "device not present";
                    return false;
                }
                if (!device->has_le_bearer) {
                    err = "no LE bearer";
                    return false;
                }
                std::string bearer_err;
                if (call(device->path, IFACE_BEARER_LE, "Connect", bearer_err))
                    return true;
                if (bearer_err.find("UnknownMethod") == std::string::npos) {
                    err = bearer_err;
                    return false;
                }
                return call(device->path, IFACE_DEVICE, "Connect", err);
            }

        private:
            std::optional<Device> lookup() const {
                for (const auto& device : monitor_.snapshot().devices) {
                    if (normalize_address(device.address) == address_)
                        return device;
                }
                return std::nullopt;
            }

            bool call(const std::string& path, const char* iface, const char* method, std::string& err) {
                GError* error = nullptr;
                GVariant* reply = g_dbus_connection_call_sync(monitor_.connection(),
                                                              BLUEZ_NAME,
                                                              path.c_str(),
                                                              iface,
                                                              method,
                                                              nullptr,
                                                              nullptr,
                                                              G_DBUS_CALL_FLAGS_NONE,
                                                              CONNECT_TIMEOUT_MS,
                                                              nullptr,
                                                              &error);
                if (!reply) {
                    err = error ? error->message : "unknown error";
                    g_clear_error(&error);
                    return false;
                }
                g_variant_unref(reply);
                return true;
            }

            BluezMonitor& monitor_;
            std::string address_;
        };

        // OBEX sessions live on the session bus, not the system bus.
        class ObexProfileOps : public ProfileOps {
        public:
            explicit ObexProfileOps(std::string address) : address_(normalize_address(std::move(address))) {}

            ~ObexProfileOps() override { g_clear_object(&conn_); }

            void set_address(const std::string& address) { address_ = normalize_address(address); }

            // The MAP session driver reuses this connection rather than opening a
            // second one to the same bus.
            GDBusConnection* bus() const { return conn_; }

            std::string create_session(const std::string& target, std::string& err) override {
                if (!ensure_connection(err))
                    return {};

                GVariantBuilder args;
                g_variant_builder_init(&args, G_VARIANT_TYPE("a{sv}"));
                g_variant_builder_add(&args, "{sv}", "Target", g_variant_new_string(target.c_str()));

                GError* error = nullptr;
                GVariant* reply = g_dbus_connection_call_sync(conn_,
                                                              OBEX_NAME,
                                                              OBEX_ROOT,
                                                              OBEX_CLIENT,
                                                              "CreateSession",
                                                              g_variant_new("(sa{sv})", address_.c_str(), &args),
                                                              G_VARIANT_TYPE("(o)"),
                                                              G_DBUS_CALL_FLAGS_NONE,
                                                              CONNECT_TIMEOUT_MS,
                                                              nullptr,
                                                              &error);
                if (!reply) {
                    err = error ? error->message : "unknown error";
                    g_clear_error(&error);
                    return {};
                }

                const gchar* path = nullptr;
                g_variant_get(reply, "(&o)", &path);
                std::string session = path ? path : "";
                g_variant_unref(reply);
                return session;
            }

            void remove_session(const std::string& path) override {
                if (path.empty() || !conn_)
                    return;
                GError* error = nullptr;
                GVariant* reply = g_dbus_connection_call_sync(conn_,
                                                              OBEX_NAME,
                                                              OBEX_ROOT,
                                                              OBEX_CLIENT,
                                                              "RemoveSession",
                                                              g_variant_new("(o)", path.c_str()),
                                                              nullptr,
                                                              G_DBUS_CALL_FLAGS_NONE,
                                                              5000,
                                                              nullptr,
                                                              &error);
                if (reply)
                    g_variant_unref(reply);
                g_clear_error(&error);
            }

        private:
            bool ensure_connection(std::string& err) {
                if (conn_)
                    return true;
                GError* error = nullptr;
                conn_ = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
                if (!conn_) {
                    err = std::string("session bus unavailable: ") + (error ? error->message : "unknown");
                    g_clear_error(&error);
                    return false;
                }
                return true;
            }

            GDBusConnection* conn_ = nullptr;
            std::string address_;
        };

    } // namespace

    struct ConnectionState {
        BluezMonitor* monitor = nullptr;
        ConnectionManager::StatusFn on_change;
        ConnectionManager::MessageFn on_message;

        std::unique_ptr<BluezBearerOps> bearer_ops;
        std::unique_ptr<ObexProfileOps> profile_ops;
        std::unique_ptr<BearerSupervisor> bearers;
        std::unique_ptr<ProfileSupervisor> profiles;

        std::thread thread;
        std::atomic<bool> running{false};
        std::mutex mutex;
        std::condition_variable wake;

        std::string address;
        // What the user asked for, versus what the controller can currently do.
        bool ancs_preference = true;
        bool ancs_enabled = true;
        bool link_was_ready = false;
        bool device_was_present = false;
        int map_failures = 0;
        int64_t next_message_poll = 0;
        std::string open_map_path;
        // Whether the phone answers a listing for a child of the message root.
        // Cleared once, permanently for the session, if it turns out not to.
        bool map_lists_subfolders = true;
        bool map_lists_sent = true;
        int map_empty_sent_listings = 0;
        // The first listing on a new MAP session returns the phone's existing
        // inbox, which on a first run is every message it holds.
        bool map_session_backfill = true;
        std::string pulled_pbap_path;

        std::unique_ptr<ancs::AncsClient> ancs_client;
        ConnectionManager::NotificationFn on_notification;
        ConnectionManager::WithdrawFn on_withdraw;
        bool ancs_ready = false;
        std::string ancs_reason;

        void run();
        void sync_messages(int64_t now);
        void sync_contacts();
        void sync_ancs(int64_t now);

        // Rebuilds the cached payload from the supervisors. Supervisor thread
        // only: it is the sole writer of BearerStatus and ProfileStatus, both of
        // which hold a std::string that no other thread may read directly.
        void refresh_status();

        // Hands the cached status to the subscriber only when it differs from the
        // last one published. The supervisors report their own internal
        // transitions as changes, and most of those are invisible in the payload,
        // so without this a subscriber receives the same status every second.
        void publish();

        // Guards everything below. publish() is reached from the supervisor
        // thread and from the ANCS readiness callback on the GLib thread, and
        // status() from whichever thread is serving a command.
        mutable std::mutex status_mutex;
        nlohmann::json current_status;
        nlohmann::json last_published;
    };

    void ConnectionState::refresh_status() {
        nlohmann::json payload = to_json(bearers->status(), profiles->status());
        std::lock_guard<std::mutex> lock(status_mutex);
        payload["ancs_ready"] = ancs_ready;
        payload["ancs_reason"] = ancs_reason;
        current_status = std::move(payload);
    }

    void ConnectionState::publish() {
        if (!on_change)
            return;

        nlohmann::json payload;
        {
            std::lock_guard<std::mutex> lock(status_mutex);
            if (current_status.is_null() || current_status == last_published)
                return;
            payload = current_status;
            last_published = payload;
        }
        on_change(payload);
    }

    nlohmann::json to_json(const BearerStatus& bearer, const ProfileStatus& profiles) {
        return {
            {"command", "bt_connection_changed"},
            {"device_present", bearer.device_present},
            {"device_paired", bearer.device_paired},
            {"classic_connected", bearer.classic_connected},
            {"le_available", bearer.le_available},
            {"le_connected", bearer.le_connected},
            {"map_open", profiles.map_open},
            {"pbap_open", profiles.pbap_open},
            {"map_error", to_string(profiles.map_error)},
            {"pbap_error", to_string(profiles.pbap_error)},
            // Two independent halves, so both reasons are reported; the UI shows
            // whichever is actionable.
            {"link_reason", bearer.reason},
            {"profile_reason", profiles.reason},
        };
    }

    // Pulls the inbox and folds it into the store, announcing anything new.
    //
    // ponytail: polling only. MAP's notification service would push new mail
    // instead, but obexd surfaces it as ObjectManager signals on the session bus,
    // which is a second watcher to build and maintain. Dedupe by handle makes
    // polling correct either way; add MNS if the poll latency proves annoying.
    void ConnectionState::sync_messages(int64_t now) {
        if (!profiles->status().map_open) {
            if (!open_map_path.empty()) {
                std::lock_guard<std::mutex> lock(g_map_mutex);
                g_map_session.reset();
                open_map_path.clear();
            }
            return;
        }

        if (open_map_path != profiles->map_session()) {
            open_map_path = profiles->map_session();
            auto session = std::make_shared<MapSession>(profile_ops->bus(), open_map_path);
            {
                std::lock_guard<std::mutex> lock(g_map_mutex);
                g_map_session = session;
            }
            // A freshly opened session starts at the root, and SetFolder walks the
            // MAP hierarchy from there — "msg" on its own is not a valid step.
            std::string err;
            if (!session->set_folder(MAP_MSG_ROOT, err))
                debug::log(WARN, "bluetooth: SetFolder({}) failed: {}", MAP_MSG_ROOT, err);
            next_message_poll = 0;
            map_session_backfill = true;
            map_lists_subfolders = true;
            map_lists_sent = true;
            map_empty_sent_listings = 0;
        }

        if (now < next_message_poll)
            return;
        next_message_poll = now + MESSAGE_POLL_SECONDS;

        std::shared_ptr<MapSession> session;
        {
            std::lock_guard<std::mutex> lock(g_map_mutex);
            session = g_map_session;
        }
        if (!session)
            return;

        std::string err;
        // Empty means the current folder; a name means that child of it.
        auto listings = session->list_messages(map_lists_subfolders ? MAP_FOLDER_INBOX : "", MESSAGE_LIST_MAX, err);

        // Some stacks only answer for the folder they are standing in. Walking
        // into the inbox costs the sent folder but keeps messages working, so it
        // is worth one attempt before declaring the session dead.
        if (!err.empty() && map_lists_subfolders) {
            debug::log(
                WARN, "bluetooth: listing '{}' failed ({}); falling back to the inbox alone", MAP_FOLDER_INBOX, err);
            map_lists_subfolders = false;
            map_lists_sent = false;
            std::string nav_err;
            // Relative: the session is parked on the message root.
            if (!session->set_folder(MAP_FOLDER_INBOX, nav_err))
                debug::log(WARN, "bluetooth: SetFolder({}) failed: {}", MAP_FOLDER_INBOX, nav_err);
            err.clear();
            listings = session->list_messages("", MESSAGE_LIST_MAX, err);
        }

        if (!err.empty()) {
            debug::log(WARN, "bluetooth: ListMessages failed: {}", err);
            // ponytail: a fail count, not an obexd ObjectManager watcher. Costs up
            // to MAP_FAILURES_BEFORE_REOPEN polls of latency; subscribe to
            // org.bluez.obex InterfacesRemoved if that ever proves too slow.
            if (++map_failures >= MAP_FAILURES_BEFORE_REOPEN) {
                debug::log(WARN, "bluetooth: MAP session is not answering; reopening it");
                map_failures = 0;
                profiles->drop_map();
                std::lock_guard<std::mutex> lock(g_map_mutex);
                g_map_session.reset();
                open_map_path.clear();
            }
            return;
        }
        map_failures = 0;

        // Replies sent from the phone itself live in a separate folder and name
        // the user as their sender, so without this a conversation only ever
        // shows one side of itself.
        if (map_lists_sent) {
            std::string sent_err;
            auto sent = session->list_messages(MAP_FOLDER_SENT, MESSAGE_LIST_MAX, sent_err);
            if (!sent_err.empty()) {
                debug::log(
                    WARN, "bluetooth: the sent folder is unavailable ({}); showing received messages only", sent_err);
                map_lists_sent = false;
            } else if (sent.empty() && !listings.empty()) {
                if (++map_empty_sent_listings >= SENT_EMPTY_LISTINGS_BEFORE_GIVING_UP) {
                    debug::log(INFO,
                               "bluetooth: the phone serves no sent messages over MAP; "
                               "conversations will show received messages and replies sent from here");
                    map_lists_sent = false;
                }
            } else {
                map_empty_sent_listings = 0;
                listings.insert(listings.end(), sent.begin(), sent.end());
            }
        }

        std::vector<Message> fresh;
        {
            std::lock_guard<std::mutex> lock(g_messages_mutex);
            for (const auto& listing : listings) {
                Message message = message_from_listing(listing);
                if (message.thread_key.empty())
                    continue;

                // MAP gives a group message one sender and no conversation
                // identifier. The correlated Messages notification is the only
                // side channel, and an ambiguous match is refused rather than
                // guessed: filing it wrongly would also aim a reply wrongly.
                if (g_group_replies_enabled) {
                    GroupInfo info;
                    std::lock_guard<std::mutex> group_lock(g_group_mutex);
                    if (g_correlator.correlate(message.body, now, info) == Correlation::Matched) {
                        const std::string key = group_thread_key(info);
                        if (!key.empty()) {
                            // The sender label is display only and never becomes
                            // part of the thread identity or the reply route.
                            message.peer_name = info.sender;
                            message.thread_key = key;
                            g_group_info[key] = info;
                        }
                    }
                }
                if (g_messages.add(message))
                    fresh.push_back(message);
                else
                    g_messages.set_read(message.handle, message.read);
            }
        }

        const bool backfill = map_session_backfill;
        map_session_backfill = false;

        for (const auto& message : fresh) {
            {
                std::lock_guard<std::mutex> lock(g_messages_mutex);
                g_journal.append(message);
            }
            if (on_message)
                on_message(message, backfill);
        }
    }

    // Pulls the phonebook once per PBAP session. Contacts change far more slowly
    // than messages, and a full pull is an expensive transfer to repeat.
    void ConnectionState::sync_contacts() {
        if (!profiles->status().pbap_open) {
            pulled_pbap_path.clear();
            return;
        }
        if (pulled_pbap_path == profiles->pbap_session())
            return;
        pulled_pbap_path = profiles->pbap_session();

        PbapSession session(profile_ops->bus(), pulled_pbap_path);
        std::string err;
        if (!session.select_phonebook(err)) {
            debug::log(WARN, "bluetooth: PBAP Select failed: {}", err);
            return;
        }

        auto contacts = session.pull_all(err);
        if (!err.empty()) {
            debug::log(WARN, "bluetooth: PBAP PullAll failed: {}", err);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(g_messages_mutex);
            contact_store().set(std::move(contacts));
        }
        save_contacts(contact_store());
        debug::log(INFO, "bluetooth: pulled {} contacts", contact_store().size());
    }

    // Keeps the ANCS client pointed at the LE session and drains its work.
    void ConnectionState::sync_ancs(int64_t now) {
        if (!ancs_client)
            return;

        // ANCS rides the LE bearer. Without it there is nothing to talk to, and
        // pointing the client at an empty path is what makes it forget a session
        // that no longer exists.
        std::string device_path;
        if (bearers->status().le_connected) {
            auto objects = monitor->snapshot();
            for (const auto& device : objects.devices) {
                if (device.address == address || normalize_address(device.address) == normalize_address(address)) {
                    device_path = device.path;
                    break;
                }
            }
        }

        ancs_client->set_device(device_path);
        ancs_client->tick(now);
    }

    void ConnectionState::run() {
        using namespace std::chrono;
        const auto started = steady_clock::now();

        while (running) {
            const int64_t now = duration_cast<seconds>(steady_clock::now() - started).count();

            bearers->tick(now);
            const bool present = bearers->status().device_present;
            const bool link_ready = bearers->status().classic_connected;

            // A phone that walks back into range must not sit out a backoff that
            // grew to minutes while it was gone.
            if (present && !device_was_present)
                bearers->reset();
            device_was_present = present;

            // A dropped Classic link invalidates every OBEX session on top of it,
            // along with any backoff accumulated against the link that just died.
            if (link_was_ready && !link_ready) {
                profiles->reset();
                bearers->reset();
                map_failures = 0;
            }
            link_was_ready = link_ready;

            profiles->tick(now, link_ready);

            // publish() drops a payload identical to the last one, so refreshing
            // every tick costs nothing and cannot miss a transition.
            refresh_status();
            publish();

            sync_contacts();
            sync_messages(now);
            sync_ancs(now);

            std::unique_lock<std::mutex> lock(mutex);
            wake.wait_for(lock, seconds(SUPERVISOR_TICK_SECONDS), [this] { return !running; });
        }
    }

    ConnectionManager::ConnectionManager(BluezMonitor& monitor, StatusFn on_change, MessageFn on_message)
        : state_(std::make_unique<ConnectionState>()) {
        state_->monitor = &monitor;
        state_->on_change = std::move(on_change);
        state_->on_message = std::move(on_message);
    }

    ConnectionManager::~ConnectionManager() { stop(); }

    bool ConnectionManager::start(const std::string& address, bool ancs_enabled) {
        if (state_->running)
            return true;
        if (address.empty()) {
            debug::log(INFO, "bluetooth: no paired iPhone selected; supervision idle");
            return false;
        }

        state_->address = address;
        state_->ancs_preference = ancs_enabled;
        // Read the controller's capability here rather than letting each caller
        // latch its own copy. At startup BlueZ may not have finished enumerating
        // the adapter, and its free advertising-instance count drops to zero
        // while one of our own adverts is registered — a caller that sampled
        // either would disable mirroring for the rest of the daemon's life.
        const bool ancs_effective = ancs_enabled && ancs_available();
        state_->ancs_enabled = ancs_effective;

        // Replay history before the first poll so the store is never briefly
        // empty, and so previously seen handles dedupe instead of re-announcing.
        {
            std::lock_guard<std::mutex> lock(g_messages_mutex);
            contact_store() = load_contacts();
            if (g_journal.open()) {
                const int64_t now = static_cast<int64_t>(std::time(nullptr));
                auto history = g_journal.load(now);
                for (const auto& message : history)
                    g_messages.add(message);
                // load() already applied retention, so writing back what survived
                // is what actually trims the file. Idempotent when nothing aged out.
                g_journal.compact(history);
                debug::log(INFO, "bluetooth: replayed {} message(s) from the journal", history.size());
            }
        }
        state_->bearer_ops = std::make_unique<BluezBearerOps>(*state_->monitor, address);
        state_->profile_ops = std::make_unique<ObexProfileOps>(address);
        state_->bearers = std::make_unique<BearerSupervisor>(*state_->bearer_ops, ancs_enabled);
        state_->profiles = std::make_unique<ProfileSupervisor>(*state_->profile_ops);

        // Only built when the caller can actually show notifications and the
        // bond can carry them.
        if (ancs_effective && state_->on_notification) {
            auto* raw = state_.get();
            state_->ancs_client = std::make_unique<ancs::AncsClient>(
                *state_->monitor,
                [raw](const ancs::Notification& notification) {
                    if (raw->on_notification)
                        raw->on_notification(notification);
                },
                [raw](uint32_t uid) {
                    if (raw->on_withdraw)
                        raw->on_withdraw(uid);
                },
                [raw](bool ready, const std::string& reason) {
                    {
                        // Fold straight into the cached payload: this runs on the
                        // GLib thread, which must not read supervisor state.
                        std::lock_guard<std::mutex> lock(raw->status_mutex);
                        raw->ancs_ready = ready;
                        raw->ancs_reason = reason;
                        if (!raw->current_status.is_null()) {
                            raw->current_status["ancs_ready"] = ready;
                            raw->current_status["ancs_reason"] = reason;
                        }
                    }
                    debug::log(INFO, "ancs: {}", reason);
                    raw->publish();
                });
            state_->ancs_client->set_content_enabled(load_config().ancs_content_enabled);
        }

        state_->running = true;
        state_->thread = std::thread([this] { state_->run(); });
        debug::log(INFO, "bluetooth: supervising {} (ANCS {})", address, ancs_effective ? "enabled" : "disabled");
        return true;
    }

    void ConnectionManager::stop() {
        if (!state_->running && !state_->thread.joinable())
            return;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->running = false;
        }
        state_->wake.notify_all();
        if (state_->thread.joinable())
            state_->thread.join();

        // MapSession borrows profile_ops' bus connection, and start() replaces
        // profile_ops. Drop the session before anything can hand out a pointer
        // into a connection that is about to go away.
        {
            std::lock_guard<std::mutex> lock(g_map_mutex);
            g_map_session.reset();
        }

        // Everything below is scoped to one device and one set of sessions, and
        // must not leak into the next start().
        state_->ancs_client.reset();
        state_->open_map_path.clear();
        state_->pulled_pbap_path.clear();
        state_->map_session_backfill = true;
        state_->map_failures = 0;
        state_->next_message_poll = 0;
        state_->link_was_ready = false;
        state_->device_was_present = false;
        {
            std::lock_guard<std::mutex> lock(state_->status_mutex);
            state_->current_status = nullptr;
            state_->last_published = nullptr;
            state_->ancs_ready = false;
            state_->ancs_reason.clear();
        }
    }

    void ConnectionManager::set_notification_handlers(NotificationFn on_notification, WithdrawFn on_withdraw) {
        state_->on_notification = std::move(on_notification);
        state_->on_withdraw = std::move(on_withdraw);
    }

    bool ConnectionManager::perform_notification_action(uint32_t uid, ancs::ActionId action) {
        return state_->ancs_client && state_->ancs_client->perform_action(uid, action);
    }

    nlohmann::json ConnectionManager::notifications(size_t limit) const {
        nlohmann::json out = nlohmann::json::array();
        if (!state_->ancs_client)
            return out;
        for (const auto& notification : state_->ancs_client->recent_notifications(limit))
            out.push_back(ancs::to_json(notification));
        return out;
    }

    void ConnectionManager::set_device(const std::string& address, bool ancs_enabled) {
        // Restarting is simpler than mutating supervisor state under the worker,
        // and selecting a device is rare. stop() clears the per-device state.
        stop();
        start(address, ancs_enabled);
    }

    void ConnectionManager::refresh_capability() {
        if (!state_->running || state_->address.empty())
            return;
        const bool wanted = state_->ancs_preference && ancs_available();
        if (wanted == state_->ancs_enabled)
            return;
        debug::log(INFO,
                   "bluetooth: notification mirroring is now {} (controller capability changed)",
                   wanted ? "available" : "unavailable");
        const std::string address = state_->address;
        set_device(address, state_->ancs_preference);
    }

    nlohmann::json ConnectionManager::status() const {
        // The supervisors' own status objects belong to the supervisor thread;
        // callers get the snapshot it published instead.
        std::lock_guard<std::mutex> lock(state_->status_mutex);
        if (state_->current_status.is_null()) {
            nlohmann::json idle;
            idle["command"] = "bt_connection_changed";
            idle["device_present"] = false;
            idle["device_paired"] = false;
            idle["link_reason"] = "No paired iPhone selected.";
            idle["profile_reason"] = "";
            return idle;
        }
        return state_->current_status;
    }

} // namespace tether::bluetooth
