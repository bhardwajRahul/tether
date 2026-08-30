#include "tether/otp.hpp"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <regex>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace tether {

    namespace {
        std::mutex g_mutex;
        std::string g_code;
        std::string g_sender_domain; // registrable domain of the code's source; "" = unknown
        uint64_t g_id = 0;
        uint64_t g_next_id = 1;
        std::string g_claimed_host; // empty until some host takes the code
        OtpClock::time_point g_set_at;

        // Caller holds g_mutex.
        void clear_locked() {
            g_code.clear();
            g_sender_domain.clear();
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
            return {g_code, g_sender_domain, g_id};
        }

        std::string normalize_domain(std::string d) {
            std::transform(d.begin(), d.end(), d.begin(), [](unsigned char c) { return std::tolower(c); });
            while (!d.empty() && d.back() == '.')
                d.pop_back();
            return d;
        }

        // Common two-label public suffixes. Without these, "amazon.co.uk" would
        // be reduced to "co.uk" and match every *.co.uk site. Keep this in sync
        // with MULTI_PART_SUFFIXES in extension/src/shared/native.js.
        const std::unordered_set<std::string> kMultiPartSuffixes = {
            "co.uk", "org.uk", "gov.uk", "ac.uk", "net.uk", "me.uk", "ltd.uk", "plc.uk", "sch.uk",
            "com.au", "net.au", "org.au", "edu.au", "gov.au", "asn.au", "id.au",
            "co.jp", "or.jp", "ne.jp", "ac.jp", "go.jp",
            "co.nz", "net.nz", "org.nz", "govt.nz", "ac.nz",
            "com.br", "net.br", "org.br", "gov.br", "edu.br",
            "co.in", "net.in", "org.in", "gen.in", "firm.in", "ind.in",
            "co.kr", "or.kr", "ne.kr", "go.kr", "ac.kr",
            "com.mx", "net.mx", "org.mx", "edu.mx", "gob.mx",
            "com.ar", "net.ar", "org.ar", "gob.ar", "edu.ar",
            "co.za", "org.za", "net.za", "gov.za", "ac.za",
            "com.sg", "net.sg", "org.sg", "gov.sg", "edu.sg",
            "com.hk", "net.hk", "org.hk", "edu.hk", "gov.hk", "idv.hk",
            "com.tw", "net.tw", "org.tw", "edu.tw", "gov.tw",
            "com.cn", "net.cn", "org.cn", "gov.cn", "edu.cn",
            "com.tr", "net.tr", "org.tr", "gov.tr", "edu.tr",
            "com.ua", "net.ua", "org.ua", "gov.ua", "edu.ua",
            "com.ru", "net.ru", "org.ru",
            "co.id", "net.id", "or.id", "go.id", "ac.id",
            "co.th", "in.th", "go.th", "ac.th",
            "com.vn", "net.vn", "org.vn", "gov.vn", "edu.vn",
            "co.il", "org.il", "net.il", "gov.il", "ac.il",
            "com.ph", "net.ph", "org.ph", "gov.ph", "edu.ph",
            "com.my", "net.my", "org.my", "gov.my", "edu.my",
            "com.pk", "net.pk", "org.pk", "gov.pk", "edu.pk",
            "com.eg", "net.eg", "org.eg", "gov.eg", "edu.eg",
            "com.sa", "net.sa", "org.sa", "gov.sa", "edu.sa",
            "com.ng", "net.ng", "org.ng", "gov.ng", "edu.ng",
            "co.ke", "or.ke", "ne.ke", "go.ke", "ac.ke",
            "com.co", "net.co", "org.co", "gov.co", "edu.co",
            "com.pe", "net.pe", "org.pe", "gob.pe", "edu.pe",
            "com.ec", "net.ec", "org.ec", "gob.ec", "edu.ec",
            "com.uy", "net.uy", "org.uy", "gub.uy", "edu.uy",
            "com.ve", "net.ve", "org.ve", "gob.ve", "edu.ve",
            "com.py", "net.py", "org.py", "gov.py", "edu.py",
            "com.bo", "net.bo", "org.bo", "gob.bo", "edu.bo",
            "com.do", "net.do", "org.do", "gov.do", "edu.do",
            "com.gt", "net.gt", "org.gt", "gob.gt", "edu.gt",
            "co.cr", "or.cr", "go.cr", "ac.cr"
        };

        std::vector<std::string> split_labels(const std::string& s) {
            std::vector<std::string> labels;
            std::string cur;
            for (char c : s) {
                if (c == '.') {
                    labels.push_back(cur);
                    cur.clear();
                } else {
                    cur += c;
                }
            }
            labels.push_back(cur);
            return labels;
        }

        bool is_ip_literal(const std::string& d) {
            if (d.find(':') != std::string::npos)
                return true;
            int dots = 0;
            for (unsigned char c : d) {
                if (c == '.') {
                    ++dots;
                    continue;
                }
                if (!std::isdigit(c))
                    return false;
            }
            return dots == 3;
        }

        // eTLD+1 approximation: strip the public suffix and return it plus one label.
        std::string registrable_domain(const std::string& host) {
            const std::string d = normalize_domain(host);
            if (d.empty())
                return d;
            if (is_ip_literal(d))
                return d;

            const std::vector<std::string> labels = split_labels(d);
            if (labels.size() == 1)
                return d;

            const std::string last_two = labels[labels.size() - 2] + "." + labels.back();
            if (kMultiPartSuffixes.count(last_two) != 0) {
                if (labels.size() == 2)
                    return d;
                return labels[labels.size() - 3] + "." + last_two;
            }
            return last_two;
        }

        // A page host matches a pinned sender domain when they share the same
        // registrable domain: "accounts.google.com" matches "google.com", and
        // "www.amazon.co.uk" matches "amazon.co.uk", while "amazon.com.evil.com"
        // and "evilamazon.com" never match "amazon.com". An empty domain means the
        // code is not pinned and any host may claim it.
        bool host_matches_domain(const std::string& host, const std::string& domain) {
            const std::string d = normalize_domain(domain);
            if (d.empty())
                return true;
            return registrable_domain(host) == registrable_domain(d);
        }
    } // namespace

    uint64_t otp_store(const std::string& code, const std::string& sender_domain, OtpClock::time_point now) {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (code.empty()) {
            clear_locked();
            return 0;
        }
        g_code = code;
        g_sender_domain = normalize_domain(sender_domain);
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

        // If the code came from email, only serve it to the domain the email was
        // sent by. A phishing page on a different domain gets nothing.
        if (!host_matches_domain(host, g_sender_domain))
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

    namespace {
        // Words an OTP message uses
        constexpr std::string_view kCues[] = {"code",
                                              "otp",
                                              "one-time",
                                              "one time",
                                              "verification",
                                              "verify",
                                              "passcode",
                                              "password",
                                              "authenticate",
                                              "login",
                                              "log in",
                                              "sign in",
                                              "2fa",
                                              "token",
                                              "pin",
                                              "security",
                                              "confirm",
                                              "do not share",
                                              "expires"};

        std::string to_lower(std::string s) {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
            return s;
        }

        // Distance from `target` to the closest occurrence of `needle`, or npos.
        size_t nearest(const std::string& haystack, std::string_view needle, size_t target) {
            size_t best = std::string::npos;
            for (size_t i = haystack.find(needle); i != std::string::npos; i = haystack.find(needle, i + 1)) {
                size_t d = i > target ? i - target : target - i;
                if (best == std::string::npos || d < best)
                    best = d;
                if (i > target)
                    break; // occurrences are ordered; past the target they only get worse
            }
            return best;
        }

        int score_at(const std::string& lower_text, size_t index) {
            int score = 0;
            for (std::string_view cue : kCues) {
                size_t d = nearest(lower_text, cue, index);
                if (d == std::string::npos)
                    continue;
                score += 10;
                if (d <= 30)
                    score += 20;
            }
            return score;
        }

        bool looks_bogus(const std::string& code, const std::string& lower_context) {
            static const std::regex kYear(R"(^(19|20)\d{2}$)");
            if (std::regex_match(code, kYear))
                return true;
            if (code.size() > 6 && code.front() == '0')
                return true; // phone-like
            if (lower_context.find('$') != std::string::npos || lower_context.find("usd") != std::string::npos)
                return true;
            return lower_context.find("order") != std::string::npos ||
                   lower_context.find("tracking") != std::string::npos;
        }

        std::string strip_separators(std::string s) {
            std::erase_if(s, [](char c) { return c == '-' || c == ' '; });
            return s;
        }
    } // namespace

    std::string otp_extract(const std::string& text) {
        if (text.empty() || text.size() > 4096)
            return {};

        const std::string lower = to_lower(text);
        if (std::none_of(std::begin(kCues), std::end(kCues), [&lower](std::string_view cue) {
                return lower.find(cue) != std::string::npos;
            }))
            return {};

        static const std::regex kPatterns[] = {
            std::regex(R"(\b\d{3}[- ]\d{3}\b)"),
            std::regex(R"(\b\d{4,8}\b)"),
            std::regex(R"(\b(?=[A-Z0-9]*\d)(?=[A-Z0-9]*[A-Z])[A-Z0-9]{6,8}\b)"),
        };

        std::string best;
        int best_score = -1;
        for (const std::regex& pattern : kPatterns) {
            for (auto it = std::sregex_iterator(text.begin(), text.end(), pattern), end = std::sregex_iterator();
                 it != end;
                 ++it) {
                const size_t index = static_cast<size_t>(it->position());
                const size_t length = static_cast<size_t>(it->length());
                const size_t from = index > 40 ? index - 40 : 0;
                const std::string context = lower.substr(from, length + 80);
                const std::string code = strip_separators(it->str());

                if (looks_bogus(code, context))
                    continue;

                int score = score_at(lower, index);
                if (score > best_score) {
                    best_score = score;
                    best = code;
                }
            }
        }
        return best;
    }

} // namespace tether
