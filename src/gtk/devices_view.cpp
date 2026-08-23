#include "devices_view.hpp"
#include "daemon_client.hpp"
#include "ui_util.hpp"
#include <tether/i18n.hpp>

#include <algorithm>
#include <deque>
#include <filesystem>
#include <string>
#include <tether/crypto.hpp>
#include <tether/discovery.hpp>
#include <vector>

namespace tether::ui {

    namespace {

        struct DevicesState {
            std::vector<tether::DiscoveredDevice> discovered_devices;
            std::vector<tether::DiscoveredDevice> pending_pairing_requests;
            std::vector<std::pair<std::string, std::string>> paired_devices; // fp, name
            std::vector<std::string> connected_fps;                          // active connections

            // The Bluetooth route
            std::vector<nlohmann::json> bt_devices;
            nlohmann::json bt_status = nlohmann::json::object();
            nlohmann::json bt_connection = nlohmann::json::object();

            GtkWidget* list_devices = nullptr;
            GtkWidget* right_pane_stack = nullptr;

            std::string selected_device_fp;
            std::string selected_device_name;
            std::string selected_device_ip;
            uint16_t selected_device_port = 5134;

            GtkWidget* lbl_action_name = nullptr;
            GtkWidget* lbl_action_status = nullptr;
            GtkWidget* btn_grid = nullptr;

            GtkWidget* lbl_unpaired_name = nullptr;
            GtkWidget* lbl_unpaired_ip = nullptr;

            std::string selected_bt_address;
            std::string selected_bt_name;

            GtkWidget* lbl_bt_name = nullptr;
            GtkWidget* bt_setup_box = nullptr;
            GtkWidget* lbl_bt_setup_what = nullptr;
            GtkWidget* lbl_bt_setup_command = nullptr;
            std::string bt_setup_commands;
            GtkWidget* lbl_bt_mode = nullptr;
            GtkWidget* lbl_bt_link = nullptr;
            GtkWidget* lbl_bt_messages = nullptr;
            GtkWidget* lbl_bt_contacts = nullptr;
            GtkWidget* lbl_bt_notifications = nullptr;
            GtkWidget* lbl_bt_reason = nullptr;
            GtkWidget* lbl_bt_progress = nullptr;
            GtkWidget* btn_bt_pair = nullptr;
            GtkWidget* btn_bt_solicit = nullptr;
            GtkWidget* chk_bt_enabled = nullptr;
            GtkWidget* chk_bt_ancs = nullptr;
            GtkWidget* chk_bt_content = nullptr;
            GtkWidget* btn_bt_unpair = nullptr;

            GtkWidget* lbl_welcome_wifi = nullptr;
            GtkWidget* lbl_welcome_bt = nullptr;

            // Files waiting to go to the phone, front is in flight.
            GtkWidget* dropzone = nullptr;
            std::deque<std::filesystem::path> send_queue;
            size_t send_batch_total = 0;
            size_t send_failed = 0;
            size_t send_skipped = 0;
            std::string send_last_error;
        };

        DevicesState g_devices;

        void set_status_action(const std::string& text) { set_text(g_devices.lbl_action_status, text); }

        const nlohmann::json* find_bt_device(const std::string& address) {
            for (const auto& device : g_devices.bt_devices) {
                if (device.value("address", "") == address)
                    return &device;
            }
            return nullptr;
        }

        // The daemon supervises one iPhone at a time, so the live profile status
        // describes whichever Bluetooth device is bonded, not the one merely
        // selected in the list.
        bool is_supervised_bt_device(const std::string& address) {
            if (address.empty())
                return false;
            const std::string supervised = g_devices.bt_status.value("device_address", "");
            return !supervised.empty() && supervised == address;
        }

        void set_capability_row(GtkWidget* label, const std::string& title, bool ok, const std::string& note) {
            std::string markup = std::string(ok ? "\u2713" : "\u2717") + "  <b>" + escape_markup(title) + "</b>";
            if (!note.empty())
                markup += "  <span size='small'>" + escape_markup(note) + "</span>";
            set_markup(label, markup);
        }

        void update_welcome_pane() {
            const bool wifi_ok = !g_devices.connected_fps.empty();
            set_text(g_devices.lbl_welcome_wifi, wifi_ok ? _("Connected.") : _("Not connected yet."));

            const bool bt_ok =
                g_devices.bt_connection.value("map_open", false) || g_devices.bt_connection.value("ancs_ready", false);
            std::string bt_note = _("Not connected yet.");
            if (bt_ok)
                bt_note = _("Connected.");
            else if (!g_devices.bt_status.empty() && !g_devices.bt_status.value("available", false))
                bt_note = _("Bluetooth is unavailable on this machine.");
            set_text(g_devices.lbl_welcome_bt, bt_note);
        }

        void on_bt_enabled_toggled(GtkWidget* widget, gpointer);
        void on_bt_ancs_toggled(GtkWidget* widget, gpointer);
        void on_bt_content_toggled(GtkWidget* widget, gpointer);

