#include "tether/i18n.hpp"

#include <clocale>
#include <cstdlib>

namespace tether {

    void init_locale() {
        setlocale(LC_ALL, "");
        // The wire protocol and JSON must keep '.' as the decimal separator
        setlocale(LC_NUMERIC, "C");

        // Uninstalled runs point this at the build tree.
        const char* dir = std::getenv("TETHER_LOCALEDIR");
        bindtextdomain(TETHER_GETTEXT_DOMAIN, (dir && *dir) ? dir : TETHER_LOCALEDIR);
        bind_textdomain_codeset(TETHER_GETTEXT_DOMAIN, "UTF-8");
        textdomain(TETHER_GETTEXT_DOMAIN);
    }

} // namespace tether
