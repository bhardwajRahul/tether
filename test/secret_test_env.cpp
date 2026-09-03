#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace {

    // Runs before main, so nothing in this binary can reach the developer's real
    // wallet or home directory. Store paths and the store key both follow $HOME,
    // and a test that forgets to scope it would otherwise write a throwaway key
    // into the user's own keyring or ~/.config/tether. Tests that scope HOME
    // themselves still override this.
    const bool sandboxed = [] {
        setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent/tether-test", 1);

        const auto home = std::filesystem::temp_directory_path() / ("tether-test-home-" + std::to_string(::getpid()));
        std::error_code ec;
        std::filesystem::create_directories(home, ec);
        setenv("HOME", home.c_str(), 1);
        return true;
    }();

} // namespace
