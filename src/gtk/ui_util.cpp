#include "ui_util.hpp"

namespace tether::ui {

    namespace {
        GtkWidget* g_window = nullptr;
        GtkWidget* g_header_bar = nullptr;

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

.tether-bubble-unconfirmed {
    background-image: none;
    border: 1px dashed alpha(@theme_selected_fg_color, 0.55);
}

.tether-badge {
    background-color: @theme_selected_bg_color;
    color: @theme_selected_fg_color;
    border-radius: 10px;
    padding: 0 8px;
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

} // namespace tether::ui
