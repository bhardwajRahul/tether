#include "messages_view.hpp"

#include "daemon_client.hpp"
#include "tray.hpp"
#include "ui_util.hpp"

#include <cstring>
#include <ctime>
#include <gdk/gdkkeysyms.h>
#include <map>
#include <set>
#include <string>
#include <tether/bluetooth/bmessage.hpp>
#include <tether/bluetooth/contacts.hpp>
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
            // A conversation opened from outside the list is a reply that is already under way
            bool focus_composer = false;

            // Addressing a message to someone with no conversation yet.
            bool composing = false;
            GtkWidget* compose_bar = nullptr;
            GtkWidget* compose_entry = nullptr;
            GtkListStore* compose_model = nullptr;
            std::map<std::string, std::string> compose_names;
            std::string compose_requested_key;
        };

        MessagesState g_messages;

        constexpr int SEND_TIMEOUT_SECONDS = 60;

        enum ComposeColumn { COMPOSE_COL_DISPLAY, COMPOSE_COL_ADDRESS, COMPOSE_COL_SEARCH, COMPOSE_COL_COUNT };

        void update_composer_sensitivity();
        void apply_row_selection(GtkWidget* row);
        void clear_selection();
        // `offer_permissions` shows the button that re-solicits the iPhone's
        // Bluetooth toggles, which only helps when that is the actual problem.
        void set_banner(const std::string& text, bool offer_permissions = false);
        std::string composer_text();
        void leave_compose();
        void on_recipient_changed(GtkEditable*, gpointer);

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
            gtk_box_pack_start(GTK_BOX(box), bubble, FALSE, FALSE, 0);

            if (!stamp.empty()) {
                GtkWidget* time_label = gtk_label_new(stamp.c_str());
                gtk_label_set_xalign(GTK_LABEL(time_label), outgoing ? 1.0 : 0.0);
                gtk_style_context_add_class(gtk_widget_get_style_context(time_label), "muted");
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
            // While composing, the selection is driven by the To: field, and the
            // thread being addressed usually has no row yet. Letting the refresh
            // fall through to clear_selection() would wipe a half-typed message
            // every time anything arrived.
            if (reselect) {
                gtk_list_box_select_row(GTK_LIST_BOX(g_messages.thread_list), GTK_LIST_BOX_ROW(reselect));
                if (!g_messages.composing)
                    apply_row_selection(reselect);
            } else if (!g_messages.selected_thread.empty() && !g_messages.composing) {
                clear_selection();
            }

            if (g_messages.thread_selected_handler)
                g_signal_handler_unblock(g_messages.thread_list, g_messages.thread_selected_handler);

            if (g_messages.focus_composer) {
                g_messages.focus_composer = false;
                if (gtk_widget_is_sensitive(g_messages.composer))
                    gtk_widget_grab_focus(g_messages.composer);
            }
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
                reason = g_messages.composing
                             ? (g_messages.selected_block_reason.empty() ? "Enter a recipient."
                                                                         : g_messages.selected_block_reason.c_str())
                             : "Select a conversation first.";
            else if (!g_messages.selected_block_reason.empty())
                reason = g_messages.selected_block_reason.c_str();
            else if (!can_send)
                reason = "Replying to this conversation is not available.";
            gtk_widget_set_tooltip_text(g_messages.send_button, reason);

            if (g_messages.composer_notice) {
                // Why the box is shut.
                const char* notice = nullptr;
                if (reason && !composer_live && !g_messages.sending)
                    notice = reason;
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
                // The conversation now exists under this key, so the thread list
                // refresh that follows will select it like any other.
                leave_compose();
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
            update_composer_sensitivity();
            gtk_stack_set_visible_child_name(GTK_STACK(g_messages.placeholder_stack), "placeholder");
        }

        // Reads the thread identity and reply eligibility the daemon attached to
        // this row. The daemon owns that decision; the UI must not re-derive it.
        void apply_row_selection(GtkWidget* row) {
            const char* thread = (const char*)g_object_get_data(G_OBJECT(row), "thread");
            const char* name = (const char*)g_object_get_data(G_OBJECT(row), "name");
            const char* block_reason = (const char*)g_object_get_data(G_OBJECT(row), "reply_reason");
            g_messages.selected_thread = thread ? thread : "";
            g_messages.selected_name = name ? name : "";
            g_messages.selected_repliable = g_object_get_data(G_OBJECT(row), "repliable") != nullptr;
            g_messages.selected_block_reason = block_reason ? block_reason : "";
            // Which conversation is open is otherwise only legible from the
            // selection highlight in the list beside it.
            set_markup(g_messages.conversation_header,
                       "<b>" +
                           escape_markup(g_messages.selected_name.empty() ? g_messages.selected_thread
                                                                          : g_messages.selected_name) +
                           "</b>");
            update_composer_sensitivity();
        }

        // Matches what GtkEntryCompletion does to the typed key before handing it
        // to the match function, so the two are comparable.
        std::string fold(const std::string& text) {
            gchar* normalized = g_utf8_normalize(text.c_str(), -1, G_NORMALIZE_ALL);
            gchar* folded = g_utf8_casefold(normalized ? normalized : text.c_str(), -1);
            std::string out = folded ? folded : "";
            g_free(normalized);
            g_free(folded);
            return out;
        }

        // Read from disk rather than held: the daemon rewrites this cache after
        // every PBAP sync, and the app outlives several of them.
        // ponytail: a bt_list_contacts command would give the live store instead
        // of the last saved snapshot; add it if the cache proves too stale.
        void rebuild_contact_completion() {
            gtk_list_store_clear(g_messages.compose_model);
            g_messages.compose_names.clear();

            const bluetooth::ContactStore contacts = bluetooth::load_contacts();
            std::set<std::string> seen;
            for (const auto& card : contacts.contacts()) {
                std::vector<std::string> addresses = card.tels;
                addresses.insert(addresses.end(), card.emails.begin(), card.emails.end());
                for (const auto& address : addresses) {
                    // The same validation a typed address gets, so the popup can
                    // never offer something the send path would then refuse.
                    bluetooth::Recipient recipient;
                    std::string err;
                    if (!bluetooth::recipient_from_input(address, recipient, err))
                        continue;
                    const std::string key = bluetooth::thread_key_for(recipient);
                    if (key.empty() || !seen.insert(key).second)
                        continue;
                    if (!card.name.empty())
                        g_messages.compose_names.emplace(key, card.name);

                    const std::string display =
                        card.name.empty() ? recipient.address : card.name + " · " + recipient.address;
                    // The normalized form is in the haystack too, so "5551234567"
                    // finds a contact whose number is stored as "+1 (555) 123-4567".
                    const std::string search =
                        fold(card.name + " " + recipient.address + " " + key.substr(key.find(':') + 1));

                    GtkTreeIter iter;
                    gtk_list_store_append(g_messages.compose_model, &iter);
                    gtk_list_store_set(g_messages.compose_model,
                                       &iter,
                                       COMPOSE_COL_DISPLAY,
                                       display.c_str(),
                                       COMPOSE_COL_ADDRESS,
                                       recipient.address.c_str(),
                                       COMPOSE_COL_SEARCH,
                                       search.c_str(),
                                       -1);
                }
            }
        }

        // Substring, over the name and the number alike. The default match is a
        // prefix test on the display column, which would miss both a surname and
        // any number typed without its formatting.
        gboolean completion_match(GtkEntryCompletion* completion, const gchar* key, GtkTreeIter* iter, gpointer) {
            if (!key || !*key)
                return FALSE;
            GtkTreeModel* model = gtk_entry_completion_get_model(completion);
            gchar* haystack = nullptr;
            gtk_tree_model_get(model, iter, COMPOSE_COL_SEARCH, &haystack, -1);
            const gboolean hit = haystack && std::strstr(haystack, key) != nullptr;
            g_free(haystack);
            return hit;
        }

        // The entry always ends up holding a bare address, never a display name,
        // so there is exactly one path from what is in the box to the thread key
        // and no separate selection to fall out of sync with an edit.
        gboolean on_completion_match_selected(GtkEntryCompletion*, GtkTreeModel* model, GtkTreeIter* iter, gpointer) {
            gchar* address = nullptr;
            gtk_tree_model_get(model, iter, COMPOSE_COL_ADDRESS, &address, -1);
            if (address)
                gtk_entry_set_text(GTK_ENTRY(g_messages.compose_entry), address);
            g_free(address);
            gtk_editable_set_position(GTK_EDITABLE(g_messages.compose_entry), -1);
            return TRUE;
        }

        void on_recipient_changed(GtkEditable*, gpointer) {
            if (!g_messages.composing)
                return;

            const std::string text = gtk_entry_get_text(GTK_ENTRY(g_messages.compose_entry));
            g_messages.selected_thread.clear();
            g_messages.selected_name.clear();
            g_messages.selected_repliable = false;
            g_messages.selected_block_reason.clear();

            bluetooth::Recipient recipient;
            std::string err;
            if (text.empty()) {
                // Nothing typed yet is not an error worth shouting about.
            } else if (bluetooth::recipient_from_input(text, recipient, err)) {
                g_messages.selected_thread = bluetooth::thread_key_for(recipient);
                g_messages.selected_repliable = !g_messages.selected_thread.empty();
                if (auto known = g_messages.compose_names.find(g_messages.selected_thread);
                    known != g_messages.compose_names.end())
                    g_messages.selected_name = known->second;
            } else {
                g_messages.selected_block_reason = err;
            }

            const bool bad = !g_messages.selected_block_reason.empty();
            gtk_entry_set_icon_from_icon_name(
                GTK_ENTRY(g_messages.compose_entry), GTK_ENTRY_ICON_SECONDARY, bad ? "dialog-error-symbolic" : nullptr);
            gtk_entry_set_icon_tooltip_text(GTK_ENTRY(g_messages.compose_entry),
                                            GTK_ENTRY_ICON_SECONDARY,
                                            bad ? g_messages.selected_block_reason.c_str() : nullptr);

            std::string header = "New Message";
            if (!g_messages.selected_name.empty())
                header = g_messages.selected_name;
            else if (!g_messages.selected_thread.empty())
                header = recipient.address;
            set_markup(g_messages.conversation_header, "<b>" + escape_markup(header) + "</b>");

            update_composer_sensitivity();

            // Whatever has already been said to this person belongs above the
            // composer. Only on a change, so this is not a request per keystroke;
            // the daemon serves it from memory either way.
            if (g_messages.selected_thread != g_messages.compose_requested_key) {
                g_messages.compose_requested_key = g_messages.selected_thread;
                clear_list_box(g_messages.conversation);
                gtk_widget_show_all(g_messages.conversation);
                request_messages(g_messages.selected_thread);
            }
        }

        void enter_compose() {
            // Dropping the selection fires row-selected(nullptr), which would run
            // clear_selection() over the state set just below.
            if (g_messages.thread_selected_handler)
                g_signal_handler_block(g_messages.thread_list, g_messages.thread_selected_handler);
            gtk_list_box_unselect_all(GTK_LIST_BOX(g_messages.thread_list));
            if (g_messages.thread_selected_handler)
                g_signal_handler_unblock(g_messages.thread_list, g_messages.thread_selected_handler);

            g_messages.composing = true;
            g_messages.compose_requested_key.clear();
            rebuild_contact_completion();

            clear_list_box(g_messages.conversation);
            gtk_widget_show_all(g_messages.conversation);
            gtk_widget_show(g_messages.compose_bar);
            gtk_stack_set_visible_child_name(GTK_STACK(g_messages.placeholder_stack), "conversation");

            gtk_entry_set_text(GTK_ENTRY(g_messages.compose_entry), "");
            // set_text only emits "changed" when the text actually differs, so the
            // reset state is applied here rather than relied on.
            on_recipient_changed(nullptr, nullptr);
            gtk_widget_grab_focus(g_messages.compose_entry);
        }

        void leave_compose() {
            if (!g_messages.composing)
                return;
            g_messages.composing = false;
            g_messages.compose_requested_key.clear();
            gtk_widget_hide(g_messages.compose_bar);
            gtk_entry_set_icon_from_icon_name(GTK_ENTRY(g_messages.compose_entry), GTK_ENTRY_ICON_SECONDARY, nullptr);
        }

        void on_compose_cancel(GtkWidget*, gpointer) {
            leave_compose();
            clear_selection();
        }

        gboolean on_compose_entry_key(GtkWidget*, GdkEventKey* event, gpointer) {
            if (event->keyval != GDK_KEY_Escape)
                return FALSE;
            on_compose_cancel(nullptr, nullptr);
            return TRUE;
        }

        void on_thread_selected(GtkListBox*, GtkListBoxRow* row, gpointer) {
            leave_compose();
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

    void messages_view_open_thread(const std::string& thread_key) {
        if (thread_key.empty())
            return;
        leave_compose();
        g_messages.selected_thread = thread_key;
        g_messages.selected_name.clear();
        g_messages.selected_repliable = false;
        g_messages.selected_block_reason.clear();
        g_messages.focus_composer = true;
        gtk_stack_set_visible_child_name(GTK_STACK(g_messages.placeholder_stack), "conversation");
        update_composer_sensitivity();
        request_threads();
        request_messages(thread_key);
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
            int unread = 0;
            if (event.contains("threads") && event["threads"].is_array())
                for (const auto& thread : event["threads"])
                    unread += thread.value("unread", 0);
            tray_set_unread(unread);
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
            // thread list is re-read even while hidden for tray unread count
            request_threads();
            if (!g_messages.visible)
                return true;
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

        GtkWidget* thread_side = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        GtkWidget* new_message = gtk_button_new_with_label("New Message");
        gtk_button_set_image(GTK_BUTTON(new_message),
                             gtk_image_new_from_icon_name("list-add-symbolic", GTK_ICON_SIZE_BUTTON));
        gtk_button_set_always_show_image(GTK_BUTTON(new_message), TRUE);
        gtk_widget_set_margin_top(new_message, 8);
        gtk_widget_set_margin_bottom(new_message, 8);
        gtk_widget_set_margin_start(new_message, 8);
        gtk_widget_set_margin_end(new_message, 8);
        g_signal_connect(new_message, "clicked", G_CALLBACK(+[](GtkWidget*, gpointer) { enter_compose(); }), nullptr);
        gtk_box_pack_start(GTK_BOX(thread_side), new_message, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(thread_side), thread_scroll, TRUE, TRUE, 0);
        gtk_paned_pack1(GTK_PANED(paned), thread_side, FALSE, FALSE);

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

        g_messages.compose_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_container_set_border_width(GTK_CONTAINER(g_messages.compose_bar), 8);
        gtk_box_pack_start(GTK_BOX(g_messages.compose_bar), gtk_label_new("To:"), FALSE, FALSE, 0);

        g_messages.compose_entry = gtk_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(g_messages.compose_entry), "Phone number or email");
        // Lets the entry shrink with the pane. At its natural minimum the Cancel
        // button beside it is pushed off the edge of a narrow window.
        gtk_entry_set_width_chars(GTK_ENTRY(g_messages.compose_entry), 12);
        gtk_box_pack_start(GTK_BOX(g_messages.compose_bar), g_messages.compose_entry, TRUE, TRUE, 0);

        g_messages.compose_model = gtk_list_store_new(COMPOSE_COL_COUNT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
        GtkEntryCompletion* completion = gtk_entry_completion_new();
        gtk_entry_completion_set_model(completion, GTK_TREE_MODEL(g_messages.compose_model));
        gtk_entry_completion_set_text_column(completion, COMPOSE_COL_DISPLAY);
        gtk_entry_completion_set_match_func(completion, completion_match, nullptr, nullptr);
        gtk_entry_completion_set_minimum_key_length(completion, 1);
        // Inline completion would rewrite a half-typed number into whichever
        // contact happens to start with those digits.
        gtk_entry_completion_set_inline_completion(completion, FALSE);
        g_signal_connect(completion, "match-selected", G_CALLBACK(on_completion_match_selected), nullptr);
        gtk_entry_set_completion(GTK_ENTRY(g_messages.compose_entry), completion);
        g_object_unref(completion);

        GtkWidget* compose_cancel = gtk_button_new_with_label("Cancel");
        g_signal_connect(compose_cancel, "clicked", G_CALLBACK(on_compose_cancel), nullptr);
        gtk_box_pack_start(GTK_BOX(g_messages.compose_bar), compose_cancel, FALSE, FALSE, 0);

        g_signal_connect(g_messages.compose_entry, "changed", G_CALLBACK(on_recipient_changed), nullptr);
        // Enter in the To: field means "done addressing"; the body is empty and
        // sending from here would be an accident every time. Connected after the
        // completion, so a popup selection consumes the key first.
        g_signal_connect(g_messages.compose_entry,
                         "activate",
                         G_CALLBACK(+[](GtkEntry*, gpointer) {
                             if (gtk_widget_is_sensitive(g_messages.composer))
                                 gtk_widget_grab_focus(g_messages.composer);
                         }),
                         nullptr);
        g_signal_connect_after(g_messages.compose_entry, "key-press-event", G_CALLBACK(on_compose_entry_key), nullptr);

        // The children are shown once here, then the bar itself is toggled. A
        // no-show-all container is skipped by gtk_widget_show_all outright, so
        // showing it later has to be gtk_widget_show on an already-built tree.
        gtk_box_pack_start(GTK_BOX(conversation_box), g_messages.compose_bar, FALSE, FALSE, 0);
        gtk_widget_show_all(g_messages.compose_bar);
        gtk_widget_hide(g_messages.compose_bar);
        gtk_widget_set_no_show_all(g_messages.compose_bar, TRUE);

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
