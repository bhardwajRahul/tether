#include "messages_view.hpp"
#include "daemon_client.hpp"
#include "ui_util.hpp"

#include <ctime>
#include <gdk/gdkkeysyms.h>
#include <set>
#include <string>
#include <vector>

namespace tether::ui {

    namespace {

        struct MessagesState {
            GtkWidget* banner = nullptr;
            GtkWidget* banner_label = nullptr;
            GtkWidget* banner_action = nullptr;
            GtkWidget* thread_list = nullptr;
            GtkWidget* conversation = nullptr;
            GtkWidget* conversation_header = nullptr;
            GtkWidget* conversation_scroll = nullptr;
            GtkWidget* composer = nullptr;
            GtkWidget* send_button = nullptr;
            GtkWidget* placeholder_stack = nullptr;

            std::string selected_thread;
            std::string selected_name;
            bool visible = false;
            bool map_open = false;
            bool sending = false;
            GtkWidget* composer_notice = nullptr;
            // Rebuilding the thread list destroys and recreates the selected row.
            // The handler is blocked across that so the churn is not mistaken for
            // the user opening a conversation.
            gulong thread_selected_handler = 0;
            // A send whose result never arrives must not lock the composer for the
            // rest of the session.
            guint send_watchdog_id = 0;
            // Handles already asked about, so reopening a conversation does not
            // re-issue a blocking OBEX write per message every time.
            std::set<std::string> marked_read;
            // Scrolling to the newest message has to wait for GTK to lay the new
            // rows out, so it is deferred rather than done inline.
            guint scroll_idle_id = 0;
            // Whether the daemon says the selected thread can be replied to, and
            // why not when it cannot.
            bool selected_repliable = false;
            std::string selected_block_reason;
            std::string selected_warning;
        };

        MessagesState g_messages;

        constexpr int SEND_TIMEOUT_SECONDS = 60;

        void update_composer_sensitivity();
        void apply_row_selection(GtkWidget* row);
        void clear_selection();
        // `offer_permissions` shows the button that re-solicits the iPhone's
        // Bluetooth toggles, which only helps when that is the actual problem.
        void set_banner(const std::string& text, bool offer_permissions = false);
        std::string composer_text();

        std::string format_timestamp(int64_t epoch) {
            if (epoch <= 0)
                return "";
            std::time_t t = static_cast<std::time_t>(epoch);
            std::tm tm{};
            localtime_r(&t, &tm);

            std::time_t now = std::time(nullptr);
            std::tm now_tm{};
            localtime_r(&now, &now_tm);

            char buffer[64];
            // Same day gets a time, anything older gets a date: a wall of
            // identical timestamps is harder to scan than a mixed list.
            const char* format =
                (tm.tm_year == now_tm.tm_year && tm.tm_yday == now_tm.tm_yday) ? "%H:%M" : "%b %-d, %H:%M";
            std::strftime(buffer, sizeof(buffer), format, &tm);
            return buffer;
        }

        void request_threads() {
            nlohmann::json j;
            j["command"] = "bt_list_threads";
            daemon_send(j);
        }

        void request_messages(const std::string& thread_key) {
            if (thread_key.empty())
                return;
            nlohmann::json j;
            j["command"] = "bt_list_messages";
            j["thread"] = thread_key;
            daemon_send(j);
        }

