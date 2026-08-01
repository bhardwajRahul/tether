#include "tether/otp.hpp"

#include <mutex>

namespace tether {

    namespace {
        std::mutex g_mutex;
        std::string g_code;
        uint64_t g_id = 0;
        uint64_t g_next_id = 1;
        std::string g_claimed_host; // empty until some host takes the code
        OtpClock::time_point g_set_at;

        // Caller holds g_mutex.
        void clear_locked() {
            g_code.clear();
            g_id = 0;
            g_claimed_host.clear();
        }

        // Caller holds g_mutex. Drops the code once it ages past the TTL.
        Otp peek_locked(OtpClock::time_point now) {
            if (g_id == 0)
                return {};
            if (now - g_set_at >= kOtpTtl) {
                clear_locked();
                return {};
            }
            return {g_code, g_id};
        }
    } // namespace

    uint64_t otp_store(const std::string& code, OtpClock::time_point now) {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (code.empty()) {
            clear_locked();
            return 0;
        }
        g_code = code;
        g_id = g_next_id++;
        g_claimed_host.clear();
        g_set_at = now;
        return g_id;
    }

    Otp otp_peek(OtpClock::time_point now) {
        std::lock_guard<std::mutex> lock(g_mutex);
        return peek_locked(now);
    }

    Otp otp_take_for_host(const std::string& host, OtpClock::time_point now) {
        std::lock_guard<std::mutex> lock(g_mutex);
        Otp current = peek_locked(now);
        if (!current)
            return {};

        // An unknown host (older extension that doesn't send url) may take an unclaimed
        // code but never claims it, so it can't lock other sites out.
        if (host.empty())
            return g_claimed_host.empty() ? current : Otp{};

        if (g_claimed_host.empty()) {
            g_claimed_host = host;
            return current;
        }
        return g_claimed_host == host ? current : Otp{};
    }

    void otp_consume(uint64_t id) {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (id == 0 || id == g_id) {
            clear_locked();
        }
        // else: a stale consume for an already-replaced code. Ignore it, otherwise a
        // slow round trip from the previous page wipes the code the user is waiting on.
    }

    bool otp_is_claimed() {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_id != 0 && !g_claimed_host.empty();
    }

    void otp_reset() {
        std::lock_guard<std::mutex> lock(g_mutex);
        clear_locked();
        g_next_id = 1;
    }

} // namespace tether
