#include "devices_view.hpp"
#include "daemon_client.hpp"
#include "ui_util.hpp"

#include <algorithm>
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
            GtkWidget* lbl_bt_mode = nullptr;
            GtkWidget* lbl_bt_link = nullptr;
            GtkWidget* lbl_bt_messages = nullptr;
            GtkWidget* lbl_bt_contacts = nullptr;
            GtkWidget* lbl_bt_notifications = nullptr;
            GtkWidget* lbl_bt_reason = nullptr;
            GtkWidget* lbl_bt_progress = nullptr;
            GtkWidget* btn_bt_pair = nullptr;
            GtkWidget* btn_bt_solicit = nullptr;
            GtkWidget* btn_bt_unpair = nullptr;

            GtkWidget* lbl_welcome_wifi = nullptr;
            GtkWidget* lbl_welcome_bt = nullptr;
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
            set_text(g_devices.lbl_welcome_wifi, wifi_ok ? "Connected." : "Not connected yet.");

            const bool bt_ok =
                g_devices.bt_connection.value("map_open", false) || g_devices.bt_connection.value("ancs_ready", false);
            std::string bt_note = "Not connected yet.";
            if (bt_ok)
                bt_note = "Connected.";
            else if (!g_devices.bt_status.empty() && !g_devices.bt_status.value("available", false))
                bt_note = "Bluetooth is unavailable on this machine.";
            set_text(g_devices.lbl_welcome_bt, bt_note);
        }

        void update_bt_pane() {
            const std::string address = g_devices.selected_bt_address;
            const nlohmann::json* device = find_bt_device(address);
            const bool paired = device && device->value("paired", false);
            const bool supervised = is_supervised_bt_device(address);
            const bool available = g_devices.bt_status.value("available", false);

            set_markup(g_devices.lbl_bt_name, "<b>" + escape_markup(g_devices.selected_bt_name) + "</b>");

            std::string mode;
            if (!g_devices.bt_status.empty()) {
                const nlohmann::json capability = g_devices.bt_status.value("capability", nlohmann::json());
                if (!available || capability.is_null()) {
                    mode = "Bluetooth is unavailable on this machine.";
                } else if (capability.value("mode", "") == "full") {
                    mode = "Full mode \u2014 messages, contacts, and notifications.";
                } else if (capability.value("mode", "") == "compatibility") {
                    mode = "Compatibility mode \u2014 messages and contacts, no notification mirroring.";
                } else {
                    mode = "This machine cannot carry the Bluetooth features.";
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
                               "Link",
                               classic || le,
                               std::string("BR/EDR ") + (classic ? "on" : "off") + ", LE " + (le ? "on" : "off"));

            const std::string map_error = connection.value("map_error", "none");
            const std::string pbap_error = connection.value("pbap_error", "none");
            set_capability_row(
                g_devices.lbl_bt_messages, "Messages", map_open, map_open || map_error == "none" ? "" : map_error);
            set_capability_row(
                g_devices.lbl_bt_contacts, "Contacts", pbap_open, pbap_open || pbap_error == "none" ? "" : pbap_error);
            set_capability_row(g_devices.lbl_bt_notifications,
                               "Notifications",
                               ancs_ready,
                               ancs_ready ? "" : connection.value("ancs_reason", ""));

            // The daemon's reasons name the actual next step, so they are shown
            // verbatim rather than replaced with something vaguer.
            std::string reason;
            if (!paired) {
                reason = "Not paired over Bluetooth yet.";
            } else if (!supervised) {
                reason = "Tether is not using this device. Pair it to select it.";
            } else {
                reason = connection.value("profile_reason", "");
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
                set_status_action(online ? "Connected and ready."
                                         : "Device is offline. Pair again or wait for network.");
                if (g_devices.btn_grid) {
                    gtk_widget_set_visible(g_devices.btn_grid, online);
                }
            } else {
                gtk_stack_set_visible_child_name(GTK_STACK(g_devices.right_pane_stack), "pair");
                set_markup(g_devices.lbl_unpaired_name,
                           ("<b>" + escape_markup(g_devices.selected_device_name) + "</b>"));
                set_text(g_devices.lbl_unpaired_ip,
                         ("Found at " + g_devices.selected_device_ip + ":" +
                          std::to_string(g_devices.selected_device_port)));
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
            set_text(g_devices.lbl_unpaired_ip, "Sending pair request...");
            nlohmann::json j;
            j["command"] = "pair_request";
            j["host"] = g_devices.selected_device_ip;
            j["port"] = g_devices.selected_device_port;
            daemon_send(j);
            set_status_main("Pair request sent. Approve on remote device!");
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
                set_text(g_devices.lbl_bt_progress, "Could not reach the Tether daemon.");
                return;
            }
            gtk_widget_set_sensitive(g_devices.btn_bt_pair, FALSE);
            set_text(g_devices.lbl_bt_progress, "Pairing\u2026 confirm the prompt on the iPhone.");
        }

        void on_bt_unpair_click(GtkWidget*, gpointer) {
            if (g_devices.selected_bt_address.empty())
                return;

            GtkWidget* dialog = gtk_message_dialog_new(GTK_WINDOW(main_window()),
                                                       GTK_DIALOG_MODAL,
                                                       GTK_MESSAGE_QUESTION,
                                                       GTK_BUTTONS_OK_CANCEL,
                                                       "Remove the Bluetooth pairing with %s?",
                                                       g_devices.selected_bt_name.c_str());
            gtk_message_dialog_format_secondary_text(
                GTK_MESSAGE_DIALOG(dialog),
                "Also delete this computer from the iPhone's Bluetooth settings before pairing again, "
                "or the phone will keep the stale bond.");
            const bool confirmed = gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK;
            gtk_widget_destroy(dialog);
            if (!confirmed)
                return;

            daemon_send({{"command", "bt_unpair"}, {"address", g_devices.selected_bt_address}});
            set_text(g_devices.lbl_bt_progress, "Removing the pairing\u2026");
        }

        // The advertisement that makes iOS reveal its Messages and Contacts
        // permission toggles expires a few minutes after pairing. Without this the
        // only way back to those toggles is to remove the bond and pair again.
        void on_bt_solicit_click(GtkWidget*, gpointer) {
            if (!daemon_send({{"command", "bt_solicit"}})) {
                set_text(g_devices.lbl_bt_progress, "Could not reach the Tether daemon.");
                return;
            }
            set_text(g_devices.lbl_bt_progress, "Asking the iPhone to show its Bluetooth permissions\u2026");
        }

        void send_file_async(const std::filesystem::path& path) {
            if (!daemon_connected()) {
                set_status_action("Daemon unavailable.");
                return;
            }
            set_status_action("Sending " + path.filename().string() + "…");
            nlohmann::json j;
            j["command"] = "send_file";
            j["path"] = path.string();
            daemon_send(j);
        }

        void on_choose_file(GtkWidget*, gpointer) {
            GtkWidget* dialog = gtk_file_chooser_dialog_new("Send File",
                                                            GTK_WINDOW(main_window()),
                                                            GTK_FILE_CHOOSER_ACTION_OPEN,
                                                            "_Cancel",
                                                            GTK_RESPONSE_CANCEL,
                                                            "_Send",
                                                            GTK_RESPONSE_ACCEPT,
                                                            nullptr);
            if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
                char* filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
                if (filename) {
                    send_file_async(filename);
                    g_free(filename);
                }
            }
            gtk_widget_destroy(dialog);
        }

        void update_wifi_indicator() {
            const bool connected = !g_devices.connected_fps.empty();
            set_route_status(Route::WiFi,
                             connected,
                             connected ? "Clipboard, files, and OTP are connected."
                                       : "No paired iPhone is connected. Open the Tether app on the phone.");
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
                        dev.name = c.value("device_name", "Unknown Device");
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
                    req.name = p.value("device_name", "Unknown Device");
                    g_devices.pending_pairing_requests.push_back(req);
                }
            }
            devices_view_refresh();
            update_wifi_indicator();
        }

    } // namespace

    void devices_view_trigger_discovery() {
        set_status_main("Scanning for nearby devices...");
        // Refresh means both routes: mDNS for Wi-Fi peers, BlueZ for Bluetooth.
        daemon_send({{"command", "discover"}});
        daemon_send({{"command", "bt_status"}});
        daemon_send({{"command", "bt_list_devices"}});
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
                        {fingerprint, name_value.is_string() ? name_value.get<std::string>() : "Unknown Device"});
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
                paired ? (online ? "network-cellular-connected-symbolic" : "network-cellular-offline-symbolic")
                       : "dialog-information-symbolic";
            GtkWidget* icon = gtk_image_new_from_icon_name(icon_name, GTK_ICON_SIZE_LARGE_TOOLBAR);
            gtk_box_pack_start(GTK_BOX(box), icon, FALSE, FALSE, 0);

            GtkWidget* labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
            GtkWidget* title = gtk_label_new(nullptr);
            gtk_label_set_markup(GTK_LABEL(title), ("<b>" + escape_markup(name) + "</b>").c_str());
            gtk_label_set_xalign(GTK_LABEL(title), 0.0);
            gtk_box_pack_start(GTK_BOX(labels), title, FALSE, FALSE, 0);

            GtkWidget* subtitle = gtk_label_new(paired ? (online ? "Connected" : "Offline") : "Nearby (Tap to Pair)");
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

            GtkWidget* subtitle = gtk_label_new(connected ? "Connected" : (paired ? "Paired" : "Nearby (Tap to Pair)"));
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

        if (!connected_rows.empty()) {
            gtk_list_box_insert(GTK_LIST_BOX(g_devices.list_devices), create_header_row("CONNECTED"), -1);
            for (auto* r : connected_rows)
                gtk_list_box_insert(GTK_LIST_BOX(g_devices.list_devices), r, -1);
        }

        if (!remembered_rows.empty()) {
            gtk_list_box_insert(GTK_LIST_BOX(g_devices.list_devices), create_header_row("REMEMBERED"), -1);
            for (auto* r : remembered_rows)
                gtk_list_box_insert(GTK_LIST_BOX(g_devices.list_devices), r, -1);
        }

        if (!discovered_rows.empty()) {
            gtk_list_box_insert(GTK_LIST_BOX(g_devices.list_devices), create_header_row("DISCOVERED"), -1);
            for (auto* r : discovered_rows)
                gtk_list_box_insert(GTK_LIST_BOX(g_devices.list_devices), r, -1);
        }

        // ponytail: iPhones, plus whatever Tether is actually bonded to. Every
        // other paired thing BlueZ knows about -- headphones, controllers,
        // keyboards -- is noise on this page. Widen the filter only if someone
        // needs to pair a phone that does not look like an iPhone.
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

            std::string resolved_name = event.value("device_name", "Unknown Device");
            if (resolved_name == "Unknown Device") {
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
            set_status_main("Ready");
            return true;
        }
        if (command == "file_send_complete") {
            set_status_action(event.value("message", ""));
            return true;
        }
        if (command == "clipboard_content") {
            set_status_action("Desktop Clipboard Sync triggered.");
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
                detail = "Messages and notifications are connected.";
            set_route_status(Route::Bluetooth, connected, detail);
            update_right_pane();
            // A status broadcast, not a command this view owns: the Messages and
            // Notifications views need the same event.
            return false;
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
        gtk_label_set_markup(GTK_LABEL(welcome_title), "<big><b>Connect your iPhone</b></big>");
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
                                            "<b>Wi-Fi</b>\nclipboard, files, OTP",
                                            "1.  Open the Tether app on the iPhone\n"
                                            "2.  It finds this computer by itself\n"
                                            "3.  Approve on both ends",
                                            &g_devices.lbl_welcome_wifi));

        gtk_container_add(GTK_CONTAINER(routes),
                          build_route_block("bluetooth-active-symbolic",
                                            "<b>Bluetooth</b>\nmessages, notifications",
                                            "1.  Pick the iPhone under BLUETOOTH\n"
                                            "2.  Press Pair over Bluetooth\n"
                                            "3.  Grant the two toggles on the phone",
                                            &g_devices.lbl_welcome_bt));

        gtk_box_pack_start(GTK_BOX(placeholder), routes, FALSE, FALSE, 0);

        // One button for both routes: scanning is what refreshes either list.
        GtkWidget* welcome_scan = gtk_button_new_with_label("Scan for devices");
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

        g_devices.lbl_bt_reason = gtk_label_new(nullptr);
        gtk_label_set_xalign(GTK_LABEL(g_devices.lbl_bt_reason), 0.0);
        gtk_label_set_line_wrap(GTK_LABEL(g_devices.lbl_bt_reason), TRUE);
        gtk_style_context_add_class(gtk_widget_get_style_context(g_devices.lbl_bt_reason), "muted");
        gtk_box_pack_start(GTK_BOX(bt_box), g_devices.lbl_bt_reason, FALSE, FALSE, 0);

        GtkWidget* bt_buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        g_devices.btn_bt_pair = gtk_button_new_with_label("Pair over Bluetooth");
        gtk_style_context_add_class(gtk_widget_get_style_context(g_devices.btn_bt_pair), "suggested-action");
        g_signal_connect(g_devices.btn_bt_pair, "clicked", G_CALLBACK(on_bt_pair_click), nullptr);
        gtk_box_pack_start(GTK_BOX(bt_buttons), g_devices.btn_bt_pair, FALSE, FALSE, 0);

        g_devices.btn_bt_solicit = gtk_button_new_with_label("Show iPhone Permissions");
        gtk_widget_set_tooltip_text(g_devices.btn_bt_solicit,
                                    "Re-advertise so the iPhone shows its Show Message Notifications and "
                                    "Sync Contacts toggles under Settings > Bluetooth > (i).");
        g_signal_connect(g_devices.btn_bt_solicit, "clicked", G_CALLBACK(on_bt_solicit_click), nullptr);
        gtk_box_pack_start(GTK_BOX(bt_buttons), g_devices.btn_bt_solicit, FALSE, FALSE, 0);

        g_devices.btn_bt_unpair = gtk_button_new_with_label("Unpair");
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

        GtkWidget* pair_btn = gtk_button_new_with_label("Pair Device");
        gtk_style_context_add_class(gtk_widget_get_style_context(pair_btn), "suggested-action");
        g_signal_connect(pair_btn, "clicked", G_CALLBACK(on_pair_click), nullptr);
        gtk_box_pack_start(GTK_BOX(pair_box), pair_btn, FALSE, FALSE, 0);

        // optional accept override button
        GtkWidget* accept_btn = gtk_button_new_with_label("Force Trust (Accept Pending)");
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

        GtkWidget* btn_send_file = gtk_button_new_with_label("Send File");
        gtk_style_context_add_class(gtk_widget_get_style_context(btn_send_file), "suggested-action");
        g_signal_connect(btn_send_file, "clicked", G_CALLBACK(on_choose_file), nullptr);
        gtk_box_pack_start(GTK_BOX(btn_grid), btn_send_file, FALSE, FALSE, 0);

        GtkWidget* btn_send_clip = gtk_button_new_with_label("Send Clipboard");
        g_signal_connect(btn_send_clip,
                         "clicked",
                         G_CALLBACK(+[](GtkWidget*, gpointer) {
                             nlohmann::json j;
                             j["command"] = "clipboard_set"; // triggers clipboard send
                             daemon_send(j);
                             set_status_action("Clipboard sync requested...");
                         }),
                         nullptr);
        gtk_box_pack_start(GTK_BOX(btn_grid), btn_send_clip, FALSE, FALSE, 0);

        gtk_box_pack_start(GTK_BOX(action_box), btn_grid, FALSE, FALSE, 0);

        g_devices.lbl_action_status = gtk_label_new("Ready");
        gtk_widget_set_halign(g_devices.lbl_action_status, GTK_ALIGN_CENTER);
        gtk_style_context_add_class(gtk_widget_get_style_context(g_devices.lbl_action_status), "muted");
        gtk_box_pack_start(GTK_BOX(action_box), g_devices.lbl_action_status, FALSE, FALSE, 0);

        gtk_stack_add_named(GTK_STACK(g_devices.right_pane_stack), action_box, "action");

        gtk_paned_pack2(GTK_PANED(paned), g_devices.right_pane_stack, TRUE, FALSE);
        gtk_stack_set_visible_child_name(GTK_STACK(g_devices.right_pane_stack), "placeholder");

        return paned;
    }

} // namespace tether::ui
