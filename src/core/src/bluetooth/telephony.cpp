#include "tether/bluetooth/telephony.hpp"
#include "tether/bluetooth/monitor.hpp"
#include "tether/log.hpp"

#include <tether/i18n.hpp>

#include <algorithm>
#include <cctype>

namespace tether::bluetooth {

    namespace {

        constexpr const char* BLUEZ_NAME = "org.bluez";
        constexpr const char* IFACE_TELEPHONY = "org.bluez.Telephony1";
        constexpr const char* IFACE_CALL = "org.bluez.Call1";

        constexpr int CALL_TIMEOUT_MS = 10000;

        constexpr size_t MAX_DIAL_DIGITS = 32;
        constexpr size_t MAX_TONES = 32;

        bool iequals(const std::string& a, const std::string& b) {
            return a.size() == b.size() &&
                   std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
                       return std::tolower(x) == std::tolower(y);
                   });
        }

    } // namespace

    std::string normalize_dial_string(const std::string& input) {
        std::string out;
        bool seen_digit = false;
        for (const char raw : input) {
            const unsigned char c = static_cast<unsigned char>(raw);
            if (c == '+') {
                // Only as the country-code prefix
                if (!out.empty())
                    return {};
                out.push_back('+');
                continue;
            }
            if (std::isdigit(c)) {
                out.push_back(static_cast<char>(c));
                seen_digit = true;
                continue;
            }
            if (c == ' ' || c == '-' || c == '(' || c == ')' || c == '.' || c == '\t')
                continue;
            return {};
        }
        if (!seen_digit)
            return {};
        const size_t digits = out.size() - (out[0] == '+' ? 1 : 0);
        return digits > MAX_DIAL_DIGITS ? std::string{} : out;
    }

    nlohmann::json to_json(const Call& call) {
        nlohmann::json j;
        j["path"] = call.path;
        j["number"] = call.withheld() ? "" : call.number;
        j["withheld"] = call.withheld();
        j["name"] = call.name;
        j["incoming_line"] = call.incoming_line;
        j["state"] = call.state;
        j["multiparty"] = call.multiparty;
        j["ringing"] = call.ringing();
        j["outgoing"] = call.outgoing();
        j["connected"] = call.connected();
        return j;
    }

    nlohmann::json to_json(const std::vector<Call>& calls) {
        nlohmann::json out = nlohmann::json::array();
        for (const auto& call : calls)
            out.push_back(to_json(call));
        return out;
    }

    nlohmann::json to_json(const Telephony& gateway) {
        nlohmann::json j;
        j["available"] = gateway.ready();
        j["path"] = gateway.path;
        j["state"] = gateway.state;
        j["operator"] = gateway.operator_name;
        j["signal"] = gateway.signal;
        j["battery"] = gateway.battery;
        j["service"] = gateway.service;
        j["roaming"] = gateway.roaming;
        j["inband_ringtone"] = gateway.inband_ringtone;
        return j;
    }

    TelephonyClient::TelephonyClient(BluezMonitor& monitor, std::string address)
        : monitor_(&monitor), address_(std::move(address)) {}

    Telephony TelephonyClient::gateway() const {
        const BluezObjects objects = monitor_->snapshot();
        for (const auto& device : objects.devices) {
            if (!iequals(device.address, address_))
                continue;
            if (const Telephony* found = objects.find_telephony(device.path))
                return *found;
        }
        return {};
    }

    bool TelephonyClient::available() const { return gateway().ready(); }

    nlohmann::json TelephonyClient::status() const {
        const Telephony gw = gateway();
        nlohmann::json j = to_json(gw);
        j["address"] = address_;
        j["calls"] = monitor_->snapshot().calls_for(gw.path).size();
        if (gw.path.empty())
            j["reason"] = _("The iPhone has not connected Hands-Free to this computer.");
        else if (!gw.ready())
            j["reason"] = _("Connecting to the iPhone's Hands-Free service.");
        else
            j["reason"] = _("Calls are controlled here; the audio plays on the iPhone.");
        return j;
    }

    nlohmann::json TelephonyClient::calls() const { return to_json(monitor_->snapshot().calls_for(gateway().path)); }

    bool TelephonyClient::invoke(
        const std::string& path, const char* iface, const char* method, GVariant* args, std::string& err) {
        GDBusConnection* conn = monitor_->connection();
        if (!conn || path.empty()) {
            // Consumes the floating reference the caller built.
            if (args)
                g_variant_unref(g_variant_ref_sink(args));
            err = _("The iPhone has not connected Hands-Free to this computer.");
            return false;
        }

        GError* error = nullptr;
        GVariant* reply = g_dbus_connection_call_sync(conn,
                                                      BLUEZ_NAME,
                                                      path.c_str(),
                                                      iface,
                                                      method,
                                                      args,
                                                      nullptr,
                                                      G_DBUS_CALL_FLAGS_NONE,
                                                      CALL_TIMEOUT_MS,
                                                      nullptr,
                                                      &error);
        if (!reply) {
            err = error ? error->message : "unknown error";
            g_clear_error(&error);
            debug::log(WARN, "telephony: {} failed: {}", method, err);
            return false;
        }
        g_variant_unref(reply);
        return true;
    }

    bool TelephonyClient::dial(const std::string& number, std::string& err) {
        const std::string dialable = normalize_dial_string(number);
        if (dialable.empty()) {
            err = _("Not a dialable number.");
            return false;
        }
        const std::string uri = "tel:" + dialable;
        return invoke(gateway().path, IFACE_TELEPHONY, "Dial", g_variant_new("(s)", uri.c_str()), err);
    }

    bool TelephonyClient::call_action(const std::string& path, const std::string& action, std::string& err) {
        const Telephony gw = gateway();
        const std::vector<Call> live = monitor_->snapshot().calls_for(gw.path);

        if (action == "answer" || action == "hangup") {
            std::string target = path;
            if (target.empty() && live.size() == 1)
                target = live.front().path;
            if (target.empty()) {
                err = _("No call named.");
                return false;
            }
            if (std::none_of(live.begin(), live.end(), [&](const Call& c) { return c.path == target; })) {
                err = _("That call is no longer active.");
                return false;
            }
            return invoke(target, IFACE_CALL, action == "answer" ? "Answer" : "Hangup", nullptr, err);
        }

        static const std::pair<const char*, const char*> gateway_actions[] = {
            {"hangup_all", "HangupAll"},
            {"swap", "SwapCalls"},
            {"hold_and_answer", "HoldAndAnswer"},
            {"release_and_answer", "ReleaseAndAnswer"},
            {"release_and_swap", "ReleaseAndSwap"},
            {"multiparty", "CreateMultiparty"},
        };
        for (const auto& [name, method] : gateway_actions)
            if (action == name)
                return invoke(gw.path, IFACE_TELEPHONY, method, nullptr, err);

        err = _("Unknown call action.");
        return false;
    }

    bool TelephonyClient::send_tones(const std::string& tones, std::string& err) {
        if (tones.empty() || tones.size() > MAX_TONES || !std::all_of(tones.begin(), tones.end(), [](unsigned char c) {
                return std::isdigit(c) || c == '*' || c == '#' || (c >= 'A' && c <= 'D');
            })) {
            err = _("Not a DTMF sequence.");
            return false;
        }
        return invoke(gateway().path, IFACE_TELEPHONY, "SendTones", g_variant_new("(s)", tones.c_str()), err);
    }

} // namespace tether::bluetooth
