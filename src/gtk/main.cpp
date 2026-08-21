#include "daemon_client.hpp"
#include "devices_view.hpp"
#include "messages_view.hpp"
#include "notifications_view.hpp"
#include "tray.hpp"
#include "ui_util.hpp"

#include <gtk/gtk.h>
#include <tether/crypto.hpp>

namespace {

    using namespace tether::ui;

    GtkWidget* g_refresh_button = nullptr;
    gboolean g_start_hidden = FALSE;

    void on_visible_view_changed(GObject* stack, GParamSpec*, gpointer) {
        const gchar* name = gtk_stack_get_visible_child_name(GTK_STACK(stack));
        const std::string view = name ? name : "";

        // Refresh means "scan for devices", which is meaningless on the other
        // views, so it only appears where it does something.
        if (g_refresh_button)
            gtk_widget_set_visible(g_refresh_button, view == "devices");

        messages_view_set_visible(view == "messages");
        notifications_view_set_visible(view == "notifications");
    }

    // The tray controls live here
    GtkWidget* create_app_menu_button() {
        GtkWidget* menu = gtk_menu_new();

        GtkWidget* close_to_tray = gtk_check_menu_item_new_with_label("Keep running in tray when closed");
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(close_to_tray), tray_close_to_tray());
        g_signal_connect(close_to_tray,
                         "toggled",
                         G_CALLBACK(+[](GtkCheckMenuItem* item, gpointer) {
                             tray_set_close_to_tray(gtk_check_menu_item_get_active(item));
                         }),
                         nullptr);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), close_to_tray);

        GtkWidget* quit = gtk_menu_item_new_with_label("Quit");
        g_signal_connect(quit,
                         "activate",
                         G_CALLBACK(+[](GtkMenuItem*, gpointer) {
                             if (GApplication* app = g_application_get_default())
                                 g_application_quit(app);
                         }),
                         nullptr);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), quit);
        gtk_widget_show_all(menu);

        GtkWidget* button = gtk_menu_button_new();
        gtk_button_set_image(GTK_BUTTON(button),
                             gtk_image_new_from_icon_name("open-menu-symbolic", GTK_ICON_SIZE_BUTTON));
        gtk_menu_button_set_popup(GTK_MENU_BUTTON(button), menu);
        return button;
    }

    // Hiding leaves the window alive but unmapped, and a second launch re-activates this instance
    gboolean on_window_delete(GtkWidget* window, GdkEvent*, gpointer) {
        if (!tray_close_to_tray())
            return FALSE;
        gtk_widget_hide(window);
        return TRUE;
    }

    void activate(GtkApplication* app, gpointer) {
        if (GtkWidget* existing = main_window()) {
            gtk_window_present(GTK_WINDOW(existing));
            return;
        }

        tether::Crypto::instance().init();
        install_style();

        GtkWidget* window = gtk_application_window_new(app);
        gtk_window_set_title(GTK_WINDOW(window), "Tether");
        gtk_window_set_default_size(GTK_WINDOW(window), 820, 560);
        set_main_window(window);
        g_signal_connect(window, "delete-event", G_CALLBACK(on_window_delete), nullptr);
        tray_init();

        GtkWidget* header_bar = gtk_header_bar_new();
        gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header_bar), TRUE);
        gtk_header_bar_set_title(GTK_HEADER_BAR(header_bar), "Tether");
        gtk_window_set_titlebar(GTK_WINDOW(window), header_bar);
        set_header_bar(header_bar);

        gtk_header_bar_pack_end(GTK_HEADER_BAR(header_bar), create_app_menu_button());

        g_refresh_button = gtk_button_new_from_icon_name("view-refresh-symbolic", GTK_ICON_SIZE_BUTTON);
        g_signal_connect(g_refresh_button,
                         "clicked",
                         G_CALLBACK(+[](GtkWidget*, gpointer) { devices_view_trigger_discovery(); }),
                         nullptr);
        gtk_header_bar_pack_start(GTK_HEADER_BAR(header_bar), g_refresh_button);

        GtkWidget* stack = gtk_stack_new();
        gtk_stack_set_transition_type(GTK_STACK(stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
        gtk_stack_add_titled(GTK_STACK(stack), devices_view_new(), "devices", "Devices");
        gtk_stack_add_titled(GTK_STACK(stack), messages_view_new(), "messages", "Messages");
        gtk_stack_add_titled(GTK_STACK(stack), notifications_view_new(), "notifications", "Notifications");

        GtkWidget* switcher = gtk_stack_switcher_new();
        gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(switcher), GTK_STACK(stack));
        gtk_header_bar_set_custom_title(GTK_HEADER_BAR(header_bar), switcher);

        g_signal_connect(stack, "notify::visible-child-name", G_CALLBACK(on_visible_view_changed), nullptr);

        GtkWidget* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_box_pack_start(GTK_BOX(root), stack, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(root), create_route_bar(), FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(window), root);

        // One feed, dispatched to whichever view owns the event; the views never
        // hold the socket themselves.
        daemon_client_on_disconnect([] {
            devices_view_handle_disconnect();
            messages_view_handle_disconnect();
        });

        daemon_client_start([](const nlohmann::json& event) {
            if (devices_view_handle_event(event))
                return;
            if (messages_view_handle_event(event))
                return;
            notifications_view_handle_event(event);
        });

        devices_view_trigger_discovery();
        devices_view_refresh();

        if (!g_start_hidden)
            gtk_widget_show_all(window);
        gtk_stack_set_visible_child_name(GTK_STACK(stack), "devices");
    }

} // namespace

int main(int argc, char** argv) {
    GtkApplication* app = gtk_application_new("com.tether.desktop", G_APPLICATION_DEFAULT_FLAGS);
    g_application_add_main_option(G_APPLICATION(app),
                                  "tray",
                                  0,
                                  G_OPTION_FLAG_NONE,
                                  G_OPTION_ARG_NONE,
                                  "Start hidden in the system tray",
                                  nullptr);
    g_signal_connect(app,
                     "handle-local-options",
                     G_CALLBACK(+[](GApplication*, GVariantDict* options, gpointer) -> gint {
                         if (g_variant_dict_contains(options, "tray"))
                             g_start_hidden = TRUE;
                         return -1;
                     }),
                     nullptr);
    g_signal_connect(app, "activate", G_CALLBACK(activate), nullptr);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    tether::ui::daemon_client_stop();
    g_object_unref(app);
    return status;
}
