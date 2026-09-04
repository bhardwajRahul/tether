#include "ui_util.hpp"

#include "tray.hpp"

#include <tether/i18n.hpp>
#include <tether/version.hpp>

namespace tether::ui {

    namespace {
        GtkWidget* g_window = nullptr;
        GtkWidget* g_header_bar = nullptr;

        struct RouteIndicator {
            GtkWidget* box = nullptr;
            GtkWidget* icon = nullptr;
            GtkWidget* label = nullptr;
            const char* name = nullptr;
            const char* icon_ok = nullptr;
            const char* icon_off = nullptr;
        };

        RouteIndicator g_routes[2];

        RouteIndicator& indicator(Route route) { return g_routes[route == Route::WiFi ? 0 : 1]; }

        constexpr const char* STYLE = R"CSS(
.muted {
    opacity: 0.62;
    font-size: 90%;
}

.tether-bubble {
    padding: 8px 12px;
    border-radius: 14px;
}

.tether-bubble-in {
    background-color: alpha(@theme_fg_color, 0.10);
}

.tether-bubble-out {
    background-color: @theme_selected_bg_color;
    color: @theme_selected_fg_color;
}

.tether-route-bar {
    border-top: 1px solid alpha(@theme_fg_color, 0.12);
}

.tether-route-off {
    opacity: 0.5;
}

.tether-setup {
    background-color: alpha(@theme_fg_color, 0.07);
    border: 1px solid alpha(@theme_fg_color, 0.18);
    border-radius: 8px;
    padding: 12px;
}

.tether-setup-command {
    font-family: monospace;
    font-size: 92%;
}

.tether-thread-unread {
    opacity: 1;
    font-weight: bold;
}

.tether-send-error {
    background-color: alpha(#e5a50a, 0.20);
    border-top: 1px solid alpha(@theme_fg_color, 0.12);
}

.tether-badge {
    background-color: @theme_selected_bg_color;
    color: @theme_selected_fg_color;
    border-radius: 10px;
    padding: 0 8px;
}

.tether-dropzone {
    border: 2px dashed alpha(@theme_fg_color, 0.28);
    border-radius: 12px;
    padding: 20px 28px;
}

.tether-dropzone-active {
    border-color: @theme_selected_bg_color;
    background-color: alpha(@theme_selected_bg_color, 0.12);
}
)CSS";
    } // namespace

    void install_style() {
        GdkScreen* screen = gdk_screen_get_default();
        if (!screen)
            return;

        GtkCssProvider* provider = gtk_css_provider_new();
        GError* error = nullptr;
        if (!gtk_css_provider_load_from_data(provider, STYLE, -1, &error)) {
            g_warning("tether: stylesheet rejected: %s", error ? error->message : "unknown");
            g_clear_error(&error);
        }
        gtk_style_context_add_provider_for_screen(
            screen, GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(provider);
    }

    std::string escape_markup(const std::string& text) {
        gchar* escaped = g_markup_escape_text(text.c_str(), -1);
        std::string result = escaped ? escaped : "";
        g_free(escaped);
        return result;
    }

    void set_markup(GtkWidget* label, const std::string& text) {
        if (label) {
            gtk_label_set_markup(GTK_LABEL(label), text.c_str());
        }
    }

    void set_text(GtkWidget* label, const std::string& text) {
        if (label) {
            gtk_label_set_text(GTK_LABEL(label), text.c_str());
        }
    }

    void clear_list_box(GtkWidget* list_box) {
        GList* children = gtk_container_get_children(GTK_CONTAINER(list_box));
        for (GList* iter = children; iter; iter = g_list_next(iter)) {
            gtk_widget_destroy(GTK_WIDGET(iter->data));
        }
        g_list_free(children);
    }

    void set_main_window(GtkWidget* window) { g_window = window; }

    GtkWidget* main_window() { return g_window; }

    void set_header_bar(GtkWidget* bar) { g_header_bar = bar; }

    void set_status_main(const std::string& text) {
        if (g_header_bar) {
            gtk_header_bar_set_subtitle(GTK_HEADER_BAR(g_header_bar), text.c_str());
        }
    }

    GtkWidget* create_route_bar() {
        GtkWidget* bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
        gtk_container_set_border_width(GTK_CONTAINER(bar), 6);
        gtk_style_context_add_class(gtk_widget_get_style_context(bar), "tether-route-bar");

        auto build = [](RouteIndicator& route, const char* name, const char* icon_ok, const char* icon_off) {
            route.icon_ok = icon_ok;
            route.icon_off = icon_off;
            route.name = name;
            route.box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
            route.icon = gtk_image_new_from_icon_name(icon_off, GTK_ICON_SIZE_MENU);
            gtk_box_pack_start(GTK_BOX(route.box), route.icon, FALSE, FALSE, 0);
            route.label = gtk_label_new(nullptr);
            gtk_label_set_ellipsize(GTK_LABEL(route.label), PANGO_ELLIPSIZE_END);
            gtk_box_pack_start(GTK_BOX(route.box), route.label, FALSE, FALSE, 0);
        };

        build(indicator(Route::WiFi),
              "Wi-Fi",
              "network-wireless-signal-excellent-symbolic",
              "network-wireless-offline-symbolic");
        build(indicator(Route::Bluetooth), "Bluetooth", "bluetooth-active-symbolic", "bluetooth-disabled-symbolic");

        gtk_box_pack_start(GTK_BOX(bar), indicator(Route::WiFi).box, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(bar), indicator(Route::Bluetooth).box, FALSE, FALSE, 0);

        GtkWidget* version = gtk_label_new(TETHER_VERSION);
        gtk_style_context_add_class(gtk_widget_get_style_context(version), "muted");
        gtk_widget_set_tooltip_text(version, _("Tether version"));
        gtk_box_pack_end(GTK_BOX(bar), version, FALSE, FALSE, 0);

        set_route_status(Route::WiFi, false, _("Waiting for the Tether daemon."));
        set_route_status(Route::Bluetooth, false, _("Waiting for the Tether daemon."));
        return bar;
    }

    void set_route_status(Route route, bool ok, const std::string& detail) {
        RouteIndicator& r = indicator(route);
        if (!r.box)
            return;

        gtk_image_set_from_icon_name(GTK_IMAGE(r.icon), ok ? r.icon_ok : r.icon_off, GTK_ICON_SIZE_MENU);
        GtkStyleContext* context = gtk_widget_get_style_context(r.box);
        if (ok)
            gtk_style_context_remove_class(context, "tether-route-off");
        else
            gtk_style_context_add_class(context, "tether-route-off");

        // TRANSLATORS: {} is a transport name, "Wi-Fi" or "Bluetooth".
        const std::string state = tr_format(_("{}: {}"), r.name, ok ? _("connected") : _("not connected"));
        set_text(r.label, state);
        // The reason can be a sentence or two, which would push the other route
        // off the strip, so it lives in the tooltip. The Devices page shows it
        // in full.
        gtk_widget_set_tooltip_text(r.box, detail.empty() ? state.c_str() : (state + "\n" + detail).c_str());

        tray_set_route(route, ok, detail);
    }

} // namespace tether::ui