        GtkWidget* build_thread_row(const nlohmann::json& thread) {
            const std::string key = thread.value("thread", "");
            const std::string address = thread.value("address", "");
            const std::string name = thread.value("name", address);
            const std::string preview = thread.value("preview", "");
            const int unread = thread.value("unread", 0);

            GtkWidget* row = gtk_list_box_row_new();
            g_object_set_data_full(G_OBJECT(row), "thread", g_strdup(key.c_str()), g_free);
            g_object_set_data_full(G_OBJECT(row), "name", g_strdup(name.c_str()), g_free);
            // The daemon owns the decision about whether a group can be replied
            // to; the UI must not re-derive it from the key shape.
            g_object_set_data(G_OBJECT(row), "repliable", GINT_TO_POINTER(thread.value("repliable", true) ? 1 : 0));
            g_object_set_data_full(
                G_OBJECT(row), "reply_warning", g_strdup(thread.value("reply_warning", "").c_str()), g_free);
            g_object_set_data_full(
                G_OBJECT(row), "reply_reason", g_strdup(thread.value("reply_reason", "").c_str()), g_free);
            g_object_set_data(G_OBJECT(row), "group", GINT_TO_POINTER(thread.value("group", false) ? 1 : 0));

            GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
            gtk_container_set_border_width(GTK_CONTAINER(box), 10);

            GtkWidget* labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

            GtkWidget* title = gtk_label_new(nullptr);
            gtk_label_set_markup(GTK_LABEL(title), ("<b>" + escape_markup(name) + "</b>").c_str());
            gtk_label_set_xalign(GTK_LABEL(title), 0.0);
            gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
            gtk_box_pack_start(GTK_BOX(labels), title, FALSE, FALSE, 0);

            GtkWidget* subtitle = gtk_label_new(preview.c_str());
            gtk_label_set_xalign(GTK_LABEL(subtitle), 0.0);
            gtk_label_set_ellipsize(GTK_LABEL(subtitle), PANGO_ELLIPSIZE_END);
            gtk_label_set_single_line_mode(GTK_LABEL(subtitle), TRUE);
            gtk_style_context_add_class(gtk_widget_get_style_context(subtitle), "muted");
            gtk_box_pack_start(GTK_BOX(labels), subtitle, FALSE, FALSE, 0);

            gtk_box_pack_start(GTK_BOX(box), labels, TRUE, TRUE, 0);

            if (unread > 0) {
                GtkWidget* badge = gtk_label_new(nullptr);
                gtk_label_set_markup(GTK_LABEL(badge), ("<b>" + std::to_string(unread) + "</b>").c_str());
                gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);
                gtk_style_context_add_class(gtk_widget_get_style_context(badge), "tether-badge");
                gtk_box_pack_start(GTK_BOX(box), badge, FALSE, FALSE, 0);
            }

            gtk_container_add(GTK_CONTAINER(row), box);
            return row;
        }

        GtkWidget* build_message_row(const nlohmann::json& message) {
            const bool outgoing = message.value("outgoing", false);
            const std::string body = message.value("body", "");
            const std::string stamp = format_timestamp(message.value("timestamp", static_cast<int64_t>(0)));

            GtkWidget* row = gtk_list_box_row_new();
            gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
            gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);

            GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
            gtk_container_set_border_width(GTK_CONTAINER(box), 8);
            gtk_widget_set_halign(box, outgoing ? GTK_ALIGN_END : GTK_ALIGN_START);

            GtkWidget* bubble = gtk_label_new(body.c_str());
            gtk_label_set_line_wrap(GTK_LABEL(bubble), TRUE);
            gtk_label_set_line_wrap_mode(GTK_LABEL(bubble), PANGO_WRAP_WORD_CHAR);
            gtk_label_set_max_width_chars(GTK_LABEL(bubble), 48);
            gtk_label_set_xalign(GTK_LABEL(bubble), 0.0);
            gtk_label_set_selectable(GTK_LABEL(bubble), TRUE);
            // Without this the label stretches to whatever else is in the row and
            // the bubble reads as a full-width bar rather than wrapping the text.
            gtk_widget_set_halign(bubble, outgoing ? GTK_ALIGN_END : GTK_ALIGN_START);
            GtkStyleContext* bubble_style = gtk_widget_get_style_context(bubble);
            gtk_style_context_add_class(bubble_style, "tether-bubble");
            gtk_style_context_add_class(bubble_style, outgoing ? "tether-bubble-out" : "tether-bubble-in");
            // The phone reports every send as delivered, including the ones it addressed to nobody.
            const bool unconfirmed = outgoing && !g_messages.selected_warning.empty();
            if (unconfirmed)
                gtk_style_context_add_class(bubble_style, "tether-bubble-unconfirmed");
            gtk_box_pack_start(GTK_BOX(box), bubble, FALSE, FALSE, 0);

