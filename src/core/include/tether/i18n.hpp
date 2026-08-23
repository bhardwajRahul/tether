#pragma once

#include <format>
#include <libintl.h>
#include <string>
#include <string_view>

// Defines the `_` macro, so include this from .cpp files only
#define _(s) gettext(s)
#define N_(s) (s)
#define P_(sing, plur, n) ngettext(sing, plur, static_cast<unsigned long>(n))

namespace tether {

    // Call once. setlocale + bindtextdomain.
    void init_locale();

    // Terminal columns a UTF-8 string occupies. CJK glyphs count as two, so
    // column alignment holds in any script. Falls back to the byte count when
    // the string is not valid in the current LC_CTYPE.
    size_t display_width(const std::string& s);

    // std::format against a runtime format string, so a translation can reorder
    // arguments with {0}/{1}. Returns the format string itself if a translation
    // drops or mangles a placeholder, rather than throwing at the user.
    template <typename... Args> std::string tr_format(std::string_view f, const Args&... args) {
        try {
            return std::vformat(f, std::make_format_args(args...));
        } catch (const std::format_error&) {
            return std::string(f);
        }
    }

} // namespace tether
