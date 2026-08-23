#include "tether/i18n.hpp"

#include <clocale>
#include <cstdlib>
#include <cwchar>
#include <wchar.h>

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

    size_t display_width(const std::string& s) {
        std::mbstate_t st{};
        const char* p = s.c_str();
        const size_t n = std::mbsrtowcs(nullptr, &p, 0, &st);
        if (n == static_cast<size_t>(-1))
            return s.size();

        std::wstring w(n, L'\0');
        p = s.c_str();
        st = {};
        std::mbsrtowcs(w.data(), &p, n, &st);
        const int width = wcswidth(w.c_str(), n);
        return width < 0 ? s.size() : static_cast<size_t>(width);
    }

} // namespace tether
