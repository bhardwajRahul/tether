#pragma once

#include "tether/bluetooth/objects.hpp"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

// Call control over Bluetooth Hands-Free, in the HF role: this machine is the
// car kit and the iPhone is the audio gateway.
namespace tether::bluetooth {

    class BluezMonitor;

    // Empty when the input cannot be dialled.
    std::string normalize_dial_string(const std::string& input);

    nlohmann::json to_json(const Call& call);
    nlohmann::json to_json(const std::vector<Call>& calls);
    nlohmann::json to_json(const Telephony& gateway);

    // iPhone's audio gateway.
    class TelephonyClient {
    public:
        TelephonyClient(BluezMonitor& monitor, std::string address);

        TelephonyClient(const TelephonyClient&) = delete;
        TelephonyClient& operator=(const TelephonyClient&) = delete;

        // Whether the phone has HFP connected and the gateway is usable.
        bool available() const;

        nlohmann::json status() const;
        nlohmann::json calls() const;

        // The number is normalized here, so callers may pass what the user typed.
        bool dial(const std::string& number, std::string& err);

        // action: answer, hangup, hangup_all, swap, hold_and_answer,
        // release_and_answer, release_and_swap, multiparty.
        // `path` is the call for answer and hangup, and is ignored otherwise.
        bool call_action(const std::string& path, const std::string& action, std::string& err);

        bool send_tones(const std::string& tones, std::string& err);

    private:
        // The gateway for the configured address, or an empty Telephony.
        Telephony gateway() const;
        bool invoke(const std::string& path, const char* iface, const char* method, GVariant* args, std::string& err);

        BluezMonitor* monitor_;
        std::string address_;
    };

} // namespace tether::bluetooth
