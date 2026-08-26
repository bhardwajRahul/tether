#include "tether/bluetooth/bmessage.hpp"
#include "tether/bluetooth/vcard.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace tether::bluetooth {

    namespace {

        std::string trim(const std::string& text) {
            size_t begin = text.find_first_not_of(" \t\r\n");
            if (begin == std::string::npos)
                return {};
            size_t end = text.find_last_not_of(" \t\r\n");
            return text.substr(begin, end - begin + 1);
        }

        std::string upper(std::string text) {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
                return static_cast<char>(std::toupper(c));
            });
            return text;
        }

        // Splits on CRLF, LF, or a bare CR, so a payload with any of the three
        // parses the same way.
        std::vector<std::string> split_lines(const std::string& text) {
            std::vector<std::string> lines;
            size_t start = 0;
            while (start <= text.size()) {
                size_t nl = text.find_first_of("\r\n", start);
                if (nl == std::string::npos) {
                    lines.push_back(text.substr(start));
                    break;
                }
                lines.push_back(text.substr(start, nl - start));
                start = nl + (text[nl] == '\r' && nl + 1 < text.size() && text[nl + 1] == '\n' ? 2 : 1);
            }
            return lines;
        }

        // The vCards embedded in a bMessage are the same grammar PBAP delivers,
        // so both share one implementation.
        using tether::bluetooth::vcard_name_from_n;
        using tether::bluetooth::vcard_split_property;

        // iOS tags address values with a one-character scheme prefix and a colon
        // ("TEL:7:+15035551234"). Neither a phone number nor an email address can
        // contain a colon, so a colon in second position is always such a tag.
        std::string strip_scheme_tag(const std::string& value) {
            return value.size() > 2 && value[1] == ':' ? value.substr(2) : value;
        }

        // Reads one BEGIN:VCARD..END:VCARD block starting at `index`, which must
        // point at the BEGIN line. Leaves `index` on the END line.
        VCardParty read_vcard(const std::vector<std::string>& lines, size_t& index) {
            VCardParty party;
            std::string structured_name;

            for (++index; index < lines.size(); ++index) {
                const std::string line = trim(lines[index]);
                if (upper(line) == "END:VCARD")
                    break;

                std::string name, value;
                if (!vcard_split_property(line, name, value) || value.empty())
                    continue;

                if (name == "TEL" && party.tel.empty())
                    party.tel = strip_scheme_tag(value);
                else if (name == "EMAIL" && party.email.empty())
                    party.email = strip_scheme_tag(value);
                else if (name == "FN")
                    party.name = value;
                else if (name == "N")
                    structured_name = vcard_name_from_n(value);
            }

            if (party.name.empty())
                party.name = structured_name;
            return party;
        }

        // True when the line is a structural token possibly already behind some
        // stuffing spaces. Looking past every leading space, rather than just
        // one, is what makes stuffing and unstuffing exact inverses: a body line
        // of " BEGIN:MSG" is stuffed to "  BEGIN:MSG" and has to come back with
        // its own space intact.
        bool is_stuffable_token(const std::string& line) {
            size_t at = line.find_first_not_of(' ');
            if (at == std::string::npos)
                return false;
            const std::string token = upper(line.substr(at));
            return token.rfind("BEGIN:", 0) == 0 || token.rfind("END:", 0) == 0;
        }

        // MAP byte-stuffing: a body line that would otherwise look like a
        // structural token is prefixed with a space by the sender.
        std::string unstuff(const std::string& line) {
            if (!line.empty() && line[0] == ' ' && is_stuffable_token(line))
                return line.substr(1);
            return line;
        }

    } // namespace

    BMessage parse_bmessage(const std::string& text) {
        BMessage message;
        if (text.empty())
            return message;

        const std::vector<std::string> lines = split_lines(text);

        bool seen_begin = false;
        int envelope_depth = 0;
        bool in_body = false;
        bool in_msg = false;
        std::vector<std::string> body_lines;

        for (size_t i = 0; i < lines.size(); ++i) {
            const std::string raw = lines[i];
            const std::string line = trim(raw);
            const std::string token = upper(line);

            // Inside MSG everything is payload until the closing token, so the
            // body is captured before any structural interpretation.
            //
            // The terminator must be matched against the *raw* line: byte-stuffing
            // marks a literal "END:MSG" in the body with a leading space, and
            // trimming first would make it end the body instead.
            if (in_msg) {
                const bool stuffed = !raw.empty() && raw[0] == ' ';
                if (!stuffed && token == "END:MSG") {
                    in_msg = false;
                    continue;
                }
                body_lines.push_back(unstuff(raw));
                continue;
            }

            if (token == "BEGIN:BMSG") {
                seen_begin = true;
                continue;
            }
            if (token == "END:BMSG")
                break;
            if (token == "BEGIN:BENV") {
                ++envelope_depth;
                continue;
            }
            if (token == "END:BENV") {
                if (envelope_depth > 0)
                    --envelope_depth;
                continue;
            }
            if (token == "BEGIN:BBODY") {
                in_body = true;
                continue;
            }
            if (token == "END:BBODY") {
                in_body = false;
                continue;
            }
            if (token == "BEGIN:MSG") {
                in_msg = true;
                continue;
            }

            if (token == "BEGIN:VCARD") {
                VCardParty party = read_vcard(lines, i);
                // The originator sits outside BENV; recipients sit inside it.
                if (envelope_depth == 0) {
                    if (message.originator.empty())
                        message.originator = party;
                } else if (!party.empty()) {
                    message.recipients.push_back(std::move(party));
                }
                continue;
            }

            if (in_body)
                continue;

            std::string name, value;
            if (!vcard_split_property(line, name, value))
                continue;
            if (name == "STATUS")
                message.read = upper(value) == "READ";
            else if (name == "TYPE")
                message.type = value;
            else if (name == "FOLDER")
                message.folder = value;
        }

        // Join without a trailing newline; the body is displayed, not re-emitted.
        for (size_t i = 0; i < body_lines.size(); ++i) {
            if (i)
                message.body += "\n";
            message.body += body_lines[i];
        }
        // Trailing blank lines are an artifact of the container, not content.
        while (!message.body.empty() && (message.body.back() == '\n' || message.body.back() == '\r'))
            message.body.pop_back();

        message.valid = seen_begin;
        return message;
    }

    std::string normalize_phone(const std::string& raw) {
        std::string out;
        bool first = true;
        for (char c : raw) {
            if (std::isdigit(static_cast<unsigned char>(c))) {
                out += c;
            } else if (c == '+' && first && out.empty()) {
                out += c;
            }
            if (!std::isspace(static_cast<unsigned char>(c)))
                first = false;
        }
        return out;
    }

    std::string normalize_email(const std::string& raw) {
        std::string out = trim(raw);
        std::transform(
            out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return out;
    }

    std::string thread_key_for(const VCardParty& party) {
        // A phone number wins when both are present: it is what the phone routes
        // SMS on, and it is the more stable of the two.
        if (!party.tel.empty()) {
            std::string tel = normalize_phone(party.tel);
            if (!tel.empty())
                return "tel:" + tel;
        }
        if (!party.email.empty()) {
            std::string email = normalize_email(party.email);
            if (!email.empty())
                return "email:" + email;
        }
        return {};
    }

    namespace {
        constexpr size_t TEL_SUFFIX_DIGITS = 10;
    } // namespace

    std::string tel_suffix(const std::string& raw) {
        std::string digits;
        for (char c : raw) {
            if (std::isdigit(static_cast<unsigned char>(c)))
                digits += c;
        }
        return digits.size() < TEL_SUFFIX_DIGITS ? std::string{} : digits.substr(digits.size() - TEL_SUFFIX_DIGITS);
    }

    std::string thread_bucket(const std::string& thread_key) {
        if (thread_key.rfind("tel:", 0) != 0)
            return thread_key;
        const std::string number = thread_key.substr(4);
        std::string suffix = tel_suffix(number);
        if (suffix.empty()) {
            for (char c : number) {
                if (std::isdigit(static_cast<unsigned char>(c)))
                    suffix += c;
            }
        }
        return suffix.empty() ? thread_key : "tel:" + suffix;
    }

    namespace {

        // Anything below 0x20, plus DEL. CR and LF are the injection vector, but
        // no control character has a legitimate place in an address.
        bool has_control_character(const std::string& text) {
            for (unsigned char c : text) {
                if (c < 0x20 || c == 0x7f)
                    return true;
            }
            return false;
        }

        // ';' separates vCard parameters, ':' ends the property name, ',' and '\'
        // are value delimiters and the escape character.
        bool has_vcard_delimiter(const std::string& text) { return text.find_first_of(";:,\\") != std::string::npos; }

        // Longer than any real address; a bound keeps a pathological value from
        // reaching the phone at all.
        constexpr size_t MAX_ADDRESS_LENGTH = 254;

        // A line already beginning with spaces is stuffed again, so a body
        // containing " BEGIN:X" survives the receiver's unstuffing instead of
        // silently losing its leading space.
        bool line_needs_stuffing(const std::string& line) { return is_stuffable_token(line); }

        const char* env_or(const char* name, const char* fallback) {
            const char* value = std::getenv(name);
            return value && *value ? value : fallback;
        }

    } // namespace

    bool validate_recipient(const Recipient& recipient, std::string& err_out) {
        const std::string& address = recipient.address;

        if (address.empty()) {
            err_out = "Recipient address is empty.";
            return false;
        }
        if (address.size() > MAX_ADDRESS_LENGTH) {
            err_out = "Recipient address is too long.";
            return false;
        }
        if (has_control_character(address)) {
            err_out = "Recipient address contains a control character.";
            return false;
        }
        if (has_vcard_delimiter(address)) {
            err_out = "Recipient address contains a vCard delimiter.";
            return false;
        }

        if (recipient.kind == RecipientKind::Tel) {
            bool digit = false;
            for (char c : address) {
                if (std::isdigit(static_cast<unsigned char>(c))) {
                    digit = true;
                    continue;
                }
                // The punctuation people actually write numbers with, and nothing
                // else. '+' is only meaningful leading, but it is harmless
                // anywhere and the phone is the authority on the rest.
                if (c == '+' || c == '-' || c == '(' || c == ')' || c == ' ' || c == '.')
                    continue;
                err_out = "Phone number contains an unexpected character.";
                return false;
            }
            if (!digit) {
                err_out = "Phone number contains no digits.";
                return false;
            }
            return true;
        }

        const size_t at = address.find('@');
        if (at == std::string::npos || address.find('@', at + 1) != std::string::npos) {
            err_out = "Email address must contain exactly one '@'.";
            return false;
        }
        if (at == 0 || at + 1 >= address.size()) {
            err_out = "Email address is missing a local part or a domain.";
            return false;
        }
        if (address.find(' ') != std::string::npos) {
            err_out = "Email address contains a space.";
            return false;
        }
        return true;
    }

    bool recipient_from_thread_key(const std::string& thread_key, Recipient& out, std::string& err_out) {
        if (thread_key.rfind("tel:", 0) == 0) {
            out.kind = RecipientKind::Tel;
            out.address = thread_key.substr(4);
        } else if (thread_key.rfind("email:", 0) == 0) {
            out.kind = RecipientKind::Email;
            out.address = thread_key.substr(6);
        } else {
            err_out = "Unsupported conversation type; only one-to-one threads can be replied to.";
            return false;
        }
        return validate_recipient(out, err_out);
    }

    bool recipient_from_input(const std::string& text, Recipient& out, std::string& err_out) {
        out.address = trim(text);
        // phone number never contains '@', and an AppleID address always does
        out.kind = out.address.find('@') == std::string::npos ? RecipientKind::Tel : RecipientKind::Email;
        return validate_recipient(out, err_out);
    }

    std::string thread_key_for(const Recipient& recipient) {
        if (recipient.kind == RecipientKind::Email) {
            std::string email = normalize_email(recipient.address);
            return email.empty() ? std::string{} : "email:" + email;
        }
        std::string tel = normalize_phone(recipient.address);
        return tel.empty() ? std::string{} : "tel:" + tel;
    }

    std::string stuff_body(const std::string& body) {
        std::string out;
        for (const std::string& line : split_lines(body)) {
            if (line_needs_stuffing(line))
                out += ' ';
            out += line;
            out += "\r\n";
        }
        // split_lines yields a trailing empty element for a body ending in a
        // line break; drop the extra break it would introduce.
        if (!body.empty() && body.back() != '\n' && body.back() != '\r' && out.size() >= 2)
            out.erase(out.size() - 2);
        return out;
    }

    std::string build_bmessage(const std::vector<Recipient>& recipients, const std::string& body) {
        if (recipients.empty())
            return {};
        for (const auto& recipient : recipients) {
            std::string err;
            if (!validate_recipient(recipient, err))
                return {};
        }

        const std::string stuffed = stuff_body(body);
        std::string message_block = "BEGIN:MSG\r\n";
        message_block += stuffed;
        if (!stuffed.empty())
            message_block += "\r\n";
        message_block += "END:MSG\r\n";

        std::string out;
        out += "BEGIN:BMSG\r\n";
        out += "VERSION:1.0\r\n";
        out += "STATUS:UNREAD\r\n";
        // Always SMS_GSM, even for an Apple-ID recipient; iOS picks the transport,
        // and its own inbound Apple-ID messages arrive typed SMS_GSM too.
        out += "TYPE:SMS_GSM\r\n";
        out += "FOLDER:telecom/msg/outbox\r\n";

        // vCard 2.1 with all five N components present, which is what MAP
        // implementations emit and the one iOS parses correctly.
        out += "BEGIN:VCARD\r\n";
        out += "VERSION:2.1\r\n";
        out += "N:;;;;\r\n";
        // The originator address is left empty for the phone to fill in, but the property itself is present.
        out += "TEL:\r\n";
        out += "END:VCARD\r\n";

        // iOS discards the first two characters of an EMAIL property value, so a
        // bare address arrives two characters short and reaches nobody. Its own
        // vCards tag address values with a two-character scheme prefix
        // ("TEL:7:+15035551234"); emitting one gives the parser something to
        // consume other than the address. TEL values are not truncated and take
        // no tag.
        //
        // The tag was read off the phone's own vCards rather than any spec, so
        // TETHER_BMSG_ADDR_PREFIX overrides it if a future iOS expects another.
        const char* email_tag = env_or("TETHER_BMSG_ADDR_PREFIX", "7:");

        out += "BEGIN:BENV\r\n";
        for (const auto& recipient : recipients) {
            out += "BEGIN:VCARD\r\n";
            out += "VERSION:2.1\r\n";
            out += "N:;;;;\r\n";
            const bool is_email = recipient.kind == RecipientKind::Email;
            out += is_email ? "EMAIL:" : "TEL:";
            if (is_email)
                out += email_tag;
            out += recipient.address;
            out += "\r\n";
            out += "END:VCARD\r\n";
        }

        out += "BEGIN:BBODY\r\n";
        out += "CHARSET:UTF-8\r\n";
        // MAP 3.1.3: LENGTH is the byte count of the whole BEGIN:MSG..END:MSG
        // block, delimiters and their CRLFs included, not of the payload alone.
        out += "LENGTH:" + std::to_string(message_block.size()) + "\r\n";
        out += message_block;
        out += "END:BBODY\r\n";
        out += "END:BENV\r\n";
        out += "END:BMSG\r\n";
        return out;
    }

    int64_t parse_map_timestamp(const std::string& text) {
        // Basic ISO 8601: YYYYMMDDTHHMMSS, optionally followed by Z or an offset.
        if (text.size() < 15 || text[8] != 'T')
            return 0;

        std::tm tm{};
        int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
        if (std::sscanf(text.c_str(), "%4d%2d%2dT%2d%2d%2d", &year, &month, &day, &hour, &minute, &second) != 6)
            return 0;
        if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60)
            return 0;

        tm.tm_year = year - 1900;
        tm.tm_mon = month - 1;
        tm.tm_mday = day;
        tm.tm_hour = hour;
        tm.tm_min = minute;
        tm.tm_sec = second;
        tm.tm_isdst = -1;

        // A trailing Z means UTC; anything else is local time on the phone, which
        // is the same zone as the desktop in every case worth supporting.
        const bool utc = text.size() > 15 && (text[15] == 'Z' || text[15] == 'z');
        const std::time_t epoch = utc ? timegm(&tm) : std::mktime(&tm);
        return epoch == static_cast<std::time_t>(-1) ? 0 : static_cast<int64_t>(epoch);
    }

} // namespace tether::bluetooth
