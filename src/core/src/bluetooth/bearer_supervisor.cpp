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

        // BlueZ is already working on a connect, or the link is already up.
        // Neither is a failure, and retrying at the minimum backoff is what turns
        // one stalled connect into a permanent InProgress storm.
        bool is_already_running(const std::string& err) {
            return err.find("InProgress") != std::string::npos ||
                   err.find("already in progress") != std::string::npos ||
                   err.find("AlreadyConnected") != std::string::npos ||
                   err.find("already-connected") != std::string::npos;
        }

        // Collapses an error a synchronous caller reported into the same shape an
        // asynchronous one returns, so both take one code path.
        ConnectResult classify(ConnectResult reported, const std::string& err) {
            return reported == ConnectResult::Failed && is_already_running(err) ? ConnectResult::Busy : reported;
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
        le_stuck_locally_ = false;
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
        if (status_.le_connected) {
            le_attempts_ = 0;
            le_stuck_locally_ = false;
        }

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
                switch (classify(ops_.connect_classic(err), err)) {
                case ConnectResult::Requested:
                    status_.classic_backoff = 0;
                    next_classic_attempt_ = 0;
                    classic_failures_ = 0;
                    break;
                case ConnectResult::Busy:
                    // Not a refusal and not an attempt. Standing back is the
                    // whole remedy.
                    status_.classic_backoff = grow_backoff(status_.classic_backoff);
                    next_classic_attempt_ = now + status_.classic_backoff;
                    break;
                case ConnectResult::Failed:
                    ++classic_failures_;
                    status_.classic_backoff = next_backoff(status_.classic_backoff, err);
                    next_classic_attempt_ = now + status_.classic_backoff;
                    debug::log(
                        WARN, "bluetooth: BR/EDR connect failed ({}), retrying in {}s", err, status_.classic_backoff);
                    break;
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
                const ConnectResult result = classify(ops_.connect_le(err), err);
                ops_.set_preferred_bearer("bredr");

                switch (result) {
                case ConnectResult::Requested:
                    // BlueZ accepting the request is not a link. Believe only an
                    // observed Connected, and widen the window each time so the
                    // next attempt cannot race a connect still running inside it.
                    ++le_attempts_;
                    le_stuck_locally_ = false;
                    status_.le_backoff = grow_backoff(status_.le_backoff);
                    next_le_attempt_ = now + status_.le_backoff;
                    break;
                case ConnectResult::Busy:
                    // A collision says nothing about the phone, so it selects
                    // different advice -- but it still counts, or a permanently
                    // wedged BlueZ would never reach any advice at all.
                    ++le_attempts_;
                    le_stuck_locally_ = true;
                    status_.le_backoff = grow_backoff(status_.le_backoff);
                    next_le_attempt_ = now + status_.le_backoff;
                    break;
                case ConnectResult::Failed:
                    ++le_attempts_;
                    le_stuck_locally_ = false;
                    status_.le_backoff = next_backoff(status_.le_backoff, err);
                    next_le_attempt_ = now + status_.le_backoff;
                    debug::log(WARN, "bluetooth: LE connect failed ({}), retrying in {}s", err, status_.le_backoff);
                    break;
                }
            }
            if (!status_.le_connected) {
                if (le_attempts_ < LE_ATTEMPTS_BEFORE_ADVICE)
                    status_.reason = "Connected. Bringing up the LE link for notifications...";
                else if (le_stuck_locally_)
                    // Indistinguishable from a phone that declines, so name the
                    // local remedy first and the phone second rather than
                    // sending the user to settings that may be fine already.
                    status_.reason = "Nothing is bringing up the LE link that carries notifications. BlueZ is "
                                     "holding a connect that never completed: run \"sudo systemctl restart "
                                     "bluetooth\" -- disconnecting the iPhone does not clear it. If it still "
                                     "will not come up, run \"tether --bt-solicit\" (or press Show iPhone "
                                     "Permissions) and check Settings > Bluetooth > (i) on the iPhone. Messages "
                                     "and contacts are unaffected.";
                else
                    status_.reason = "The iPhone is not opening the LE link that carries notifications. "
                                     "Run \"tether --bt-solicit\" (or press Show iPhone Permissions) to ask it "
                                     "to, then check Settings > Bluetooth > (i) on the iPhone. Messages and "
                                     "contacts are unaffected.";
                return !(status_ == previous);
            }
        }

        status_.reason = "Connected over BR/EDR and LE.";
        return !(status_ == previous);
    }

} // namespace tether::bluetooth
