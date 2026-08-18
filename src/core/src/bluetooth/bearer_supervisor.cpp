#include "tether/bluetooth/bearer_supervisor.hpp"
#include "tether/log.hpp"

#include <algorithm>

namespace tether::bluetooth {

    namespace {

        int grow_backoff(int current) {
            if (current <= 0)
                return BEARER_BACKOFF_MIN_SECONDS;
            return std::min(current * 2, BEARER_BACKOFF_MAX_SECONDS);
        }

        // Failures that clear on their own, unlike a refusal from the phone.
        // Backing off for minutes on these leaves the link down long after the
        // reason for it is gone (left the room answers with a page timeout, not a rejection).
        bool is_transient(const std::string& err) {
            static const char* const transient[] = {
                "org.bluez.Error.InProgress",
                "already in progress",
                "org.bluez.Error.NotReady",
                "page-timeout",
                "page timeout",
                "connection-abort",
                "Software caused connection abort",
                "Host is down",
                "Connection timed out",
                "br-connection-canceled",
                "le-connection-abort-by-local",
                "br-connection-refused",
            };
            for (const char* needle : transient) {
                if (err.find(needle) != std::string::npos)
                    return true;
            }
            return false;
        }

        int next_backoff(int current, const std::string& err) {
            return is_transient(err) ? BEARER_BACKOFF_MIN_SECONDS : grow_backoff(current);
        }

    } // namespace

    BearerSupervisor::BearerSupervisor(BearerOps& ops, bool ancs_enabled) : ops_(ops), ancs_enabled_(ancs_enabled) {}

    void BearerSupervisor::set_ancs_enabled(bool enabled) { ancs_enabled_ = enabled; }

    void BearerSupervisor::reset() {
        classic_failures_ = 0;
        le_attempts_ = 0;
        classic_connected_since_ = -1;
        next_classic_attempt_ = 0;
        next_le_attempt_ = 0;
        status_.classic_backoff = 0;
        status_.le_backoff = 0;
    }

    bool BearerSupervisor::tick(int64_t now) {
        BearerStatus previous = status_;

        status_.device_present = ops_.device_present();
        status_.device_paired = status_.device_present && ops_.device_paired();
        if (!status_.device_present || !status_.device_paired) {
            status_.classic_connected = false;
            status_.le_connected = false;
            status_.le_available = false;
            classic_connected_since_ = -1;
            status_.reason = status_.device_present ? "iPhone is not paired." : "iPhone not known to BlueZ.";
            return !(status_ == previous);
        }

        status_.classic_connected = ops_.classic_connected();
        status_.le_available = ops_.le_bearer_available();
        status_.le_connected = status_.le_available && ops_.le_connected();
        if (status_.le_connected)
            le_attempts_ = 0;

        if (status_.classic_connected) {
            if (classic_connected_since_ < 0)
                classic_connected_since_ = now;
            status_.classic_backoff = 0;
            classic_failures_ = 0;
        } else {
            // A dropped link invalidates the settle window, so no LE attempt is
            // made until BR/EDR is back and has settled. An LE link that is
            // already up is a separate matter and is reported as it is: ANCS
            // rides LE alone, so calling it down here would switch notification
            // mirroring off for a phone that is still delivering notifications.
            classic_connected_since_ = -1;

            if (now >= next_classic_attempt_) {
                ops_.set_preferred_bearer("bredr");
                std::string err;
                if (ops_.connect_classic(err)) {
                    status_.classic_backoff = 0;
                    next_classic_attempt_ = 0;
                    classic_failures_ = 0;
                } else {
                    ++classic_failures_;
                    status_.classic_backoff = next_backoff(status_.classic_backoff, err);
                    next_classic_attempt_ = now + status_.classic_backoff;
                    debug::log(
                        WARN, "bluetooth: BR/EDR connect failed ({}), retrying in {}s", err, status_.classic_backoff);
                }
            }

            // An iPhone that keeps refusing while unlocked, with its Bluetooth
            // permissions already on, is wedged on its own side: nothing here
            // clears it, and saying "connecting" forever hides that.
            status_.reason = classic_failures_ >= CLASSIC_FAILURES_BEFORE_ADVICE
                                 ? "The iPhone keeps refusing the Bluetooth connection. Turn Bluetooth off and "
                                   "back on on the iPhone — that clears it even when the permissions are already "
                                   "on. Re-pairing is not the fix."
                                 : "Connecting to the iPhone over Bluetooth...";
            return !(status_ == previous);
        }

        // Classic is up. LE is only worth attempting once the ACL has settled.
        if (!ancs_enabled_) {
            status_.reason = "Connected. Notification mirroring is disabled.";
            return !(status_ == previous);
        }

        if (!status_.le_available) {
            status_.reason = "Connected. BlueZ is not exposing an LE bearer, so notifications are unavailable.";
            return !(status_ == previous);
        }

        if (!status_.le_connected) {
            const bool settled =
                classic_connected_since_ >= 0 && now - classic_connected_since_ >= BEARER_SETTLE_SECONDS;
            // Dialling LE from this side only works if the phone is willing to be
            // dialled. When it is not, every attempt either times out or collides
            // with the previous one still running inside bluez, stop after
            // LE_ATTEMPTS_BEFORE_ADVICE and leave it to the phone.
            if (settled && le_attempts_ < LE_ATTEMPTS_BEFORE_ADVICE && now >= next_le_attempt_) {
                // Select LE only for this attempt, then hand the preference back
                // so LE is never left preferred while idle.
                ops_.set_preferred_bearer("le");
                std::string err;
                const bool ok = ops_.connect_le(err);
                ops_.set_preferred_bearer("bredr");
                ++le_attempts_;

                if (ok) {
                    status_.le_backoff = 0;
                    // connect can report success without the link coming up
                    next_le_attempt_ = now + BEARER_BACKOFF_MIN_SECONDS;
                    status_.le_connected = true;
                } else {
                    status_.le_backoff = next_backoff(status_.le_backoff, err);
                    next_le_attempt_ = now + status_.le_backoff;
                    debug::log(WARN, "bluetooth: LE connect failed ({}), retrying in {}s", err, status_.le_backoff);
                }
            }
            if (!status_.le_connected) {
                status_.reason = le_attempts_ >= LE_ATTEMPTS_BEFORE_ADVICE
                                     ? "The iPhone is not opening the LE link that carries notifications. "
                                       "Run \"tether --bt-solicit\" (or press Show iPhone Permissions) to ask it "
                                       "to, then check Settings > Bluetooth > (i) on the iPhone. Messages and "
                                       "contacts are unaffected."
                                     : "Connected. Bringing up the LE link for notifications...";
                return !(status_ == previous);
            }
        }

        status_.reason = "Connected over BR/EDR and LE.";
        return !(status_ == previous);
    }

} // namespace tether::bluetooth