            const std::string meta =
                unconfirmed ? (stamp.empty() ? std::string("not confirmed") : stamp + " \u00b7 not confirmed") : stamp;
            if (!meta.empty()) {
                GtkWidget* time_label = gtk_label_new(meta.c_str());
                gtk_label_set_xalign(GTK_LABEL(time_label), outgoing ? 1.0 : 0.0);
                gtk_style_context_add_class(gtk_widget_get_style_context(time_label), "muted");
                if (unconfirmed)
                    gtk_widget_set_tooltip_text(time_label, g_messages.selected_warning.c_str());
                gtk_box_pack_start(GTK_BOX(box), time_label, FALSE, FALSE, 0);
            }

            gtk_container_add(GTK_CONTAINER(row), box);
            return row;
        }

        gboolean scroll_to_end_idle(gpointer) {
            g_messages.scroll_idle_id = 0;
            if (!g_messages.conversation_scroll)
                return G_SOURCE_REMOVE;
            GtkAdjustment* adjustment =
                gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(g_messages.conversation_scroll));
            if (adjustment) {
                // upper is the full content height; subtracting the visible page
                // is what puts the last message at the bottom edge rather than
                // one screen past it.
                gtk_adjustment_set_value(
                    adjustment, gtk_adjustment_get_upper(adjustment) - gtk_adjustment_get_page_size(adjustment));
            }
            return G_SOURCE_REMOVE;
        }

        void scroll_conversation_to_end() {
            // The rows have only just been inserted, so GTK has not laid them out
            // and the adjustment still reports the bounds of the old contents.
            // Scrolling now lands somewhere short of the newest message.
            if (g_messages.scroll_idle_id == 0)
                g_messages.scroll_idle_id =
                    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, scroll_to_end_idle, nullptr, nullptr);
        }

        void show_threads(const nlohmann::json& event) {
            if (!event.contains("threads") || !event["threads"].is_array())
                return;

            // Clearing the list destroys the selected row, which fires
            // row-selected(nullptr) and then again for the row that replaces it.
            // Unblocked, that reads as the user opening a conversation: it
            // re-requests the messages, which re-sends their mark-read requests,
            // which produce another refresh — a loop that never settles.
            if (g_messages.thread_selected_handler)
                g_signal_handler_block(g_messages.thread_list, g_messages.thread_selected_handler);

            clear_list_box(g_messages.thread_list);

            GtkWidget* reselect = nullptr;
            for (const auto& thread : event["threads"]) {
                GtkWidget* row = build_thread_row(thread);
                gtk_list_box_insert(GTK_LIST_BOX(g_messages.thread_list), row, -1);
                if (thread.value("thread", "") == g_messages.selected_thread)
                    reselect = row;
            }
            gtk_widget_show_all(g_messages.thread_list);

            // Restoring the selection keeps the open conversation from closing
            // every time a message arrives. The row is new, so the reply
            // eligibility attached to it has to be re-read.
            if (reselect) {
                gtk_list_box_select_row(GTK_LIST_BOX(g_messages.thread_list), GTK_LIST_BOX_ROW(reselect));
                apply_row_selection(reselect);
            } else if (!g_messages.selected_thread.empty()) {
                clear_selection();
            }

            if (g_messages.thread_selected_handler)
                g_signal_handler_unblock(g_messages.thread_list, g_messages.thread_selected_handler);
        }

        void show_messages(const nlohmann::json& event) {
            if (event.value("thread", "") != g_messages.selected_thread)
                return;

            clear_list_box(g_messages.conversation);
            if (!event.contains("messages") || !event["messages"].is_array()) {
                gtk_widget_show_all(g_messages.conversation);
                return;
            }

            for (const auto& message : event["messages"])
                gtk_list_box_insert(GTK_LIST_BOX(g_messages.conversation), build_message_row(message), -1);
            gtk_widget_show_all(g_messages.conversation);
            scroll_conversation_to_end();

            // Opening a conversation marks it read here and on the phone, which is
            // what makes the unread badge agree with what the user has seen. One
            // request for the whole batch, and never twice for the same message:
            // each one costs the daemon a blocking OBEX write.
            std::vector<std::string> pending;
            for (const auto& message : event["messages"]) {
                if (message.value("read", true) || message.value("outgoing", false))
                    continue;
                const std::string handle = message.value("handle", "");
                if (!handle.empty() && !g_messages.marked_read.count(handle))
                    pending.push_back(handle);
            }
            if (!pending.empty()) {
                nlohmann::json j;
                j["command"] = "bt_mark_read";
                j["handles"] = pending;
                j["read"] = true;
                // Only remember them once the request is actually on its way, so a
                // command lost to a dead socket is retried next time rather than
                // leaving the conversation permanently stuck unread.
                if (daemon_send(j))
                    g_messages.marked_read.insert(pending.begin(), pending.end());
            }
        }

        void set_banner(const std::string& text, bool offer_permissions) {
            if (!g_messages.banner)
                return;
            if (text.empty()) {
                gtk_widget_hide(g_messages.banner);
                return;
            }
            set_text(g_messages.banner_label, text);
            gtk_widget_show_all(g_messages.banner);
            // show_all revealed it; hide it again unless re-soliciting is the
            // thing that would actually help here.
            gtk_widget_set_visible(g_messages.banner_action, offer_permissions);
        }

        // The advertisement that makes iOS reveal its Messages and Contacts
        // permission toggles expires a few minutes after pairing. Without this the
        // only way back to those toggles is to remove the bond and pair again.
        void on_solicit_clicked(GtkWidget*, gpointer) {
            if (!daemon_send({{"command", "bt_solicit"}})) {
                set_status_main("Could not reach the Tether daemon.");
                return;
            }
            set_status_main("Asking the iPhone to show its Bluetooth permissions…");
        }

        void update_connection(const nlohmann::json& event) {
            const bool map_open = event.value("map_open", false);
            g_messages.map_open = map_open;
            update_composer_sensitivity();
            if (map_open) {
                set_banner("");
                if (g_messages.visible)
                    request_threads();
                return;
            }
            // The daemon's reason strings name the actual next step, including
            // which toggle to flip on the phone, so they are shown verbatim
            // rather than replaced with something vaguer.
            std::string reason = event.value("profile_reason", "");
            if (reason.empty())
                reason = event.value("link_reason", "");
            if (reason.empty())
                reason = "Messages are not connected.";

            // Re-soliciting only helps when the phone is withholding the profile,
            // not when the link itself is down.
            const std::string map_error = event.value("map_error", "none");
            set_banner(reason, map_error == "forbidden" || map_error == "no_record");
        }

        // Replying needs an open conversation and a live MAP session. A group
        // thread has no reply routing yet, and the daemon refuses one anyway, so
        // the composer stays shut rather than accepting text that will bounce.
        void update_composer_sensitivity() {
            const bool can_send =
                g_messages.map_open && !g_messages.selected_thread.empty() && g_messages.selected_repliable;
            if (!g_messages.composer)
                return;

            // The box stays typable whenever the conversation can take a reply;
            // only the button waits for there to be something in it.
            const bool composer_live = can_send && !g_messages.sending;
            gtk_widget_set_sensitive(g_messages.composer, composer_live);
            gtk_widget_set_sensitive(g_messages.send_button, composer_live && !composer_text().empty());

            const char* reason = nullptr;
            if (g_messages.sending)
                reason = "Sending…";
            else if (!g_messages.map_open)
                reason = "Messages are not connected.";
            else if (g_messages.selected_thread.empty())
                reason = "Select a conversation first.";
            else if (!g_messages.selected_block_reason.empty())
                reason = g_messages.selected_block_reason.c_str();
            else if (!can_send)
                reason = "Replying to this conversation is not available.";
            gtk_widget_set_tooltip_text(g_messages.send_button, reason);

            if (g_messages.composer_notice) {
                // Why the box is shut, or -- when it is open but the phone is
                // known to mangle this conversation's address -- why a send that
                // reports success may still not arrive.
                const char* notice = nullptr;
                if (reason && !composer_live && !g_messages.sending)
                    notice = reason;
                else if (composer_live && !g_messages.selected_warning.empty())
                    notice = g_messages.selected_warning.c_str();
                if (notice)
                    set_text(g_messages.composer_notice, notice);
                gtk_widget_set_visible(g_messages.composer_notice, notice != nullptr);
            }
        }

        std::string composer_text() {
            GtkTextBuffer* buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_messages.composer));
            GtkTextIter start, end;
            gtk_text_buffer_get_bounds(buffer, &start, &end);
            gchar* text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
            std::string out = text ? text : "";
            g_free(text);
            return out;
        }

        void clear_sending() {
            if (g_messages.send_watchdog_id != 0) {
                g_source_remove(g_messages.send_watchdog_id);
                g_messages.send_watchdog_id = 0;
            }
            g_messages.sending = false;
        }

        gboolean on_send_timeout(gpointer) {
            g_messages.send_watchdog_id = 0;
            if (g_messages.sending) {
                g_messages.sending = false;
                update_composer_sensitivity();
                set_status_main("No answer about that message; it may still have been sent.");
            }
            return G_SOURCE_REMOVE;
        }

        void on_send_clicked(GtkWidget*, gpointer) {
            const std::string body = composer_text();
            if (body.empty() || g_messages.selected_thread.empty())
                return;

            nlohmann::json j;
            j["command"] = "bt_send_message";
            j["thread"] = g_messages.selected_thread;
            j["body"] = body;
            if (!daemon_send(j)) {
                set_status_main("Could not reach the Tether daemon; the message was not sent.");
                return;
            }

            // The composer stays locked until the phone answers. Sending is a
            // real OBEX transfer and takes a moment; an unlocked box invites a
            // second copy of the same message. The watchdog is what stops a
            // result that never arrives from locking it for the whole session.
            g_messages.sending = true;
            g_messages.send_watchdog_id = g_timeout_add_seconds(SEND_TIMEOUT_SECONDS, on_send_timeout, nullptr);
            update_composer_sensitivity();
            set_status_main("Sending…");
        }

        // Every messaging app has trained people that Enter sends and Shift+Enter
        // starts a new line. A multi-line box that only sends on a button click is
        // the single thing that makes a composer feel unfinished.
        gboolean on_composer_key(GtkWidget*, GdkEventKey* event, gpointer) {
            if (event->keyval != GDK_KEY_Return && event->keyval != GDK_KEY_KP_Enter)
                return FALSE;
            if (event->state & (GDK_SHIFT_MASK | GDK_CONTROL_MASK))
                return FALSE;
            // Mid-composition input methods deliver Return as part of a candidate
            // selection, which must not be read as a send.
            if (event->state & GDK_MOD1_MASK)
                return FALSE;

            if (gtk_widget_is_sensitive(g_messages.send_button))
                on_send_clicked(nullptr, nullptr);
            return TRUE;
        }

        void on_send_result(const nlohmann::json& event) {
            clear_sending();
            update_composer_sensitivity();

            if (event.value("success", false)) {
                // Only cleared once the phone accepted it, so a failed send
                // leaves the text where the user can retry or copy it out.
                GtkTextBuffer* buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_messages.composer));
                gtk_text_buffer_set_text(buffer, "", -1);
                set_status_main("Sent");
                return;
            }
            set_status_main(event.value("message", "The message was not sent."));
        }

        void clear_selection() {
            g_messages.selected_thread.clear();
            g_messages.selected_name.clear();
            g_messages.selected_repliable = false;
            g_messages.selected_block_reason.clear();
            g_messages.selected_warning.clear();
            update_composer_sensitivity();
            gtk_stack_set_visible_child_name(GTK_STACK(g_messages.placeholder_stack), "placeholder");
        }

        // Reads the thread identity and reply eligibility the daemon attached to
        // this row. The daemon owns that decision; the UI must not re-derive it.
        void apply_row_selection(GtkWidget* row) {
            const char* thread = (const char*)g_object_get_data(G_OBJECT(row), "thread");
            const char* name = (const char*)g_object_get_data(G_OBJECT(row), "name");
            const char* block_reason = (const char*)g_object_get_data(G_OBJECT(row), "reply_reason");
            const char* warning = (const char*)g_object_get_data(G_OBJECT(row), "reply_warning");
            g_messages.selected_thread = thread ? thread : "";
            g_messages.selected_name = name ? name : "";
            g_messages.selected_repliable = g_object_get_data(G_OBJECT(row), "repliable") != nullptr;
            g_messages.selected_block_reason = block_reason ? block_reason : "";
            g_messages.selected_warning = warning ? warning : "";
            // Which conversation is open is otherwise only legible from the
            // selection highlight in the list beside it.
            set_markup(g_messages.conversation_header,
                       "<b>" +
                           escape_markup(g_messages.selected_name.empty() ? g_messages.selected_thread
                                                                          : g_messages.selected_name) +
                           "</b>");
            update_composer_sensitivity();
        }

        void on_thread_selected(GtkListBox*, GtkListBoxRow* row, gpointer) {
            if (!row) {
                clear_selection();
                return;
            }
            apply_row_selection(GTK_WIDGET(row));
            gtk_stack_set_visible_child_name(GTK_STACK(g_messages.placeholder_stack), "conversation");
            request_messages(g_messages.selected_thread);
            // Only on a real click: the handler is blocked while the list is
            // rebuilt, so this cannot steal focus behind the user's back.
            if (gtk_widget_is_sensitive(g_messages.composer))
                gtk_widget_grab_focus(g_messages.composer);
        }

    } // namespace

    void messages_view_handle_disconnect() {
        clear_sending();
        g_messages.map_open = false;
        // The next session re-lists messages under fresh object paths, so nothing
        // is owed from the one that just ended.
        g_messages.marked_read.clear();
        update_composer_sensitivity();
        set_banner("The Tether daemon is not running.");
    }

    void messages_view_set_visible(bool visible) {
        g_messages.visible = visible;
        if (!visible)
            return;
        request_threads();
        request_messages(g_messages.selected_thread);
    }

    bool messages_view_handle_event(const nlohmann::json& event) {
        const std::string command = event.value("command", "");
        if (command == "bt_threads") {
            show_threads(event);
            return true;
        }
        if (command == "bt_messages") {
            show_messages(event);
            return true;
        }
        if (command == "bt_connection_changed") {
            update_connection(event);
            return false;
        }
        if (command == "bt_message") {
            // A new message changes both the thread list and, when it lands in
            // the open conversation, the conversation itself. Refreshing while
            // the view is hidden would just be traffic nobody sees; switching
            // back re-reads everything anyway.
            if (!g_messages.visible)
                return true;
            request_threads();
            if (event.value("thread", "") == g_messages.selected_thread)
                request_messages(g_messages.selected_thread);
            return true;
        }
        if (command == "bt_send_result") {
            on_send_result(event);
            return true;
        }
        if (command == "bt_message_read") {
            if (g_messages.visible)
                request_threads();
            return true;
        }
        if (command == "bt_solicit_result") {
            set_status_main(event.value("message", ""));
            return true;
        }
        return false;
    }

    GtkWidget* messages_view_new() {
        GtkWidget* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

        g_messages.banner = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_container_set_border_width(GTK_CONTAINER(g_messages.banner), 8);
        GtkWidget* banner_icon = gtk_image_new_from_icon_name("dialog-information-symbolic", GTK_ICON_SIZE_BUTTON);
        gtk_box_pack_start(GTK_BOX(g_messages.banner), banner_icon, FALSE, FALSE, 0);
        g_messages.banner_label = gtk_label_new(nullptr);
        gtk_label_set_xalign(GTK_LABEL(g_messages.banner_label), 0.0);
        gtk_label_set_line_wrap(GTK_LABEL(g_messages.banner_label), TRUE);
        gtk_box_pack_start(GTK_BOX(g_messages.banner), g_messages.banner_label, TRUE, TRUE, 0);

        g_messages.banner_action = gtk_button_new_with_label("Show iPhone Permissions");
        gtk_widget_set_valign(g_messages.banner_action, GTK_ALIGN_CENTER);
        gtk_widget_set_tooltip_text(g_messages.banner_action,
                                    "Re-advertise so the iPhone shows its Show Message Notifications and "
                                    "Sync Contacts toggles under Settings > Bluetooth > (i).");
        g_signal_connect(g_messages.banner_action, "clicked", G_CALLBACK(on_solicit_clicked), nullptr);
        gtk_box_pack_start(GTK_BOX(g_messages.banner), g_messages.banner_action, FALSE, FALSE, 0);

        gtk_box_pack_start(GTK_BOX(root), g_messages.banner, FALSE, FALSE, 0);

        GtkWidget* paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
        gtk_box_pack_start(GTK_BOX(root), paned, TRUE, TRUE, 0);

        GtkWidget* thread_scroll = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(thread_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_widget_set_size_request(thread_scroll, 240, -1);
        g_messages.thread_list = gtk_list_box_new();
        g_messages.thread_selected_handler =
            g_signal_connect(g_messages.thread_list, "row-selected", G_CALLBACK(on_thread_selected), nullptr);
        gtk_container_add(GTK_CONTAINER(thread_scroll), g_messages.thread_list);
        gtk_paned_pack1(GTK_PANED(paned), thread_scroll, FALSE, FALSE);

        g_messages.placeholder_stack = gtk_stack_new();

        GtkWidget* placeholder = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
        gtk_widget_set_valign(placeholder, GTK_ALIGN_CENTER);
        gtk_widget_set_halign(placeholder, GTK_ALIGN_CENTER);
        GtkWidget* placeholder_icon = gtk_image_new_from_icon_name("mail-unread-symbolic", GTK_ICON_SIZE_DIALOG);
        gtk_box_pack_start(GTK_BOX(placeholder), placeholder_icon, FALSE, FALSE, 0);
        GtkWidget* placeholder_label = gtk_label_new("Select a conversation");
        gtk_style_context_add_class(gtk_widget_get_style_context(placeholder_label), "muted");
        gtk_box_pack_start(GTK_BOX(placeholder), placeholder_label, FALSE, FALSE, 0);
        gtk_stack_add_named(GTK_STACK(g_messages.placeholder_stack), placeholder, "placeholder");

        GtkWidget* conversation_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

        GtkWidget* conversation_header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_container_set_border_width(GTK_CONTAINER(conversation_header_box), 10);
        g_messages.conversation_header = gtk_label_new(nullptr);
        gtk_label_set_xalign(GTK_LABEL(g_messages.conversation_header), 0.0);
        gtk_label_set_ellipsize(GTK_LABEL(g_messages.conversation_header), PANGO_ELLIPSIZE_END);
        gtk_box_pack_start(GTK_BOX(conversation_header_box), g_messages.conversation_header, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(conversation_box), conversation_header_box, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(conversation_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);

        g_messages.conversation_scroll = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_policy(
            GTK_SCROLLED_WINDOW(g_messages.conversation_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        g_messages.conversation = gtk_list_box_new();
        gtk_list_box_set_selection_mode(GTK_LIST_BOX(g_messages.conversation), GTK_SELECTION_NONE);
        gtk_container_add(GTK_CONTAINER(g_messages.conversation_scroll), g_messages.conversation);
        gtk_box_pack_start(GTK_BOX(conversation_box), g_messages.conversation_scroll, TRUE, TRUE, 0);

        GtkWidget* composer_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_container_set_border_width(GTK_CONTAINER(composer_box), 8);
        g_messages.composer = gtk_text_view_new();
        gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(g_messages.composer), GTK_WRAP_WORD_CHAR);
        gtk_text_view_set_left_margin(GTK_TEXT_VIEW(g_messages.composer), 6);
        gtk_text_view_set_right_margin(GTK_TEXT_VIEW(g_messages.composer), 6);
        gtk_text_view_set_top_margin(GTK_TEXT_VIEW(g_messages.composer), 6);
        gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(g_messages.composer), 6);
        gtk_widget_set_size_request(g_messages.composer, -1, 48);
        GtkWidget* composer_frame = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(composer_frame), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        // Without a border the box floats in the panel with nothing marking it as
        // somewhere to type.
        gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(composer_frame), GTK_SHADOW_IN);
        gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(composer_frame), 140);
        gtk_container_add(GTK_CONTAINER(composer_frame), g_messages.composer);
        gtk_box_pack_start(GTK_BOX(composer_box), composer_frame, TRUE, TRUE, 0);

        g_messages.send_button = gtk_button_new_with_label("Send");
        gtk_widget_set_valign(g_messages.send_button, GTK_ALIGN_END);
        gtk_style_context_add_class(gtk_widget_get_style_context(g_messages.send_button), "suggested-action");
        gtk_box_pack_start(GTK_BOX(composer_box), g_messages.send_button, FALSE, FALSE, 0);

        g_signal_connect(g_messages.send_button, "clicked", G_CALLBACK(on_send_clicked), nullptr);
        g_signal_connect(g_messages.composer, "key-press-event", G_CALLBACK(on_composer_key), nullptr);
        // The button follows what is actually in the box, so "Send" is never
        // offered for an empty message.
        g_signal_connect(gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_messages.composer)),
                         "changed",
                         G_CALLBACK(+[](GtkTextBuffer*, gpointer) { update_composer_sensitivity(); }),
                         nullptr);
        // Enabled only once MAP is up and a conversation is open.
        update_composer_sensitivity();

        // A dead composer with the explanation hidden in the send button's tooltip
        // reads as the app being broken. The button is insensitive, so the tooltip
        // never appears at all.
        g_messages.composer_notice = gtk_label_new(nullptr);
        gtk_label_set_xalign(GTK_LABEL(g_messages.composer_notice), 0.0);
        gtk_label_set_line_wrap(GTK_LABEL(g_messages.composer_notice), TRUE);
        gtk_widget_set_no_show_all(g_messages.composer_notice, TRUE);
        gtk_style_context_add_class(gtk_widget_get_style_context(g_messages.composer_notice), "muted");
        gtk_widget_set_margin_start(g_messages.composer_notice, 8);
        gtk_widget_set_margin_end(g_messages.composer_notice, 8);
        gtk_box_pack_start(GTK_BOX(conversation_box), g_messages.composer_notice, FALSE, FALSE, 0);

        gtk_box_pack_start(GTK_BOX(conversation_box), composer_box, FALSE, FALSE, 0);
        gtk_stack_add_named(GTK_STACK(g_messages.placeholder_stack), conversation_box, "conversation");

        gtk_paned_pack2(GTK_PANED(paned), g_messages.placeholder_stack, TRUE, FALSE);
        gtk_stack_set_visible_child_name(GTK_STACK(g_messages.placeholder_stack), "placeholder");

        gtk_widget_set_no_show_all(g_messages.banner, TRUE);
        return root;
    }

} // namespace tether::ui
