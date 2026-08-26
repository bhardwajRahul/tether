#include "tether/bluetooth/pairing.hpp"
#include "tether/bluetooth/advert.hpp"
#include "tether/bluetooth/agent.hpp"
#include "tether/bluetooth/monitor.hpp"
#include "tether/log.hpp"
#include <tether/i18n.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace tether::bluetooth {

    namespace {

        constexpr const char* BLUEZ_NAME = "org.bluez";
        constexpr const char* IFACE_DEVICE = "org.bluez.Device1";
        constexpr const char* IFACE_ADAPTER = "org.bluez.Adapter1";
        constexpr const char* IFACE_PROPS = "org.freedesktop.DBus.Properties";

        // iphone shows its confirmation prompt and waits for a slow human
        constexpr int PAIR_TIMEOUT_SECONDS = 90;
        constexpr int DIALOG_TIMEOUT_SECONDS = 60;

        // A refused connect still gets a short grace window, because Connect() can
        // report failure while authentication completes behind it.
        constexpr int CONNECT_REFUSED_GRACE_SECONDS = 5;

        constexpr int CLASSIC_SETTLE_SECONDS = 3;
        constexpr int DISCOVERY_TIMEOUT_SECONDS = 30;

        // how long to stand back after bluez refuses to put the solicitation on air
        constexpr int SOLICIT_RETRY_SECONDS = 30;

        std::string normalize_address(std::string address) {
            for (char& c : address)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return address;
        }

        void notify(const ProgressFn& progress, const std::string& step, const std::string& detail) {
            debug::log(INFO, "bluetooth: {} {}", step, detail);
            if (progress)
                progress(step, detail);
        }

        bool set_property(GDBusConnection* conn,
                          const std::string& path,
                          const char* iface,
                          const char* name,
                          GVariant* value,
                          std::string* err_out = nullptr) {
            GError* error = nullptr;
            GVariant* reply = g_dbus_connection_call_sync(conn,
                                                          BLUEZ_NAME,
                                                          path.c_str(),
                                                          IFACE_PROPS,
                                                          "Set",
                                                          g_variant_new("(ssv)", iface, name, value),
                                                          nullptr,
                                                          G_DBUS_CALL_FLAGS_NONE,
                                                          5000,
                                                          nullptr,
                                                          &error);
            if (!reply) {
                if (err_out)
                    *err_out = error ? error->message : "unknown";
                g_clear_error(&error);
                return false;
            }
            g_variant_unref(reply);
            return true;
        }

        bool call_device(
            GDBusConnection* conn, const std::string& path, const char* method, int timeout_ms, std::string& err_out) {
            GError* error = nullptr;
            GVariant* reply = g_dbus_connection_call_sync(conn,
                                                          BLUEZ_NAME,
                                                          path.c_str(),
                                                          IFACE_DEVICE,
                                                          method,
                                                          nullptr,
                                                          nullptr,
                                                          G_DBUS_CALL_FLAGS_NONE,
                                                          timeout_ms,
                                                          nullptr,
                                                          &error);
            if (!reply) {
                err_out = error ? error->message : "unknown error";
                g_clear_error(&error);
                return false;
            }
            g_variant_unref(reply);
            return true;
        }

        const Device* find_by_address(const BluezObjects& objects, const std::string& address) {
            for (const auto& d : objects.devices) {
                if (normalize_address(d.address) == address)
                    return &d;
            }
            return nullptr;
        }

        // Copies out the device record rather than holding a pointer into a
        // snapshot that the watcher thread replaces underneath us.
        bool lookup(BluezMonitor& monitor, const std::string& address, Device& out) {
            auto objects = monitor.snapshot();
            if (const Device* d = find_by_address(objects, address)) {
                out = *d;
                return true;
            }
            return false;
        }

        bool call_adapter(GDBusConnection* conn,
                          const std::string& path,
                          const char* method,
                          std::string* err_out = nullptr) {
            GError* error = nullptr;
            GVariant* reply = g_dbus_connection_call_sync(conn,
                                                          BLUEZ_NAME,
                                                          path.c_str(),
                                                          IFACE_ADAPTER,
                                                          method,
                                                          nullptr,
                                                          nullptr,
                                                          G_DBUS_CALL_FLAGS_NONE,
                                                          5000,
                                                          nullptr,
                                                          &error);
            if (!reply) {
                const std::string message = error && error->message ? error->message : "unknown";
                debug::log(WARN, "bluetooth: {} failed: {}", method, message);
                if (err_out)
                    *err_out = message;
                g_clear_error(&error);
                return false;
            }
            g_variant_unref(reply);
            return true;
        }

        bool get_adapter_bool(GDBusConnection* conn, const std::string& adapter, const char* name) {
            GError* error = nullptr;
            GVariant* reply = g_dbus_connection_call_sync(conn,
                                                          BLUEZ_NAME,
                                                          adapter.c_str(),
                                                          IFACE_PROPS,
                                                          "Get",
                                                          g_variant_new("(ss)", IFACE_ADAPTER, name),
                                                          G_VARIANT_TYPE("(v)"),
                                                          G_DBUS_CALL_FLAGS_NONE,
                                                          5000,
                                                          nullptr,
                                                          &error);
            if (!reply) {
                g_clear_error(&error);
                return false;
            }
            GVariant* boxed = nullptr;
            g_variant_get(reply, "(v)", &boxed);
            const bool value = boxed && g_variant_get_boolean(boxed);
            if (boxed)
                g_variant_unref(boxed);
            g_variant_unref(reply);
            return value;
        }

        // Scans until the device shows up. Needed because an unpair makes BlueZ
        // forget the device entirely, which would otherwise leave no way to pair
        // again from inside the app.
        bool discover(BluezMonitor& monitor, GDBusConnection* conn, const std::string& address, Device& out) {
            auto objects = monitor.snapshot();
            if (objects.adapters.empty())
                return false;
            const std::string adapter = objects.adapters.front().path;

            if (!call_adapter(conn, adapter, "StartDiscovery"))
                return false;

            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(DISCOVERY_TIMEOUT_SECONDS);
            bool found = false;
            while (std::chrono::steady_clock::now() < deadline) {
                if (lookup(monitor, address, out)) {
                    found = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }

            // discovery must stop before pairing
            call_adapter(conn, adapter, "StopDiscovery");
            return found;
        }

        // Connect-first makes the iPhone the authentication initiator, so BlueZ
        // must accept an inbound pairing request for the transaction to complete.
        // A desktop adapter normally sits with Pairable off, in which case iOS
        // never shows its half of the numeric comparison and the link fails with
        // br-connection-key-missing.
        //
        // Restores the previous values on every exit path, including the early
        // returns and the timeout.
        class PairableWindow {
        public:
            PairableWindow(GDBusConnection* conn, std::string adapter) : conn_(conn), adapter_(std::move(adapter)) {
                if (adapter_.empty())
                    return;
                was_pairable_ = get_bool(IFACE_ADAPTER, "Pairable");
                was_discoverable_ = get_bool(IFACE_ADAPTER, "Discoverable");
                set_bool("Pairable", true);
                set_bool("Discoverable", true);
            }

            ~PairableWindow() {
                if (adapter_.empty())
                    return;
                set_bool("Pairable", was_pairable_);
                set_bool("Discoverable", was_discoverable_);
            }

            PairableWindow(const PairableWindow&) = delete;
            PairableWindow& operator=(const PairableWindow&) = delete;

        private:
            bool get_bool(const char* iface, const char* name) const {
                GError* error = nullptr;
                GVariant* reply = g_dbus_connection_call_sync(conn_,
                                                              BLUEZ_NAME,
                                                              adapter_.c_str(),
                                                              IFACE_PROPS,
                                                              "Get",
                                                              g_variant_new("(ss)", iface, name),
                                                              G_VARIANT_TYPE("(v)"),
                                                              G_DBUS_CALL_FLAGS_NONE,
                                                              5000,
                                                              nullptr,
                                                              &error);
                if (!reply) {
                    g_clear_error(&error);
                    return false;
                }
                GVariant* boxed = nullptr;
                g_variant_get(reply, "(v)", &boxed);
                const bool value = boxed && g_variant_get_boolean(boxed);
                if (boxed)
                    g_variant_unref(boxed);
                g_variant_unref(reply);
                return value;
            }

            void set_bool(const char* name, bool value) const {
                set_property(conn_, adapter_, IFACE_ADAPTER, name, g_variant_new_boolean(value));
            }

            GDBusConnection* conn_;
            std::string adapter_;
            bool was_pairable_ = false;
            bool was_discoverable_ = false;
        };

        std::string adapter_for(BluezMonitor& monitor, const Device& device) {
            if (!device.adapter_path.empty())
                return device.adapter_path;
            auto objects = monitor.snapshot();
            return objects.adapters.empty() ? std::string{} : objects.adapters.front().path;
        }

        std::mutex g_advert_mutex;
        AncsAdvertisement* g_advert = nullptr;

        std::atomic<bool> g_pairing_owns_advert{false};

        struct AdvertOwnership {
            AdvertOwnership() { g_pairing_owns_advert = true; }
            ~AdvertOwnership() { g_pairing_owns_advert = false; }
            AdvertOwnership(const AdvertOwnership&) = delete;
            AdvertOwnership& operator=(const AdvertOwnership&) = delete;
        };

        // An advert soliciting ANCS must never be on air while Classic pairing is in flight
        void stop_advert_locked(BluezMonitor& monitor) {
            if (!g_advert)
                return;
            g_advert->unregister_with_bluez();
            monitor.invoke_sync([&] { g_advert->unexport_object(); });
            delete g_advert;
            g_advert = nullptr;
        }

        void stop_advert(BluezMonitor& monitor) {
            std::lock_guard<std::mutex> lock(g_advert_mutex);
            stop_advert_locked(monitor);
        }

        // Puts the solicitation advert on air, replacing whatever was there.
        bool start_advert(BluezMonitor& monitor, const std::string& adapter_path, std::string& err) {
            if (adapter_path.empty()) {
                err = "No Bluetooth adapter is available.";
                return false;
            }
            std::lock_guard<std::mutex> lock(g_advert_mutex);
            stop_advert_locked(monitor);
            auto* advert = new AncsAdvertisement(monitor.connection(), adapter_path);

            bool exported = false;
            monitor.invoke_sync([&] { exported = advert->export_object(); });
            // BlueZ reads the advertisement's properties back before returning, so
            // this call must stay off the thread that answers those reads.
            if (exported && advert->register_with_bluez()) {
                g_advert = advert;
                return true;
            }

            monitor.invoke_sync([&] { advert->unexport_object(); });
            delete advert;
            err = "Could not advertise for ANCS. The controller may have no free LE advertising instance.";
            return false;
        }

        // Reads Device1.Paired straight from BlueZ rather than from the monitor's
        // snapshot. The confirmation dialog blocks the monitor's GLib thread for as
        // long as the user takes to answer, so the snapshot is frozen for exactly
        // the window in which the bond completes — polling it reports a timeout for
        // a pairing that actually succeeded.
        bool device_is_paired(GDBusConnection* conn, const std::string& path) {
            GError* error = nullptr;
            GVariant* reply = g_dbus_connection_call_sync(conn,
                                                          BLUEZ_NAME,
                                                          path.c_str(),
                                                          IFACE_PROPS,
                                                          "Get",
                                                          g_variant_new("(ss)", IFACE_DEVICE, "Paired"),
                                                          G_VARIANT_TYPE("(v)"),
                                                          G_DBUS_CALL_FLAGS_NONE,
                                                          5000,
                                                          nullptr,
                                                          &error);
            if (!reply) {
                g_clear_error(&error);
                return false;
            }
            GVariant* boxed = nullptr;
            g_variant_get(reply, "(v)", &boxed);
            const bool paired =
                boxed && g_variant_is_of_type(boxed, G_VARIANT_TYPE_BOOLEAN) && g_variant_get_boolean(boxed);
            if (boxed)
                g_variant_unref(boxed);
            g_variant_unref(reply);
            return paired;
        }

    } // namespace

    bool solicit_ancs(BluezMonitor& monitor, std::string& err) {
        if (!monitor.connection()) {
            err = "Bluetooth is unavailable.";
            return false;
        }
        // Deliberately not gated on Capability::advertising: BlueZ reports zero
        // free instances while one of our own adverts is registered, which is
        // precisely the state a user retrying this is likely to be in. Try, and
        // report what BlueZ actually says.
        auto objects = monitor.snapshot();
        const std::string adapter = objects.adapters.empty() ? std::string{} : objects.adapters.front().path;
        return start_advert(monitor, adapter, err);
    }

    bool ancs_solicitation_active() {
        std::lock_guard<std::mutex> lock(g_advert_mutex);
        return g_advert && g_advert->active();
    }

    void stop_ancs_solicitation(BluezMonitor& monitor) { stop_advert(monitor); }

    void supervise_ancs_solicitation(BluezMonitor& monitor, bool want) {
        if (g_pairing_owns_advert)
            return;
        if (!want) {
            if (ancs_solicitation_active())
                stop_advert(monitor);
            return;
        }
        if (ancs_solicitation_active())
            return;

        static std::chrono::steady_clock::time_point next_attempt{};
        const auto now = std::chrono::steady_clock::now();
        if (now < next_attempt)
            return;

        std::string err;
        if (solicit_ancs(monitor, err)) {
            next_attempt = {};
            return;
        }
        next_attempt = now + std::chrono::seconds(SOLICIT_RETRY_SECONDS);
        debug::log(WARN, "bluetooth: could not re-arm the ANCS solicitation ({})", err);
    }

    bool confirm_with_dialog(const std::string& device_name, const std::string& code, bool& unavailable) {
        // Translated before the fork: gettext takes a lock, and calling it in the
        // child of a threaded process can deadlock if another thread held it.
        // TRANSLATORS: {0} is the device name, {1} is the numeric pairing code.
        const std::string body = tr_format(
            _("{0} wants to pair.\n\nConfirm this code matches the one on your iPhone:\n\n{1}"), device_name, code);
        const std::string title = _("Bluetooth Pairing");
        const std::string accept = _("Confirm");
        const std::string reject = _("Cancel");

        pid_t pid = fork();
        if (pid < 0) {
            debug::log(ERR, "bluetooth: fork() for confirmation dialog failed");
            unavailable = true;
            return false;
        }

        if (pid == 0) {
            std::filesystem::path self_path;
            try {
                self_path = std::filesystem::read_symlink("/proc/self/exe");
            } catch (...) {
            }
            std::string sibling = (self_path.parent_path() / "tether-dialog").string();
            std::string timeout = std::to_string(DIALOG_TIMEOUT_SECONDS);

            execl(sibling.c_str(),
                  "tether-dialog",
                  "--title",
                  title.c_str(),
                  "--body",
                  body.c_str(),
                  "--accept",
                  accept.c_str(),
                  "--reject",
                  reject.c_str(),
                  "--timeout",
                  timeout.c_str(),
                  nullptr);
            execlp("tether-dialog",
                   "tether-dialog",
                   "--title",
                   title.c_str(),
                   "--body",
                   body.c_str(),
                   "--accept",
                   accept.c_str(),
                   "--reject",
                   reject.c_str(),
                   "--timeout",
                   timeout.c_str(),
                   nullptr);
            _exit(3);
        }

        int status = 0;
        if (waitpid(pid, &status, 0) < 0) {
            unavailable = true;
            return false;
        }
        // Exit 0 is the only acceptance. A dialog that could not be shown at all exits 3
        if (!WIFEXITED(status) || WEXITSTATUS(status) == 3)
            unavailable = true;
        return WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }

    nlohmann::json to_json(const PairResult& result) {
        return {
            {"command", "bt_pair_result"},
            {"success", result.success},
            {"status", result.status},
            {"message", result.message},
            {"address", result.device_address},
            {"dual_bond", result.dual_bond},
            {"auth_strategy_used", to_string(result.auth_strategy_used)},
        };
    }

    bool should_fall_back(AuthStrategy tried, bool paired, bool confirmation_failed) {
        return tried == AuthStrategy::ConnectFirst && !paired && !confirmation_failed;
    }

    PairResult pair_device(BluezMonitor& monitor,
                           const std::string& address,
                           AuthStrategy strategy,
                           const ProgressFn& progress,
                           const ConfirmFn& confirm) {
        PairResult result;
        result.device_address = normalize_address(address);

        GDBusConnection* conn = monitor.connection();
        if (!conn) {
            result.status = "error";
            result.message = "Bluetooth is unavailable.";
            return result;
        }

        AdvertOwnership advert_owned;
        PairableWindow pairable(conn, [&] {
            auto objects = monitor.snapshot();
            return objects.adapters.empty() ? std::string{} : objects.adapters.front().path;
        }());

        Device device;
        if (!lookup(monitor, result.device_address, device)) {
            notify(progress, "discovering", result.device_address);
            if (!discover(monitor, conn, result.device_address, device)) {
                result.status = "not_found";
                result.message = "Device " + result.device_address +
                                 " is not visible to BlueZ. Unlock the iPhone and open its Bluetooth settings so it "
                                 "is discoverable, then try again.";
                return result;
            }
        }
        result.device_path = device.path;

        const std::string adapter_path = adapter_for(monitor, device);
        const std::string display_name = device.name.empty() ? result.device_address : device.name;

        if (device.paired) {
            notify(progress, "already_paired", display_name);
            result.success = true;
            result.status = "already_paired";
            result.dual_bond = device.has_le_bearer && device.le_bonded;
            result.message = display_name + " is already paired.";
        } else {
            auto cap = monitor.capability();
            if (!cap.class_ok) {
                notify(progress,
                       "warning",
                       "Adapter class is not A/V Hands-Free; the iPhone may not offer its permissions. "
                       "Run scripts/bt-probe.sh --set-class.");
            }

            // Silence any advert left over from an earlier pairing before
            // authentication starts.
            stop_advert(monitor);

            // The agent's callback lands on the monitor's GLib thread.
            std::atomic<bool> auth_seen{false};
            std::atomic<bool> user_rejected{false};
            std::atomic<bool> confirm_unavailable{false};

            PairingAgent agent(conn, device.path, [&](const std::string& code) {
                auth_seen = true;
                notify(progress, "confirm", code);

                bool unavailable = false;
                bool accepted = confirm_with_dialog(display_name, code, unavailable);

                if (unavailable && confirm) {
                    accepted = confirm(code);
                    unavailable = false;
                }

                if (unavailable)
                    confirm_unavailable = true;
                else if (!accepted)
                    user_rejected = true;
                return accepted;
            });

            bool exported = false;
            monitor.invoke_sync([&] { exported = agent.export_object(); });
            if (!exported || !agent.register_with_bluez()) {
                monitor.invoke_sync([&] { agent.unexport_object(); });
                result.status = "error";
                result.message = "Could not register a Bluetooth pairing agent.";
                return result;
            }

            std::string err;
            bool initiated = false;

            // A Connect() that failed can leave an attempt in flight, which makes
            // the next call fail fast with br-connection-busy.
            auto clear_attempt_in_flight = [&] {
                std::string ignored;
                call_device(conn, device.path, "Disconnect", 5000, ignored);
                std::this_thread::sleep_for(std::chrono::seconds(2));
            };

            // The agent stays registered across all attempts; re-registering
            // mid-transaction races BlueZ's own agent bookkeeping.
            auto attempt = [&](AuthStrategy how) {
                err.clear();
                if (how == AuthStrategy::ConnectFirst) {
                    notify(progress, "connecting", display_name);
                    initiated = call_device(conn, device.path, "Connect", PAIR_TIMEOUT_SECONDS * 1000, err);
                } else {
                    notify(progress, "pairing", display_name);
                    initiated = call_device(conn, device.path, "Pair", PAIR_TIMEOUT_SECONDS * 1000, err);
                }

                // Connect() can report failure while authentication still completes so a refusal is waited out too
                const int seconds = (!initiated && !auth_seen) ? CONNECT_REFUSED_GRACE_SECONDS : PAIR_TIMEOUT_SECONDS;
                auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
                while (std::chrono::steady_clock::now() < deadline) {
                    if (device_is_paired(conn, device.path))
                        return true;
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                }
                return false;
            };

            bool paired = attempt(strategy);
            result.auth_strategy_used = strategy;

            // Connect-first is the only transaction that yields the LE half of the
            // bond, and the iPhone's refusal of it can be transient, so it is worth
            // a second attempt before trading notifications away.
            if (should_fall_back(strategy, paired, user_rejected || confirm_unavailable)) {
                notify(progress, "retrying", "the iPhone refused the connection; trying once more");
                clear_attempt_in_flight();
                paired = attempt(AuthStrategy::ConnectFirst);
            }

            if (should_fall_back(strategy, paired, user_rejected || confirm_unavailable)) {
                notify(progress,
                       "retrying",
                       "the iPhone will not start pairing over the connection; requesting authentication directly. "
                       "A bond made this way can carry messages and contacts but not notifications");
                clear_attempt_in_flight();
                result.auth_strategy_used = AuthStrategy::ExplicitPair;
                paired = attempt(AuthStrategy::ExplicitPair);
            }

            agent.unregister_with_bluez();
            monitor.invoke_sync([&] { agent.unexport_object(); });

            if (!paired) {
                if (confirm_unavailable) {
                    result.status = "error";
                    result.message = "The pairing code could not be shown for confirmation: this computer has no "
                                     "display, and whatever started the pairing did not answer either. Run tether "
                                     "--bt-pair " +
                                     result.device_address + " from a terminal and confirm the code there.";
                } else if (user_rejected) {
                    result.status = "rejected";
                    result.message = "Pairing was not confirmed on this computer.";
                } else if (err.find("Rejected") != std::string::npos) {
                    result.status = "rejected";
                    result.message = "The iPhone declined the pairing request.";
                } else if (!auth_seen) {
                    result.status = "timeout";
                    result.message =
                        "The iPhone refused the connection before pairing started" + (err.empty() ? "" : ": " + err) +
                        ". Delete every entry for this computer on the iPhone (Settings -> Bluetooth, there can be "
                        "two), run tether --bt-unpair " +
                        result.device_address + ", then try again.";
                } else {
                    result.status = "timeout";
                    result.message = initiated ? "Pairing did not complete. Confirm the prompt on the iPhone."
                                               : ("Pairing failed: " + err);
                }
                return result;
            }

            notify(progress, "paired", display_name);
            result.success = true;
            result.status = "paired";
            result.message = "Paired with " + display_name + ".";
        }

        // Trusting the bond lets BlueZ reconnect without asking again.
        std::string trust_err;
        if (!set_property(conn, device.path, IFACE_DEVICE, "Trusted", g_variant_new_boolean(TRUE), &trust_err))
            debug::log(WARN, "bluetooth: could not trust device: {}", trust_err);

        // Prefer BR/EDR for the next outbound connection, then let the Classic ACL
        // settle before anything touches LE. Older BlueZ has no such property.
        set_property(conn, device.path, IFACE_DEVICE, "PreferredBearer", g_variant_new_string("bredr"));
        notify(progress, "settling", "waiting for the Classic link to settle");
        std::this_thread::sleep_for(std::chrono::seconds(CLASSIC_SETTLE_SECONDS));

        Device settled;
        if (lookup(monitor, result.device_address, settled))
            result.dual_bond = settled.has_le_bearer && settled.le_bonded;

        // Hand the preference back to LE now that Classic has settled. Leaving the
        // bond pinned to BR/EDR keeps the LE half down indefinitely
        set_property(conn, device.path, IFACE_DEVICE, "PreferredBearer", g_variant_new_string("le"));

        // Only now solicit ANCS. This advert is what makes iOS reveal its
        // "Show Message Notifications" and "Sync Contacts" toggles, and it is safe
        // to broadcast because the bond already exists.
        std::string advert_err;
        if (start_advert(monitor, adapter_path, advert_err)) {
            notify(progress,
                   "soliciting",
                   "Open Settings > Bluetooth > (i) on the iPhone and enable Show Message Notifications and "
                   "Sync Contacts. They can take a few minutes to appear.");
        } else {
            notify(progress, "warning", advert_err + " Notification permissions may not appear.");
        }

        if (!result.dual_bond) {
            result.message += " The bond covers BR/EDR only, so messages and contacts will work but notification "
                              "mirroring will not.";
            if (result.auth_strategy_used == AuthStrategy::ExplicitPair)
                result.message += " Only the connect-first transaction derives the LE keys, and the iPhone refused "
                                  "it this time. To try for notifications, Forget This Device on the iPhone and "
                                  "pair again.";
        }
        return result;
    }

    bool scan_devices(BluezMonitor& monitor, int seconds, const std::function<void()>& on_tick, std::string& err) {
        GDBusConnection* conn = monitor.connection();
        if (!conn) {
            err = "Bluetooth is unavailable.";
            return false;
        }

        auto objects = monitor.snapshot();
        if (objects.adapters.empty()) {
            err = "No Bluetooth adapter is present.";
            return false;
        }
        const std::string adapter = objects.adapters.front().path;

        // InProgress means BlueZ believes a discovery is already running.
        std::string start_err;
        bool owns_discovery = call_adapter(conn, adapter, "StartDiscovery", &start_err);
        if (!owns_discovery) {
            if (start_err.find("InProgress") == std::string::npos) {
                err = start_err.empty() ? "BlueZ refused to start scanning." : start_err;
                return false;
            }
            if (!get_adapter_bool(conn, adapter, "Discovering")) {
                err = "BlueZ is holding a discovery session that never ended, so scanning cannot start. "
                      "Restart the Bluetooth service (sudo systemctl restart bluetooth) and try again.";
                return false;
            }
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        while (std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (on_tick)
                on_tick();
        }

        // A discovery left running blocks the pairing transaction that usually
        // follows it, so it stops even when the caller loses interest.
        if (owns_discovery)
            call_adapter(conn, adapter, "StopDiscovery");
        return true;
    }

    PairResult unpair_device(BluezMonitor& monitor, const std::string& address) {
        PairResult result;
        result.device_address = normalize_address(address);

        GDBusConnection* conn = monitor.connection();
        if (!conn) {
            result.status = "error";
            result.message = "Bluetooth is unavailable.";
            return result;
        }

        Device device;
        if (!lookup(monitor, result.device_address, device)) {
            result.status = "not_found";
            result.message = "Device " + result.device_address + " is not known to BlueZ.";
            return result;
        }

        const std::string adapter_path = adapter_for(monitor, device);
        GError* error = nullptr;
        GVariant* reply = g_dbus_connection_call_sync(conn,
                                                      BLUEZ_NAME,
                                                      adapter_path.c_str(),
                                                      IFACE_ADAPTER,
                                                      "RemoveDevice",
                                                      g_variant_new("(o)", device.path.c_str()),
                                                      nullptr,
                                                      G_DBUS_CALL_FLAGS_NONE,
                                                      10000,
                                                      nullptr,
                                                      &error);
        if (!reply) {
            result.status = "error";
            result.message = std::string("Could not remove the bond: ") + (error ? error->message : "unknown");
            g_clear_error(&error);
            return result;
        }
        g_variant_unref(reply);

        result.success = true;
        result.status = "unpaired";
        result.message = "Removed the bond. Also delete this computer from the iPhone's Bluetooth settings before "
                         "pairing again — a stale record there will block a clean retry.";
        return result;
    }

} // namespace tether::bluetooth
