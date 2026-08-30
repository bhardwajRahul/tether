#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace tether {

    // single-slot OTP vault shared by every transport
    struct Otp {
        std::string code;
        std::string sender_domain; // registrable domain of the source; "" when unknown
        uint64_t id = 0; // 0 == no code
        explicit operator bool() const { return id != 0 && !code.empty(); }
    };

    using OtpClock = std::chrono::steady_clock;

    inline constexpr auto kOtpTtl = std::chrono::minutes(5);

    // Store a code, assign it the next id, drop any previous host claim.
    // sender_domain is the registrable domain of the email sender ("" when the
    // code did not come from email, e.g. Bluetooth/ANCS).
    uint64_t otp_store(const std::string& code,
                       const std::string& sender_domain = "",
                       OtpClock::time_point now = OtpClock::now());

    // Current code if still within the TTL, else {}. Expires the slot as a side effect.
    Otp otp_peek(OtpClock::time_point now = OtpClock::now());

    Otp otp_take_for_host(const std::string& host, OtpClock::time_point now = OtpClock::now());

    // retire a code
    void otp_consume(uint64_t id);

    bool otp_is_claimed();

    void otp_reset(); // tests

    // Best OTP candidate in a short message body or notification, or "" when no OTP.
    std::string otp_extract(const std::string& text);

} // namespace tether
