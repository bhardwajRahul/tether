#pragma once

#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

namespace tether::bluetooth {

    // Removes identifying values from text destined for a bug report: Bluetooth
    // addresses (bare, or spelled as a D-Bus device node), phone numbers, email
    // addresses, and the user's home and runtime directories.
    class Redactor {
    public:
        // Registers a literal to scrub wherever it appears
        void hide(const std::string& literal, const std::string& kind);

        std::string text(const std::string& in);

        // Recursively redacts strings
        nlohmann::json value(const nlohmann::json& in);

    private:
        std::string placeholder(const std::string& kind, const std::string& key);

        std::map<std::string, std::string> assigned_;
        // Longest first, so a name that contains another is replaced whole.
        std::vector<std::pair<std::string, std::string>> literals_;
    };

    // True for keys whose values are personal content and must never appear in
    // a report.
    bool is_content_key(const std::string& key);

    // Appends a daemon event to the bounded diagnostic timeline, stamped with
    // milliseconds since the first recorded event.
    void record_diagnostic_event(const nlohmann::json& event);

    // Oldest first
    nlohmann::json diagnostic_timeline();
    void clear_diagnostic_timeline();

    // Assembles a redacted report from the caller's live status payloads.
    nlohmann::json build_diagnostics(const nlohmann::json& status, const nlohmann::json& connection);

} // namespace tether::bluetooth
