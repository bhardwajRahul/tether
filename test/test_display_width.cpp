#include <clocale>
#include <gtest/gtest.h>
#include <string>
#include <tether/i18n.hpp>

namespace {
    // display_width is measured against LC_CTYPE, which the test binary does not
    // inherit from the environment the way the CLI does via init_locale().
    struct Utf8Locale {
        Utf8Locale() { setlocale(LC_CTYPE, "C.UTF-8"); }
        ~Utf8Locale() { setlocale(LC_CTYPE, "C"); }
    };
} // namespace

TEST(DisplayWidth, Ascii) {
    Utf8Locale locale;
    EXPECT_EQ(tether::display_width("Bearer API"), 10u);
    EXPECT_EQ(tether::display_width(""), 0u);
}

TEST(DisplayWidth, MultibyteLatinCountsOneColumnPerGlyph) {
    Utf8Locale locale;
    // "Gerät" is 6 bytes but 5 columns.
    EXPECT_EQ(tether::display_width("Gerät"), 5u);
    EXPECT_EQ(tether::display_width("Přítomné"), 8u);
}

TEST(DisplayWidth, CjkCountsTwoColumnsPerGlyph) {
    Utf8Locale locale;
    EXPECT_EQ(tether::display_width("接続"), 4u);
    EXPECT_EQ(tether::display_width("モード"), 6u);
    // Mixed Latin and CJK add up.
    EXPECT_EQ(tether::display_width("LE 接続"), 7u);
}

TEST(DisplayWidth, InvalidUtf8FallsBackToByteCount) {
    Utf8Locale locale;
    const std::string bad("\xff\xfe", 2);
    EXPECT_EQ(tether::display_width(bad), 2u);
}
