#pragma once

#include <cstdlib>
#include <filesystem>
#include <string>

namespace tether::testing {

    // Store paths are derived from $HOME, so the only way to point them at a
    // scratch file is to move HOME for the duration of a test.
    class ScopedEnv {
    public:
        ScopedEnv(const char* name, const std::string& value) : name_(name) {
            if (const char* previous = getenv(name)) {
                previous_ = previous;
                had_previous_ = true;
            }
            setenv(name, value.c_str(), 1);
        }

        ScopedEnv(const char* name, const std::filesystem::path& value) : ScopedEnv(name, value.string()) {}

        ~ScopedEnv() {
            if (had_previous_)
                setenv(name_, previous_.c_str(), 1);
            else
                unsetenv(name_);
        }

        ScopedEnv(const ScopedEnv&) = delete;
        ScopedEnv& operator=(const ScopedEnv&) = delete;

    private:
        const char* name_;
        std::string previous_;
        bool had_previous_ = false;
    };

} // namespace tether::testing
