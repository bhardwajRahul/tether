#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace tether {

    // A mirrored notification as the desktop should present it.
    struct NotificationSpec {
        // The originating iPhone app. Shown as the popup's application name
        std::string app_name;
        std::string summary;
        std::string body;
        // Freedesktop icon names, best first.
        std::vector<std::string> icons;
        bool quiet = false;
    };

    class DesktopNotifier {
    public:
        DesktopNotifier();
        ~DesktopNotifier();

        DesktopNotifier(const DesktopNotifier&) = delete;
        DesktopNotifier& operator=(const DesktopNotifier&) = delete;

        bool init();
        void notify_file_arrived(const std::filesystem::path& path);

        // A plain notification with no actions.
        void notify(const NotificationSpec& spec);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace tether