        void update_bt_pane() {
            const std::string address = g_devices.selected_bt_address;
            const nlohmann::json* device = find_bt_device(address);
            const bool paired = device && device->value("paired", false);
            const bool supervised = is_supervised_bt_device(address);
            const bool available = g_devices.bt_status.value("available", false);
            const bool bt_on = g_devices.bt_status.value("enabled", true);

            set_markup(g_devices.lbl_bt_name, "<b>" + escape_markup(g_devices.selected_bt_name) + "</b>");

            const nlohmann::json capability = g_devices.bt_status.value("capability", nlohmann::json());

            std::string what;
            std::string commands;
            if (capability.is_object() && capability.contains("setup")) {
                int n = 0;
                for (const auto& step : capability["setup"]) {
                    if (!what.empty())
                        what += "\n";
                    what += std::to_string(++n) + ". " + step.value("what", "");
                    if (!commands.empty())
                        commands += "\n";
                    commands += step.value("command", "");
                }
            }
            g_devices.bt_setup_commands = commands;
            set_text(g_devices.lbl_bt_setup_what, what);
            set_text(g_devices.lbl_bt_setup_command, commands);
            gtk_widget_set_visible(g_devices.bt_setup_box, !commands.empty());

            std::string mode;
            if (!g_devices.bt_status.empty()) {
                if (!available || capability.is_null()) {
                    mode = _("Bluetooth is unavailable on this machine.");
                } else if (capability.value("mode", "") == "full") {
                    mode = _("Full mode \u2014 messages, contacts, and notifications.");
                } else if (capability.value("mode", "") == "compatibility") {
                    mode = _("Compatibility mode \u2014 messages and contacts, no notification mirroring.");
                } else {
                    mode = _("This machine cannot carry the Bluetooth features.");
                }
                // The reasons name what is missing (adapter class, LE roles, BlueZ
                // version) and are the only place that detail surfaces outside the CLI.
                if (!capability.is_null() && capability.contains("reasons")) {
                    for (const auto& reason : capability["reasons"]) {
                        if (reason.is_string())
                            mode += "\n" + reason.get<std::string>();
                    }
                }
            }
            set_text(g_devices.lbl_bt_mode, mode);

            // Only the supervised bond has live profile state; anything else can
            // report what BlueZ knows and no more.
            const nlohmann::json& connection = g_devices.bt_connection;
            const bool classic = supervised ? connection.value("classic_connected", false)
                                            : (device && device->value("classic_connected", false));
            const bool le =
                supervised ? connection.value("le_connected", false) : (device && device->value("le_connected", false));
            const bool map_open = supervised && connection.value("map_open", false);
            const bool pbap_open = supervised && connection.value("pbap_open", false);
            const bool ancs_ready = supervised && connection.value("ancs_ready", false);

            set_capability_row(g_devices.lbl_bt_link,
                               _("Link"),
                               classic || le,
                               std::string("BR/EDR ") + (classic ? "on" : "off") + ", LE " + (le ? "on" : "off"));

            const std::string map_error = connection.value("map_error", "none");
            const std::string pbap_error = connection.value("pbap_error", "none");
            set_capability_row(
                g_devices.lbl_bt_messages, _("Messages"), map_open, map_open || map_error == "none" ? "" : map_error);
            set_capability_row(g_devices.lbl_bt_contacts,
                               _("Contacts"),
                               pbap_open,
                               pbap_open || pbap_error == "none" ? "" : pbap_error);
            set_capability_row(g_devices.lbl_bt_notifications,
                               _("Notifications"),
                               ancs_ready,
                               ancs_ready ? "" : connection.value("ancs_reason", ""));

            // The daemon's reasons name the actual next step, so they are shown
            // verbatim rather than replaced with something vaguer.
            std::string reason;
            if (!paired) {
                reason = _("Not paired over Bluetooth yet.");
            } else if (!supervised) {
                reason = _("Tether is not using this device. Pair it to select it.");
            } else if (!bt_on) {
                // Supervision is idle while switched off, so its status would
                // otherwise report no device selected.
                reason = _("Bluetooth is switched off for this iPhone.");
            } else {
                const bool link_degraded = !classic || !le;
                reason = connection.value(link_degraded ? "link_reason" : "profile_reason", "");
                if (reason.empty())
                    reason = connection.value("link_reason", "");
            }
            set_text(g_devices.lbl_bt_reason, reason);

            gtk_widget_set_visible(g_devices.btn_bt_pair, available && (!paired || !supervised));
            gtk_widget_set_visible(g_devices.btn_bt_unpair, available && paired);
            // Re-soliciting only helps when the phone is withholding a profile,
            // not when the link itself is down or nothing is bonded.
            gtk_widget_set_visible(g_devices.btn_bt_solicit,
                                   available && supervised &&
                                       (map_error == "forbidden" || map_error == "no_record" || !ancs_ready));

            const bool ancs_on = g_devices.bt_status.value("ancs_enabled", true);

            gtk_widget_set_visible(g_devices.chk_bt_enabled, available && supervised);
            gtk_widget_set_visible(g_devices.chk_bt_ancs, available && supervised);
            gtk_widget_set_visible(g_devices.chk_bt_content, available && supervised);
            gtk_widget_set_sensitive(g_devices.chk_bt_ancs, bt_on);
            gtk_widget_set_sensitive(g_devices.chk_bt_content, bt_on && ancs_on);

            g_signal_handlers_block_by_func(
                g_devices.chk_bt_enabled, reinterpret_cast<gpointer>(on_bt_enabled_toggled), nullptr);
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_devices.chk_bt_enabled), bt_on);
            g_signal_handlers_unblock_by_func(
                g_devices.chk_bt_enabled, reinterpret_cast<gpointer>(on_bt_enabled_toggled), nullptr);

