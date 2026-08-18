#pragma once

#include <gtk/gtk.h>
#include <string>

namespace tether::ui {

    // Loads the application stylesheet. Every view already tags labels with
    // "muted"; until this is installed nothing defines that class, so the tag
    // has no effect at all.
    void install_style();

    std::string escape_markup(const std::string& text);
    void set_markup(GtkWidget* label, const std::string& text);
    void set_text(GtkWidget* label, const std::string& text);
    void clear_list_box(GtkWidget* list_box);

    // Registered once at startup so views can reach the window and the header
    // bar without every one of them holding the whole application struct.
    void set_main_window(GtkWidget* window);
    GtkWidget* main_window();
    void set_header_bar(GtkWidget* bar);
    void set_status_main(const std::string& text);

} // namespace tether::ui
