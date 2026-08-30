#include "tether/otp.hpp"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <regex>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "psl_data.hpp"

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

        struct PslSets {
            std::unordered_set<std::string> normal;
            std::unordered_set<std::string> wildcard;
            std::unordered_set<std::string> exception;
        };

        std::string trim(std::string_view v) {
            size_t begin = 0;
            size_t end = v.size();
            while (begin < end && (v[begin] == ' ' || v[begin] == '\t' || v[begin] == '\r'))
                ++begin;
            while (end > begin && (v[end - 1] == ' ' || v[end - 1] == '\t' || v[end - 1] == '\r'))
                --end;
            return std::string(v.substr(begin, end - begin));
        }

        // Built once from the generated Public Suffix List (psl_data.hpp).
        const PslSets& psl_sets() {
            static const PslSets sets = [] {
                PslSets s;
                size_t pos = 0;
                while (pos <= kPslData.size()) {
                    size_t nl = kPslData.find('\n', pos);
                    if (nl == std::string_view::npos)
                        nl = kPslData.size();
                    const std::string rule = trim(kPslData.substr(pos, nl - pos));
                    pos = nl + 1;

                    if (rule.empty())
                        continue;
                    if (rule.front() == '!')
                        s.exception.insert(rule.substr(1));
                    else if (rule.size() >= 2 && rule[0] == '*' && rule[1] == '.')
                        s.wildcard.insert(rule.substr(2));
                    else
                        s.normal.insert(rule);
                }
                return s;
            }();
            return sets;
        }

        std::string join_labels(const std::vector<std::string>& labels, size_t start) {
            std::string out;
            for (size_t i = start; i < labels.size(); ++i) {
                if (i > start)
                    out += '.';
                out += labels[i];
            }
            return out;
        }

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

        // Registrable domain (eTLD+1) per the Public Suffix List prevailing-rule
        // algorithm. Returns "" when there is no registrable label (the input is
        // itself a public suffix, e.g. "com" or "test.ck").
        std::string registrable_domain(const std::string& host) {
            const std::string d = normalize_domain(host);
            if (d.empty() || d.front() == '.')
                return "";
            if (is_ip_literal(d))
                return d;

            const std::vector<std::string> labels = split_labels(d);
            const size_t n = labels.size();
            const PslSets& sets = psl_sets();

            int suffix_start = -1;
            for (size_t i = 0; i < n; ++i) {
                const std::string candidate = join_labels(labels, i);
                if (sets.exception.count(candidate) != 0) {
                    suffix_start = static_cast<int>(i) + 1;
                    break;
                }
                if (sets.normal.count(candidate) != 0) {
                    suffix_start = static_cast<int>(i);
                    break;
                }
                if (i + 1 < n && sets.wildcard.count(join_labels(labels, i + 1)) != 0) {
                    suffix_start = static_cast<int>(i);
                    break;
                }
            }
            if (suffix_start == -1)
                suffix_start = static_cast<int>(n) - 1;
            if (suffix_start == 0)
                return "";
            return join_labels(labels, static_cast<size_t>(suffix_start) - 1);
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