            g_signal_handlers_block_by_func(
                g_devices.chk_bt_ancs, reinterpret_cast<gpointer>(on_bt_ancs_toggled), nullptr);
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_devices.chk_bt_ancs), ancs_on);
            g_signal_handlers_unblock_by_func(
                g_devices.chk_bt_ancs, reinterpret_cast<gpointer>(on_bt_ancs_toggled), nullptr);

            g_signal_handlers_block_by_func(
                g_devices.chk_bt_content, reinterpret_cast<gpointer>(on_bt_content_toggled), nullptr);
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_devices.chk_bt_content),
                                         g_devices.bt_status.value("ancs_content_enabled", true));
            g_signal_handlers_unblock_by_func(
                g_devices.chk_bt_content, reinterpret_cast<gpointer>(on_bt_content_toggled), nullptr);
        }

        void update_right_pane() {
            if (!g_devices.selected_bt_address.empty()) {
                gtk_stack_set_visible_child_name(GTK_STACK(g_devices.right_pane_stack), "bluetooth");
                update_bt_pane();
                return;
            }

            if (g_devices.selected_device_fp.empty()) {
                update_welcome_pane();
                gtk_stack_set_visible_child_name(GTK_STACK(g_devices.right_pane_stack), "placeholder");
                return;
            }

            bool is_paired = false;
            for (const auto& p : g_devices.paired_devices) {
                if (p.first == g_devices.selected_device_fp)
                    is_paired = true;
            }

            if (is_paired) {
                gtk_stack_set_visible_child_name(GTK_STACK(g_devices.right_pane_stack), "action");
                set_markup(g_devices.lbl_action_name, ("<b>" + escape_markup(g_devices.selected_device_name) + "</b>"));
                bool online = std::find(g_devices.connected_fps.begin(),
                                        g_devices.connected_fps.end(),
                                        g_devices.selected_device_fp) != g_devices.connected_fps.end();
                set_status_action(
                    online ? _("Connected and ready.")
                           : _("Device is offline. Open the Tether app on iPhone while on the same network."));
                if (g_devices.btn_grid) {
                    gtk_widget_set_visible(g_devices.btn_grid, online);
                }
            } else {
                gtk_stack_set_visible_child_name(GTK_STACK(g_devices.right_pane_stack), "pair");
                set_markup(g_devices.lbl_unpaired_name,
                           ("<b>" + escape_markup(g_devices.selected_device_name) + "</b>"));
                // TRANSLATORS: {0} is an IP address, {1} a port number.
                set_text(g_devices.lbl_unpaired_ip,
                         tether::tr_format(
                             _("Found at {0}:{1}"), g_devices.selected_device_ip, g_devices.selected_device_port));
            }
        }

        void on_device_selected(GtkListBox*, GtkListBoxRow* row, gpointer) {
            if (!row)
                return;

            const char* bt_address = (const char*)g_object_get_data(G_OBJECT(row), "bt_address");
            g_devices.selected_bt_address = bt_address ? bt_address : "";
            if (!g_devices.selected_bt_address.empty()) {
                const char* bt_name = (const char*)g_object_get_data(G_OBJECT(row), "name");
                g_devices.selected_bt_name = bt_name ? bt_name : "";
                g_devices.selected_device_fp.clear();
                update_right_pane();
                return;
            }

            const char* fp = (const char*)g_object_get_data(G_OBJECT(row), "fp");
            const char* name = (const char*)g_object_get_data(G_OBJECT(row), "name");
            const char* ip = (const char*)g_object_get_data(G_OBJECT(row), "ip");
            g_devices.selected_device_fp = fp ? fp : "";
            g_devices.selected_device_name = name ? name : "";
            g_devices.selected_device_ip = ip ? ip : "";
            g_devices.selected_device_port = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "port"));
            update_right_pane();
        }

        void on_pair_click(GtkWidget*, gpointer) {
            if (g_devices.selected_device_ip.empty())
                return;
            set_text(g_devices.lbl_unpaired_ip, _("Sending pair request..."));
            nlohmann::json j;
            j["command"] = "pair_request";
            j["host"] = g_devices.selected_device_ip;
            j["port"] = g_devices.selected_device_port;
            daemon_send(j);
            set_status_main(_("Pair request sent. Approve on remote device!"));
        }

        void on_accept_click(GtkWidget*, gpointer) {
            if (g_devices.selected_device_fp.empty())
                return;
            nlohmann::json j;
            j["command"] = "accept_device";
            j["fingerprint"] = g_devices.selected_device_fp;
            daemon_send(j);
        }

        void on_bt_pair_click(GtkWidget*, gpointer) {
            if (g_devices.selected_bt_address.empty())
                return;
            if (!daemon_send({{"command", "bt_pair"}, {"address", g_devices.selected_bt_address}})) {
                set_text(g_devices.lbl_bt_progress, _("Could not reach the Tether daemon."));
                return;
            }
            gtk_widget_set_sensitive(g_devices.btn_bt_pair, FALSE);
            set_text(g_devices.lbl_bt_progress, _("Pairing\u2026 confirm the prompt on the iPhone."));
        }

        void on_bt_unpair_click(GtkWidget*, gpointer) {
            if (g_devices.selected_bt_address.empty())
                return;

            GtkWidget* dialog = gtk_message_dialog_new(GTK_WINDOW(main_window()),
                                                       GTK_DIALOG_MODAL,
                                                       GTK_MESSAGE_QUESTION,
                                                       GTK_BUTTONS_OK_CANCEL,
                                                       _("Remove the Bluetooth pairing with %s?"),
                                                       g_devices.selected_bt_name.c_str());
            gtk_message_dialog_format_secondary_text(
                GTK_MESSAGE_DIALOG(dialog),
                _("Also delete this computer from the iPhone's Bluetooth settings before pairing again, "
                  "or the phone will keep the stale bond."));
            const bool confirmed = gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK;
            gtk_widget_destroy(dialog);
            if (!confirmed)
                return;

            daemon_send({{"command", "bt_unpair"}, {"address", g_devices.selected_bt_address}});
            set_text(g_devices.lbl_bt_progress, _("Removing the pairing\u2026"));
        }

        void on_bt_enabled_toggled(GtkWidget* widget, gpointer) {
            daemon_send({{"command", "bt_set_enabled"},
                         {"enabled", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)) == TRUE}});
        }

        // The advertisement that makes iOS reveal its Messages and Contacts
        // permission toggles expires a few minutes after pairing. Without this the
        // only way back to those toggles is to remove the bond and pair again.
        void on_bt_ancs_toggled(GtkWidget* widget, gpointer) {
            daemon_send({{"command", "bt_set_ancs"},
                         {"enabled", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)) == TRUE}});
        }

        void on_bt_content_toggled(GtkWidget* widget, gpointer) {
            daemon_send({{"command", "bt_set_ancs_content"},
                         {"enabled", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)) == TRUE}});
        }

        void on_bt_setup_copy_click(GtkWidget*, gpointer) {
            gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), g_devices.bt_setup_commands.c_str(), -1);
            set_status_main(_("Setup commands copied to the clipboard."));
        }

        void on_bt_solicit_click(GtkWidget*, gpointer) {
            if (!daemon_send({{"command", "bt_solicit"}})) {
                set_text(g_devices.lbl_bt_progress, _("Could not reach the Tether daemon."));
                return;
            }
            set_text(g_devices.lbl_bt_progress, _("Asking the iPhone to show its Bluetooth permissions\u2026"));
        }

        // The daemon opens a fresh connection per send_file, batch one file at a time
        void start_next_send() {
            if (g_devices.send_queue.empty())
                return;
            const std::filesystem::path& path = g_devices.send_queue.front();
            // TRANSLATORS: {} is a file name.
            std::string status = tether::tr_format(_("Sending {}…"), path.filename().string());
            if (g_devices.send_batch_total > 1) {
                const size_t index = g_devices.send_batch_total - g_devices.send_queue.size() + 1;
                // TRANSLATORS: {0} is the current file's position, {1} the batch size.
                status += " " + tether::tr_format(_("({0} of {1})"), index, g_devices.send_batch_total);
            }
            set_status_action(status);
            nlohmann::json j;
            j["command"] = "send_file";
            j["path"] = path.string();
            daemon_send(j);
        }

        // skipped counts items that came in with the batch but are not sendable
        // files, so the final status can admit they were dropped.
        void queue_files(std::vector<std::filesystem::path> paths, size_t skipped = 0) {
            if (paths.empty())
                return;
            if (!daemon_connected()) {
                set_status_action(_("Daemon unavailable."));
                return;
            }
            const bool idle = g_devices.send_queue.empty();
            if (idle) {
                g_devices.send_batch_total = paths.size();
                g_devices.send_failed = 0;
                g_devices.send_skipped = skipped;
                g_devices.send_last_error.clear();
            } else {
                g_devices.send_batch_total += paths.size();
                g_devices.send_skipped += skipped;
            }
            for (auto& path : paths)
                g_devices.send_queue.push_back(std::move(path));
            if (idle)
                start_next_send();
        }

        void on_choose_file(GtkWidget*, gpointer) {
            GtkWidget* dialog = gtk_file_chooser_dialog_new(_("Send File"),
                                                            GTK_WINDOW(main_window()),
                                                            GTK_FILE_CHOOSER_ACTION_OPEN,
                                                            "_Cancel",
                                                            GTK_RESPONSE_CANCEL,
                                                            "_Send",
                                                            GTK_RESPONSE_ACCEPT,
                                                            nullptr);
            gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(dialog), TRUE);
            std::vector<std::filesystem::path> files;
            if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
                GSList* names = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(dialog));
                for (GSList* node = names; node; node = node->next) {
                    files.emplace_back(static_cast<char*>(node->data));
                    g_free(node->data);
                }
                g_slist_free(names);
            }
            gtk_widget_destroy(dialog);
            queue_files(std::move(files));
        }

        void set_dropzone_active(bool active) {
            if (!g_devices.dropzone)
                return;
            GtkStyleContext* ctx = gtk_widget_get_style_context(g_devices.dropzone);
            if (active)
                gtk_style_context_add_class(ctx, "tether-dropzone-active");
            else
                gtk_style_context_remove_class(ctx, "tether-dropzone-active");
        }

        gboolean on_drop_motion(GtkWidget*, GdkDragContext*, gint, gint, guint, gpointer) {
            set_dropzone_active(true);
            return FALSE; // let GTK_DEST_DEFAULT_MOTION answer the drag
        }

        void on_drop_leave(GtkWidget*, GdkDragContext*, guint, gpointer) { set_dropzone_active(false); }

        void on_drop_received(
            GtkWidget*, GdkDragContext* context, gint, gint, GtkSelectionData* data, guint, guint time, gpointer) {
            // drag-leave is not guaranteed once the drop lands.
            set_dropzone_active(false);

            gchar** uris = gtk_selection_data_get_uris(data);
            if (!uris) {
                gtk_drag_finish(context, FALSE, FALSE, time);
                return;
            }

            std::vector<std::filesystem::path> files;
            size_t skipped = 0;
            for (gchar** uri = uris; *uri; ++uri) {
                // Non-file:// URIs have no local path and cannot be streamed.
                gchar* local = g_filename_from_uri(*uri, nullptr, nullptr);
                if (!local) {
                    ++skipped;
                    continue;
                }
                std::filesystem::path path(local);
                g_free(local);
                std::error_code ec;
                if (std::filesystem::is_directory(path, ec))
                    ++skipped;
                else
                    files.push_back(std::move(path));
            }
            g_strfreev(uris);

            gtk_drag_finish(context, !files.empty(), FALSE, time);
            if (files.empty()) {
                set_status_action(_("Folders can't be sent — drop files instead."));
                return;
            }
            queue_files(std::move(files), skipped);
        }

        void update_wifi_indicator() {
            const bool connected = !g_devices.connected_fps.empty();
            set_route_status(Route::WiFi,
                             connected,
                             connected ? _("Clipboard, files, and OTP are connected.")
                                       : _("No paired iPhone is connected. Open the Tether app on the phone."));
        }

        void apply_state_snapshot(const nlohmann::json& j) {
            g_devices.connected_fps.clear();
            if (j.contains("connected_clients") && j["connected_clients"].is_array()) {
                for (auto& c : j["connected_clients"]) {
                    if (c.value("paired", false)) {
                        g_devices.connected_fps.push_back(c.value("fingerprint", ""));
                    } else {
                        tether::DiscoveredDevice dev;
                        dev.fingerprint = c.value("fingerprint", "");
                        dev.name = c.value("device_name", _("Unknown Device"));
                        dev.addresses.push_back({c.value("address", ""), 5134});
                        g_devices.discovered_devices.push_back(dev);
                    }
                }
            }
            g_devices.pending_pairing_requests.clear();
            if (j.contains("pending_pairs") && j["pending_pairs"].is_array()) {
                for (auto& p : j["pending_pairs"]) {
                    tether::DiscoveredDevice req;
                    req.fingerprint = p.value("fingerprint", "");
                    req.name = p.value("device_name", _("Unknown Device"));
                    g_devices.pending_pairing_requests.push_back(req);
                }
            }
            devices_view_refresh();
            update_wifi_indicator();
        }

    } // namespace

    void devices_view_handle_disconnect() {
        g_devices.send_queue.clear();
        g_devices.send_batch_total = 0;
        g_devices.send_failed = 0;
        g_devices.send_skipped = 0;
        g_devices.send_last_error.clear();
    }

    void devices_view_trigger_discovery() {
        set_status_main(_("Scanning for nearby devices..."));
        // Refresh means both routes: mDNS for Wi-Fi peers, and a real BlueZ discovery for Bluetooth.
        daemon_send({{"command", "discover"}});
        daemon_send({{"command", "bt_status"}});
        daemon_send({{"command", "bt_scan"}});
    }

    // Refresh the device list based on Discovered (unpaired), Paired (offline), Paired (online)
    void devices_view_refresh() {
        clear_list_box(g_devices.list_devices);

        tether::Crypto::instance().init();
        try {
            nlohmann::json known = nlohmann::json::parse(tether::Crypto::instance().get_known_hosts_dump());
            g_devices.paired_devices.clear();
            if (!known.empty()) {
                for (auto& [fingerprint, name_value] : known.items()) {
                    g_devices.paired_devices.push_back(
                        {fingerprint, name_value.is_string() ? name_value.get<std::string>() : _("Unknown Device")});
                }
            }
        } catch (...) {
        }

        // We build a unified list of devices.
        // 1. All Paired Devices
        // 2. Any Discovered Devices not in the Paired list

        auto create_row = [](const std::string& name,
                             const std::string& fp,
                             const std::string& ip,
                             uint16_t port,
                             bool paired,
                             bool online) {
            GtkWidget* row = gtk_list_box_row_new();
            g_object_set_data_full(G_OBJECT(row), "fp", g_strdup(fp.c_str()), g_free);
            g_object_set_data_full(G_OBJECT(row), "name", g_strdup(name.c_str()), g_free);
            g_object_set_data_full(G_OBJECT(row), "ip", g_strdup(ip.c_str()), g_free);
            g_object_set_data(G_OBJECT(row), "port", GINT_TO_POINTER(port));
            g_object_set_data(G_OBJECT(row), "paired", GINT_TO_POINTER(paired ? 1 : 0));

            GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
            gtk_container_set_border_width(GTK_CONTAINER(box), 12);

            const char* icon_name =
                paired ? (online ? "network-wireless-signal-excellent-symbolic" : "network-wireless-offline-symbolic")
                       : "network-wireless-acquiring-symbolic";
            GtkWidget* icon = gtk_image_new_from_icon_name(icon_name, GTK_ICON_SIZE_LARGE_TOOLBAR);
            gtk_box_pack_start(GTK_BOX(box), icon, FALSE, FALSE, 0);

            GtkWidget* labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
            GtkWidget* title = gtk_label_new(nullptr);
            gtk_label_set_markup(GTK_LABEL(title), ("<b>" + escape_markup(name) + "</b>").c_str());
            gtk_label_set_xalign(GTK_LABEL(title), 0.0);
            gtk_box_pack_start(GTK_BOX(labels), title, FALSE, FALSE, 0);

            GtkWidget* subtitle =
                gtk_label_new(paired ? (online ? _("Connected") : _("Offline")) : _("Nearby (Tap to Pair)"));
            gtk_label_set_xalign(GTK_LABEL(subtitle), 0.0);
            gtk_style_context_add_class(gtk_widget_get_style_context(subtitle), "muted");
            gtk_box_pack_start(GTK_BOX(labels), subtitle, FALSE, FALSE, 0);

            gtk_box_pack_start(GTK_BOX(box), labels, TRUE, TRUE, 0);
            gtk_container_add(GTK_CONTAINER(row), box);
            return row;
        };

        auto create_bt_row = [](const nlohmann::json& device) {
            const std::string address = device.value("address", "");
            const std::string name = device.value("name", "").empty() ? address : device.value("name", "");
            const bool paired = device.value("paired", false);
            const bool connected = device.value("connected", false);

            GtkWidget* row = gtk_list_box_row_new();
            g_object_set_data_full(G_OBJECT(row), "bt_address", g_strdup(address.c_str()), g_free);
            g_object_set_data_full(G_OBJECT(row), "name", g_strdup(name.c_str()), g_free);

            GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
            gtk_container_set_border_width(GTK_CONTAINER(box), 12);

            GtkWidget* icon = gtk_image_new_from_icon_name(
                connected ? "bluetooth-active-symbolic" : "bluetooth-symbolic", GTK_ICON_SIZE_LARGE_TOOLBAR);
            gtk_box_pack_start(GTK_BOX(box), icon, FALSE, FALSE, 0);

            GtkWidget* labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
            GtkWidget* title = gtk_label_new(nullptr);
            gtk_label_set_markup(GTK_LABEL(title), ("<b>" + escape_markup(name) + "</b>").c_str());
            gtk_label_set_xalign(GTK_LABEL(title), 0.0);
            gtk_box_pack_start(GTK_BOX(labels), title, FALSE, FALSE, 0);

            GtkWidget* subtitle =
                gtk_label_new(connected ? _("Connected") : (paired ? _("Paired") : _("Nearby (Tap to Pair)")));
            gtk_label_set_xalign(GTK_LABEL(subtitle), 0.0);
            gtk_style_context_add_class(gtk_widget_get_style_context(subtitle), "muted");
            gtk_box_pack_start(GTK_BOX(labels), subtitle, FALSE, FALSE, 0);

            gtk_box_pack_start(GTK_BOX(box), labels, TRUE, TRUE, 0);
            gtk_container_add(GTK_CONTAINER(row), box);
            return row;
        };

        auto create_header_row = [](const std::string& title_text) {
            GtkWidget* row = gtk_list_box_row_new();
            gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
            GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
            gtk_container_set_border_width(GTK_CONTAINER(box), 8);
            GtkWidget* lbl = gtk_label_new(nullptr);
            gtk_label_set_markup(
                GTK_LABEL(lbl),
                ("<b><span size='small' color='gray'>" + escape_markup(title_text) + "</span></b>").c_str());
            gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
            gtk_box_pack_start(GTK_BOX(box), lbl, TRUE, TRUE, 0);
            gtk_container_add(GTK_CONTAINER(row), box);
            return row;
        };

        std::string my_fp = tether::Crypto::instance().get_my_fingerprint();

        std::vector<GtkWidget*> connected_rows;
        std::vector<GtkWidget*> remembered_rows;
        std::vector<GtkWidget*> discovered_rows;

        for (const auto& paired : g_devices.paired_devices) {
            bool online = std::find(g_devices.connected_fps.begin(), g_devices.connected_fps.end(), paired.first) !=
                          g_devices.connected_fps.end();
            GtkWidget* r = create_row(paired.second, paired.first, "", 5134, true, online);
            if (online) {
                connected_rows.push_back(r);
            } else {
                remembered_rows.push_back(r);
            }
        }

        for (const auto& disc : g_devices.discovered_devices) {
            if (disc.fingerprint == my_fp)
                continue;
            bool is_paired = false;
            for (const auto& p : g_devices.paired_devices) {
                if (p.first == disc.fingerprint) {
                    is_paired = true;
                    break;
                }
            }
            if (is_paired)
                continue; // already added above
            std::string ip = disc.addresses.empty() ? "" : disc.addresses[0].address;
            uint16_t port = disc.addresses.empty() ? 5134 : disc.addresses[0].port;
            GtkWidget* r = create_row(disc.name, disc.fingerprint, ip, port, false, false);
            discovered_rows.push_back(r);
        }

        // Render Pending Pair Requests
        for (const auto& req : g_devices.pending_pairing_requests) {
            bool is_paired = false;
            for (const auto& p : g_devices.paired_devices) {
                if (p.first == req.fingerprint) {
                    is_paired = true;
                    break;
                }
            }
            if (is_paired)
                continue; // ignore if they successfully paired

            bool already_shown = false;
            for (const auto& disc : g_devices.discovered_devices) {
                if (disc.fingerprint == req.fingerprint) {
                    already_shown = true;
                    break;
                }
            }
            if (already_shown)
                continue; // ignore if mDNS already populated it

            std::string ip = req.addresses.empty() ? "" : req.addresses[0].address;
            uint16_t port = req.addresses.empty() ? 5134 : req.addresses[0].port;
            GtkWidget* r = create_row(req.name, req.fingerprint, ip, port, false, false);
            discovered_rows.push_back(r);
        }

        if (!connected_rows.empty() || !remembered_rows.empty() || !discovered_rows.empty()) {
            gtk_list_box_insert(GTK_LIST_BOX(g_devices.list_devices), create_header_row("WI-FI"), -1);
            for (const auto* rows : {&connected_rows, &remembered_rows, &discovered_rows})
                for (auto* r : *rows)
                    gtk_list_box_insert(GTK_LIST_BOX(g_devices.list_devices), r, -1);
        }

        std::vector<GtkWidget*> bt_rows;
        for (const auto& device : g_devices.bt_devices) {
            if (!device.value("iphone", false) && !is_supervised_bt_device(device.value("address", "")))
                continue;
            bt_rows.push_back(create_bt_row(device));
        }

        if (!bt_rows.empty()) {
            gtk_list_box_insert(GTK_LIST_BOX(g_devices.list_devices), create_header_row("BLUETOOTH"), -1);
            for (auto* r : bt_rows)
                gtk_list_box_insert(GTK_LIST_BOX(g_devices.list_devices), r, -1);
        }

        gtk_widget_show_all(g_devices.list_devices);
    }

    bool devices_view_handle_event(const nlohmann::json& event) {
        const std::string command = event.value("command", "");
        if (command == "state_snapshot") {
            apply_state_snapshot(event);
            return true;
        }
        if (command == "client_connected") {
            std::string fp = event.value("fingerprint", "");
            if (std::find(g_devices.connected_fps.begin(), g_devices.connected_fps.end(), fp) ==
                g_devices.connected_fps.end()) {
                g_devices.connected_fps.push_back(fp);
            }
            devices_view_refresh();
            update_right_pane();
            update_wifi_indicator();
            return true;
        }
        if (command == "client_disconnected") {
            std::string fp = event.value("fingerprint", "");
            g_devices.connected_fps.erase(
                std::remove(g_devices.connected_fps.begin(), g_devices.connected_fps.end(), fp),
                g_devices.connected_fps.end());
            devices_view_refresh();
            update_right_pane();
            update_wifi_indicator();
            return true;
        }
        if (command == "pair_request_received" || command == "untrusted_client_connected") {
            tether::DiscoveredDevice req;
            req.fingerprint = event.value("fingerprint", "");

            std::string resolved_name = event.value("device_name", _("Unknown Device"));
            if (resolved_name == _("Unknown Device")) {
                for (const auto& d : g_devices.discovered_devices) {
                    if (d.fingerprint == req.fingerprint && !d.name.empty()) {
                        resolved_name = d.name;
                        break;
                    }
                }
            }
            req.name = resolved_name;
            req.addresses.push_back({event.value("address", ""), 5134});
            bool exists = false;
            for (const auto& dev : g_devices.pending_pairing_requests) {
                if (dev.fingerprint == req.fingerprint) {
                    exists = true;
                    break;
                }
            }
            if (!exists) {
                g_devices.pending_pairing_requests.push_back(req);
                devices_view_refresh();
            }
            return true;
        }
        if (command == "pair_accepted") {
            std::string fp = event.value("fingerprint", "");
            std::string name = event.value("device_name", "");
            if (!fp.empty()) {
                g_devices.connected_fps.push_back(fp);

                // Ensure it's in the paired devices list so the UI transitions
                bool already_paired = false;
                for (const auto& p : g_devices.paired_devices) {
                    if (p.first == fp) {
                        already_paired = true;
                        break;
                    }
                }
                if (!already_paired)
                    g_devices.paired_devices.push_back({fp, name});

                g_devices.pending_pairing_requests.erase(
                    std::remove_if(g_devices.pending_pairing_requests.begin(),
                                   g_devices.pending_pairing_requests.end(),
                                   [&](const tether::DiscoveredDevice& d) { return d.fingerprint == fp; }),
                    g_devices.pending_pairing_requests.end());
                devices_view_refresh();
                update_right_pane();
            }
            return true;
        }
        if (command == "discovery_result") {
            g_devices.discovered_devices.clear();
            if (event.contains("devices") && event["devices"].is_array()) {
                for (const auto& d : event["devices"]) {
                    tether::DiscoveredDevice dev;
                    dev.name = d.value("name", "");
                    dev.fingerprint = d.value("fingerprint", "");
                    if (d.contains("addresses") && d["addresses"].is_array()) {
                        for (const auto& a : d["addresses"]) {
                            dev.addresses.push_back({a.value("address", ""), a.value<uint16_t>("port", 5134)});
                        }
                    }
                    g_devices.discovered_devices.push_back(dev);
                }
            }
            devices_view_refresh();
            set_status_main(_("Ready"));
            return true;
        }
        if (command == "file_send_complete") {
            if (!event.value("success", true)) {
                ++g_devices.send_failed;
                g_devices.send_last_error = event.value("message", "");
            }
            if (!g_devices.send_queue.empty())
                g_devices.send_queue.pop_front();
            if (!g_devices.send_queue.empty()) {
                start_next_send();
                return true;
            }

            // The daemon's own message is exact for a single file; a batch needs
            // a tally, otherwise a failure halfway through is never seen.
            std::string message = event.value("message", "");
            if (g_devices.send_batch_total > 1) {
                const size_t sent = g_devices.send_batch_total - g_devices.send_failed;
                // TRANSLATORS: {0} is how many were sent, {1} the batch size.
                message =
                    tether::tr_format(P_("Sent {0} of {1} file.", "Sent {0} of {1} files.", g_devices.send_batch_total),
                                      sent,
                                      g_devices.send_batch_total);
                if (g_devices.send_failed > 0 && !g_devices.send_last_error.empty())
                    message += " " + g_devices.send_last_error;
            }
            if (g_devices.send_skipped > 0)
                message +=
                    " " + tether::tr_format(
                              P_("Skipped {} non-file item.", "Skipped {} non-file items.", g_devices.send_skipped),
                              g_devices.send_skipped);
            set_status_action(message);

            g_devices.send_batch_total = 0;
            g_devices.send_failed = 0;
            g_devices.send_skipped = 0;
            g_devices.send_last_error.clear();
            return true;
        }
        if (command == "clipboard_content") {
            set_status_action(_("Desktop Clipboard Sync triggered."));
            return true;
        }
        if (command == "bt_status") {
            g_devices.bt_status = event;
            devices_view_refresh();
            update_right_pane();
            return true;
        }
        if (command == "bt_devices") {
            g_devices.bt_devices.clear();
            if (event.contains("devices") && event["devices"].is_array()) {
                for (const auto& device : event["devices"])
                    g_devices.bt_devices.push_back(device);
            }
            devices_view_refresh();
            update_right_pane();
            return true;
        }
        if (command == "bt_connection_changed") {
            g_devices.bt_connection = event;
            const bool connected = event.value("map_open", false) || event.value("ancs_ready", false);
            std::string detail = event.value("profile_reason", "");
            if (detail.empty())
                detail = event.value("link_reason", "");
            if (detail.empty() && connected)
                detail = _("Messages and notifications are connected.");
            set_route_status(Route::Bluetooth, connected, detail);
            update_right_pane();
            // A status broadcast, not a command this view owns: the Messages and
            // Notifications views need the same event.
            return false;
        }
        if (command == "bt_scan_result") {
            const std::string message = event.value("message", "");
            set_status_main(message);
            set_text(g_devices.lbl_bt_progress, message);
            // A failed scan already carries the reason; only a scan that really
            // ran and found nothing wants the "check the phone" advice.
            if (event.value("success", false) && g_devices.bt_devices.empty())
                set_text(g_devices.lbl_welcome_bt,
                         _("No iPhone found. Unlock the phone and open Settings > Bluetooth so it advertises, "
                           "then scan again."));
            else if (!event.value("success", false))
                set_text(g_devices.lbl_welcome_bt, message);
            return true;
        }
        if (command == "bt_solicit_result") {
            set_text(g_devices.lbl_bt_progress, event.value("message", ""));
            return false;
        }
        if (command == "bt_pair_progress") {
            set_text(g_devices.lbl_bt_progress, event.value("step", "") + "  " + event.value("detail", ""));
            return true;
        }
        if (command == "bt_pair_result" || command == "bt_unpair_result") {
            gtk_widget_set_sensitive(g_devices.btn_bt_pair, TRUE);
            set_text(g_devices.lbl_bt_progress, event.value("message", ""));
            // The bond and the supervised device both just changed.
            daemon_send({{"command", "bt_status"}});
            daemon_send({{"command", "bt_list_devices"}});
            return true;
        }
        return false;
    }

    GtkWidget* devices_view_new() {
        GtkWidget* paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);

        // Left Pane (List)
        GtkWidget* left_scroll = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(left_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_widget_set_size_request(left_scroll, 220, -1);

        g_devices.list_devices = gtk_list_box_new();
        g_signal_connect(g_devices.list_devices, "row-selected", G_CALLBACK(on_device_selected), nullptr);
        gtk_container_add(GTK_CONTAINER(left_scroll), g_devices.list_devices);
        gtk_paned_pack1(GTK_PANED(paned), left_scroll, FALSE, FALSE);

        // Right Pane
        g_devices.right_pane_stack = gtk_stack_new();
        gtk_stack_set_transition_type(GTK_STACK(g_devices.right_pane_stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);

        // Placeholder, which on a machine with nothing paired is the first thing
        // anyone sees: it explains both routes rather than saying "select a device".
        GtkWidget* placeholder = gtk_box_new(GTK_ORIENTATION_VERTICAL, 24);
        gtk_container_set_border_width(GTK_CONTAINER(placeholder), 24);
        gtk_widget_set_valign(placeholder, GTK_ALIGN_CENTER);

        GtkWidget* welcome_title = gtk_label_new(nullptr);
        set_markup(welcome_title, "<big><b>" + escape_markup(_("Connect your iPhone")) + "</b></big>");
        gtk_box_pack_start(GTK_BOX(placeholder), welcome_title, FALSE, FALSE, 0);

        // A plain box cannot reflow, and the two blocks do not both fit in a narrow
        // window. A flow box drops to a single column instead of clipping.
        GtkWidget* routes = gtk_flow_box_new();
        gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(routes), GTK_SELECTION_NONE);
        gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(routes), 1);
        gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(routes), 2);
        gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(routes), 24);
        gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(routes), 16);
        gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(routes), TRUE);
        gtk_widget_set_halign(routes, GTK_ALIGN_CENTER);

        auto build_route_block =
            [](const char* icon_name, const char* title, const char* steps, GtkWidget** status_out) {
                GtkWidget* block = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
                gtk_widget_set_valign(block, GTK_ALIGN_START);

                GtkWidget* heading = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
                gtk_box_pack_start(
                    GTK_BOX(heading), gtk_image_new_from_icon_name(icon_name, GTK_ICON_SIZE_BUTTON), FALSE, FALSE, 0);
                GtkWidget* heading_label = gtk_label_new(nullptr);
                gtk_label_set_markup(GTK_LABEL(heading_label), title);
                gtk_label_set_xalign(GTK_LABEL(heading_label), 0.0);
                gtk_box_pack_start(GTK_BOX(heading), heading_label, FALSE, FALSE, 0);
                gtk_box_pack_start(GTK_BOX(block), heading, FALSE, FALSE, 0);

                GtkWidget* steps_label = gtk_label_new(steps);
                gtk_label_set_xalign(GTK_LABEL(steps_label), 0.0);
                gtk_label_set_line_wrap(GTK_LABEL(steps_label), TRUE);
                gtk_label_set_max_width_chars(GTK_LABEL(steps_label), 28);
                gtk_box_pack_start(GTK_BOX(block), steps_label, FALSE, FALSE, 0);

                *status_out = gtk_label_new("");
                gtk_label_set_xalign(GTK_LABEL(*status_out), 0.0);
                gtk_label_set_line_wrap(GTK_LABEL(*status_out), TRUE);
                gtk_label_set_max_width_chars(GTK_LABEL(*status_out), 30);
                gtk_style_context_add_class(gtk_widget_get_style_context(*status_out), "muted");
                gtk_box_pack_start(GTK_BOX(block), *status_out, FALSE, FALSE, 0);
                return block;
            };

        gtk_container_add(GTK_CONTAINER(routes),
                          build_route_block("network-wireless-signal-excellent-symbolic",
                                            ("<b>Wi-Fi</b>\n" + escape_markup(_("clipboard, files, OTP"))).c_str(),
                                            _("1.  Open the Tether app on the iPhone\n"
                                              "2.  It finds this computer by itself\n"
                                              "3.  Approve on both ends"),
                                            &g_devices.lbl_welcome_wifi));

        gtk_container_add(
            GTK_CONTAINER(routes),
            build_route_block("bluetooth-active-symbolic",
                              ("<b>Bluetooth</b>\n" + escape_markup(_("messages, notifications"))).c_str(),
                              _("1.  Pick the iPhone under BLUETOOTH\n"
                                "2.  Press Pair over Bluetooth\n"
                                "3.  Grant the two toggles on the phone"),
                              &g_devices.lbl_welcome_bt));

        gtk_box_pack_start(GTK_BOX(placeholder), routes, FALSE, FALSE, 0);

        // One button for both routes: scanning is what refreshes either list.
        GtkWidget* welcome_scan = gtk_button_new_with_label(_("Scan for devices"));
        gtk_widget_set_halign(welcome_scan, GTK_ALIGN_CENTER);
        g_signal_connect(welcome_scan,
                         "clicked",
                         G_CALLBACK(+[](GtkWidget*, gpointer) { devices_view_trigger_discovery(); }),
                         nullptr);
        gtk_box_pack_start(GTK_BOX(placeholder), welcome_scan, FALSE, FALSE, 0);
        gtk_stack_add_named(GTK_STACK(g_devices.right_pane_stack), placeholder, "placeholder");

        // Bluetooth
        GtkWidget* bt_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
        gtk_container_set_border_width(GTK_CONTAINER(bt_box), 32);
        gtk_widget_set_valign(bt_box, GTK_ALIGN_CENTER);

        g_devices.lbl_bt_name = gtk_label_new(nullptr);
        gtk_label_set_xalign(GTK_LABEL(g_devices.lbl_bt_name), 0.0);
        gtk_box_pack_start(GTK_BOX(bt_box), g_devices.lbl_bt_name, FALSE, FALSE, 0);

        // One-time system setup, shown only while something is actually missing.
        g_devices.bt_setup_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        gtk_style_context_add_class(gtk_widget_get_style_context(g_devices.bt_setup_box), "tether-setup");
        gtk_box_pack_start(GTK_BOX(bt_box), g_devices.bt_setup_box, FALSE, FALSE, 0);

        GtkWidget* lbl_setup_title = gtk_label_new(nullptr);
        gtk_label_set_xalign(GTK_LABEL(lbl_setup_title), 0.0);
        set_markup(lbl_setup_title, "<b>" + escape_markup(_("Bluetooth needs one-time system setup")) + "</b>");
        gtk_box_pack_start(GTK_BOX(g_devices.bt_setup_box), lbl_setup_title, FALSE, FALSE, 0);

        g_devices.lbl_bt_setup_what = gtk_label_new(nullptr);
        gtk_label_set_xalign(GTK_LABEL(g_devices.lbl_bt_setup_what), 0.0);
        gtk_label_set_line_wrap(GTK_LABEL(g_devices.lbl_bt_setup_what), TRUE);
        gtk_box_pack_start(GTK_BOX(g_devices.bt_setup_box), g_devices.lbl_bt_setup_what, FALSE, FALSE, 0);

        // Selectable so the command can be copied by hand as well as by the button.
        g_devices.lbl_bt_setup_command = gtk_label_new(nullptr);
        gtk_label_set_xalign(GTK_LABEL(g_devices.lbl_bt_setup_command), 0.0);
        gtk_label_set_selectable(GTK_LABEL(g_devices.lbl_bt_setup_command), TRUE);
        gtk_label_set_line_wrap(GTK_LABEL(g_devices.lbl_bt_setup_command), TRUE);
        gtk_style_context_add_class(gtk_widget_get_style_context(g_devices.lbl_bt_setup_command),
                                    "tether-setup-command");
        gtk_box_pack_start(GTK_BOX(g_devices.bt_setup_box), g_devices.lbl_bt_setup_command, FALSE, FALSE, 0);

        GtkWidget* setup_actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget* btn_setup_copy = gtk_button_new_with_label(_("Copy commands"));
        gtk_widget_set_tooltip_text(btn_setup_copy,
                                    _("Run these in a terminal. Tether does not change your Bluetooth "
                                      "adapter or bluetoothd on its own."));
        g_signal_connect(btn_setup_copy, "clicked", G_CALLBACK(on_bt_setup_copy_click), nullptr);
        gtk_box_pack_start(GTK_BOX(setup_actions), btn_setup_copy, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(g_devices.bt_setup_box), setup_actions, FALSE, FALSE, 0);

        g_devices.lbl_bt_mode = gtk_label_new(nullptr);
        gtk_label_set_xalign(GTK_LABEL(g_devices.lbl_bt_mode), 0.0);
        gtk_label_set_line_wrap(GTK_LABEL(g_devices.lbl_bt_mode), TRUE);
        gtk_style_context_add_class(gtk_widget_get_style_context(g_devices.lbl_bt_mode), "muted");
        gtk_box_pack_start(GTK_BOX(bt_box), g_devices.lbl_bt_mode, FALSE, FALSE, 0);

        GtkWidget* capabilities = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        for (GtkWidget** label : {&g_devices.lbl_bt_link,
                                  &g_devices.lbl_bt_messages,
                                  &g_devices.lbl_bt_contacts,
                                  &g_devices.lbl_bt_notifications}) {
            *label = gtk_label_new(nullptr);
            gtk_label_set_xalign(GTK_LABEL(*label), 0.0);
            gtk_label_set_line_wrap(GTK_LABEL(*label), TRUE);
            gtk_box_pack_start(GTK_BOX(capabilities), *label, FALSE, FALSE, 0);
        }
        gtk_box_pack_start(GTK_BOX(bt_box), capabilities, FALSE, FALSE, 0);

        g_devices.chk_bt_enabled = gtk_check_button_new_with_label(_("Connect to this iPhone over Bluetooth"));
        gtk_widget_set_tooltip_text(g_devices.chk_bt_enabled,
                                    _("Keep the Bluetooth link to the iPhone up, reconnecting whenever it drops. "
                                      "Turning this off stops Tether reconnecting; a link that is already up stays "
                                      "up until you disconnect it."));
        g_signal_connect(g_devices.chk_bt_enabled, "toggled", G_CALLBACK(on_bt_enabled_toggled), nullptr);
        gtk_box_pack_start(GTK_BOX(bt_box), g_devices.chk_bt_enabled, FALSE, FALSE, 0);

        // pairing that produced no LE half latches mirroring off, and this is the only way back to it without the CLI.
        g_devices.chk_bt_ancs = gtk_check_button_new_with_label(_("Mirror iPhone notifications"));
        gtk_widget_set_tooltip_text(g_devices.chk_bt_ancs,
                                    _("Show notifications from the iPhone on this desktop. Turned off "
                                      "automatically when pairing produces no LE bond; turn it back on after "
                                      "re-pairing."));
        g_signal_connect(g_devices.chk_bt_ancs, "toggled", G_CALLBACK(on_bt_ancs_toggled), nullptr);
        gtk_box_pack_start(GTK_BOX(bt_box), g_devices.chk_bt_ancs, FALSE, FALSE, 0);

        g_devices.chk_bt_content = gtk_check_button_new_with_label(_("Show notification contents"));
        gtk_widget_set_tooltip_text(g_devices.chk_bt_content,
                                    _("Ask the iPhone for each notification's title and message text, not just "
                                      "which app sent it. The phone's own Show Message Notifications toggle still "
                                      "has to be on."));
        g_signal_connect(g_devices.chk_bt_content, "toggled", G_CALLBACK(on_bt_content_toggled), nullptr);
        gtk_box_pack_start(GTK_BOX(bt_box), g_devices.chk_bt_content, FALSE, FALSE, 0);

        g_devices.lbl_bt_reason = gtk_label_new(nullptr);
        gtk_label_set_xalign(GTK_LABEL(g_devices.lbl_bt_reason), 0.0);
        gtk_label_set_line_wrap(GTK_LABEL(g_devices.lbl_bt_reason), TRUE);
        gtk_style_context_add_class(gtk_widget_get_style_context(g_devices.lbl_bt_reason), "muted");
        gtk_box_pack_start(GTK_BOX(bt_box), g_devices.lbl_bt_reason, FALSE, FALSE, 0);

        GtkWidget* bt_buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        g_devices.btn_bt_pair = gtk_button_new_with_label(_("Pair over Bluetooth"));
        gtk_style_context_add_class(gtk_widget_get_style_context(g_devices.btn_bt_pair), "suggested-action");
        g_signal_connect(g_devices.btn_bt_pair, "clicked", G_CALLBACK(on_bt_pair_click), nullptr);
        gtk_box_pack_start(GTK_BOX(bt_buttons), g_devices.btn_bt_pair, FALSE, FALSE, 0);

        g_devices.btn_bt_solicit = gtk_button_new_with_label(_("Show iPhone Permissions"));
        gtk_widget_set_tooltip_text(g_devices.btn_bt_solicit,
                                    _("Re-advertise so the iPhone shows its Show Message Notifications and "
                                      "Sync Contacts toggles under Settings > Bluetooth > (i)."));
        g_signal_connect(g_devices.btn_bt_solicit, "clicked", G_CALLBACK(on_bt_solicit_click), nullptr);
        gtk_box_pack_start(GTK_BOX(bt_buttons), g_devices.btn_bt_solicit, FALSE, FALSE, 0);

        g_devices.btn_bt_unpair = gtk_button_new_with_label(_("Unpair"));
        g_signal_connect(g_devices.btn_bt_unpair, "clicked", G_CALLBACK(on_bt_unpair_click), nullptr);
        gtk_box_pack_start(GTK_BOX(bt_buttons), g_devices.btn_bt_unpair, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(bt_box), bt_buttons, FALSE, FALSE, 0);

        g_devices.lbl_bt_progress = gtk_label_new(nullptr);
        gtk_label_set_xalign(GTK_LABEL(g_devices.lbl_bt_progress), 0.0);
        gtk_label_set_line_wrap(GTK_LABEL(g_devices.lbl_bt_progress), TRUE);
        gtk_style_context_add_class(gtk_widget_get_style_context(g_devices.lbl_bt_progress), "muted");
        gtk_box_pack_start(GTK_BOX(bt_box), g_devices.lbl_bt_progress, FALSE, FALSE, 0);

        gtk_stack_add_named(GTK_STACK(g_devices.right_pane_stack), bt_box, "bluetooth");

        // Pair
        GtkWidget* pair_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
        gtk_widget_set_valign(pair_box, GTK_ALIGN_CENTER);
        gtk_widget_set_halign(pair_box, GTK_ALIGN_CENTER);
        g_devices.lbl_unpaired_name = gtk_label_new(nullptr);
        gtk_box_pack_start(GTK_BOX(pair_box), g_devices.lbl_unpaired_name, FALSE, FALSE, 0);
        g_devices.lbl_unpaired_ip = gtk_label_new(nullptr);
        gtk_style_context_add_class(gtk_widget_get_style_context(g_devices.lbl_unpaired_ip), "muted");
        gtk_box_pack_start(GTK_BOX(pair_box), g_devices.lbl_unpaired_ip, FALSE, FALSE, 0);

        GtkWidget* pair_btn = gtk_button_new_with_label(_("Pair Device"));
        gtk_style_context_add_class(gtk_widget_get_style_context(pair_btn), "suggested-action");
        g_signal_connect(pair_btn, "clicked", G_CALLBACK(on_pair_click), nullptr);
        gtk_box_pack_start(GTK_BOX(pair_box), pair_btn, FALSE, FALSE, 0);

        // optional accept override button
        GtkWidget* accept_btn = gtk_button_new_with_label(_("Force Trust (Accept Pending)"));
        g_signal_connect(accept_btn, "clicked", G_CALLBACK(on_accept_click), nullptr);
        gtk_box_pack_start(GTK_BOX(pair_box), accept_btn, FALSE, FALSE, 0);

        gtk_stack_add_named(GTK_STACK(g_devices.right_pane_stack), pair_box, "pair");

        // Action
        GtkWidget* action_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 24);
        gtk_container_set_border_width(GTK_CONTAINER(action_box), 32);
        gtk_widget_set_valign(action_box, GTK_ALIGN_CENTER);

        g_devices.lbl_action_name = gtk_label_new(nullptr);
        gtk_widget_set_halign(g_devices.lbl_action_name, GTK_ALIGN_CENTER);
        gtk_box_pack_start(GTK_BOX(action_box), g_devices.lbl_action_name, FALSE, FALSE, 0);

        GtkWidget* btn_grid = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
        gtk_widget_set_halign(btn_grid, GTK_ALIGN_CENTER);
        g_devices.btn_grid = btn_grid;

        // drop zone
        GtkWidget* dropzone = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
        gtk_style_context_add_class(gtk_widget_get_style_context(dropzone), "tether-dropzone");
        g_devices.dropzone = dropzone;
        gtk_box_pack_start(GTK_BOX(btn_grid), dropzone, FALSE, FALSE, 0);

        GtkWidget* lbl_drop = gtk_label_new(_("Drop files here to send"));
        gtk_style_context_add_class(gtk_widget_get_style_context(lbl_drop), "muted");
        gtk_box_pack_start(GTK_BOX(dropzone), lbl_drop, FALSE, FALSE, 0);

        GtkWidget* btn_send_file = gtk_button_new_with_label(_("Send File"));
        gtk_style_context_add_class(gtk_widget_get_style_context(btn_send_file), "suggested-action");
        g_signal_connect(btn_send_file, "clicked", G_CALLBACK(on_choose_file), nullptr);
        gtk_box_pack_start(GTK_BOX(dropzone), btn_send_file, FALSE, FALSE, 0);

        GtkWidget* btn_send_clip = gtk_button_new_with_label(_("Send Clipboard"));
        g_signal_connect(btn_send_clip,
                         "clicked",
                         G_CALLBACK(+[](GtkWidget*, gpointer) {
                             nlohmann::json j;
                             j["command"] = "clipboard_set"; // triggers clipboard send
                             daemon_send(j);
                             set_status_action(_("Clipboard sync requested..."));
                         }),
                         nullptr);
        gtk_box_pack_start(GTK_BOX(btn_grid), btn_send_clip, FALSE, FALSE, 0);

        gtk_box_pack_start(GTK_BOX(action_box), btn_grid, FALSE, FALSE, 0);

        g_devices.lbl_action_status = gtk_label_new(_("Ready"));
        gtk_widget_set_halign(g_devices.lbl_action_status, GTK_ALIGN_CENTER);
        gtk_style_context_add_class(gtk_widget_get_style_context(g_devices.lbl_action_status), "muted");
        gtk_box_pack_start(GTK_BOX(action_box), g_devices.lbl_action_status, FALSE, FALSE, 0);

        GtkWidget* action_page = gtk_event_box_new();
        gtk_container_add(GTK_CONTAINER(action_page), action_box);

        static const GtkTargetEntry kUriTargets[] = {{const_cast<gchar*>("text/uri-list"), 0, 0}};

        gtk_drag_dest_set(action_page,
                          static_cast<GtkDestDefaults>(GTK_DEST_DEFAULT_MOTION | GTK_DEST_DEFAULT_DROP),
                          kUriTargets,
                          G_N_ELEMENTS(kUriTargets),
                          GDK_ACTION_COPY);
        g_signal_connect(action_page, "drag-motion", G_CALLBACK(on_drop_motion), nullptr);
        g_signal_connect(action_page, "drag-leave", G_CALLBACK(on_drop_leave), nullptr);
        g_signal_connect(action_page, "drag-data-received", G_CALLBACK(on_drop_received), nullptr);

        gtk_stack_add_named(GTK_STACK(g_devices.right_pane_stack), action_page, "action");

        gtk_paned_pack2(GTK_PANED(paned), g_devices.right_pane_stack, TRUE, FALSE);
        gtk_stack_set_visible_child_name(GTK_STACK(g_devices.right_pane_stack), "placeholder");

        return paned;
    }

} // namespace tether::ui
