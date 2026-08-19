#include <gtest/gtest.h>
#include <tether/bluetooth/bmessage.hpp>

using namespace tether::bluetooth;

namespace {

    Recipient tel(const std::string& address) { return {RecipientKind::Tel, address}; }
    Recipient email(const std::string& address) { return {RecipientKind::Email, address}; }

    size_t count_occurrences(const std::string& haystack, const std::string& needle) {
        size_t count = 0;
        for (size_t at = haystack.find(needle); at != std::string::npos; at = haystack.find(needle, at + 1))
            ++count;
        return count;
    }

} // namespace

TEST(BMessageBuild, ProducesTheShapeThePhoneAccepts) {
    const std::string out = build_bmessage({tel("+15551234567")}, "on my way");

    EXPECT_NE(out.find("BEGIN:BMSG"), std::string::npos);
    EXPECT_NE(out.find("TYPE:SMS_GSM"), std::string::npos);
    EXPECT_NE(out.find("FOLDER:telecom/msg/outbox"), std::string::npos);
    EXPECT_NE(out.find("CHARSET:UTF-8"), std::string::npos);
    EXPECT_NE(out.find("TEL:+15551234567"), std::string::npos);
    EXPECT_NE(out.find("on my way"), std::string::npos);

    // The originator vCard is empty and sits before the envelope; the recipient
    // vCard sits inside it.
    const size_t originator = out.find("BEGIN:VCARD");
    const size_t envelope = out.find("BEGIN:BENV");
    const size_t recipient = out.find("TEL:+15551234567");
    ASSERT_NE(envelope, std::string::npos);
    EXPECT_LT(originator, envelope) << "originator must be outside the envelope";
    EXPECT_LT(envelope, recipient) << "recipient must be inside the envelope";
}

// iOS chooses iMessage on its own; the type never changes, and nothing here can
// force or observe that choice.
TEST(BMessageBuild, AlwaysUsesSmsGsmEvenForAppleIdRecipients) {
    const std::string out = build_bmessage({email("someone@example.com")}, "hi");

    EXPECT_NE(out.find("TYPE:SMS_GSM"), std::string::npos);
    EXPECT_NE(out.find("EMAIL:someone@example.com"), std::string::npos);
    // The originator carries an empty TEL; no TEL may carry an address.
    EXPECT_EQ(out.find("TEL:+"), std::string::npos);
    EXPECT_EQ(out.find("TEL:someone"), std::string::npos);
}

// iOS was observed recording a recipient with characters missing from the front
// of the address, against an envelope that differed from a known-good sender's
// only in these details. Pinned so the shape cannot drift back.
TEST(BMessageBuild, UsesTheVcardShapeIosParsesCorrectly) {
    const std::string out = build_bmessage({email("cece.blair@icloud.com")}, "hi");

    EXPECT_EQ(out.find("VERSION:3.0"), std::string::npos) << "vCard 3.0 is not the shape that parsed correctly";
    EXPECT_NE(out.find("VERSION:2.1"), std::string::npos);
    // All five N components present; a bare "N:" preceded the mangled address.
    EXPECT_NE(out.find("N:;;;;"), std::string::npos);
    EXPECT_EQ(out.find("N:\r\n"), std::string::npos);
}

// The address the phone acts on has to survive a round trip through the same
// parser that reads incoming messages, parameters and all.
TEST(BMessageBuild, EmailRecipientSurvivesAParseOfWhatWasBuilt) {
    const std::string address = "cece.blair@icloud.com";
    const BMessage parsed = parse_bmessage(build_bmessage({email(address)}, "hi"));

    ASSERT_TRUE(parsed.valid);
    ASSERT_EQ(parsed.recipients.size(), 1u);
    EXPECT_EQ(parsed.recipients.front().email, address) << "the recipient lost characters between build and parse";
}

// An iPhone truncates the EMAIL address in a pushed bMessage and delivers to the
// result, so the message reaches nobody -- and reports the transfer complete
// either way. Verified against two independent senders emitting byte-identical
// bMessages. The send is allowed, so the warning is the only thing standing
// between the user and a message they believe arrived.
TEST(BMessageBuild, AppleIdRecipientsCarryADeliveryWarning) {
    const std::string why = delivery_warning(email("cece.blair@icloud.com"));
    ASSERT_FALSE(why.empty()) << "an Apple ID recipient was reported as reliable";
    EXPECT_NE(why.find("not arrive"), std::string::npos) << "the warning does not say what goes wrong";

    EXPECT_TRUE(delivery_warning(tel("+15551234567")).empty()) << "phone recipients are unaffected";
}

TEST(BMessageBuild, LengthCountsTheBodyWithoutItsDelimiters) {
    const std::string out = build_bmessage({tel("+15551234567")}, "hello");

    const size_t length_at = out.find("LENGTH:");
    ASSERT_NE(length_at, std::string::npos);
    const size_t declared = std::stoul(out.substr(length_at + 7));

    const std::string open = "BEGIN:MSG\r\n";
    const size_t begin = out.find(open);
    const size_t end = out.find("\r\nEND:MSG");
    ASSERT_NE(begin, std::string::npos);
    ASSERT_NE(end, std::string::npos);
    EXPECT_EQ(declared, end - (begin + open.size()));
}

// --- The trust boundary --------------------------------------------------
//
// Every case below is an address that, interpolated verbatim, would add a
// recipient the user never chose.

TEST(BMessageBuild, RejectsNewlineInjectedRecipient) {
    std::string err;
    EXPECT_FALSE(validate_recipient(tel("+15551234567\r\nTEL:+15559999999"), err));
    EXPECT_FALSE(err.empty());

    EXPECT_FALSE(validate_recipient(tel("+15551234567\nTEL:+15559999999"), err));
    EXPECT_FALSE(validate_recipient(email("a@b.com\r\nEMAIL:evil@example.com"), err));
}

TEST(BMessageBuild, RejectsVCardDelimiters) {
    std::string err;
    EXPECT_FALSE(validate_recipient(email("a@b.com;TYPE=WORK"), err));
    EXPECT_FALSE(validate_recipient(email("a@b.com:x"), err));
    EXPECT_FALSE(validate_recipient(email("a@b.com,c@d.com"), err));
    EXPECT_FALSE(validate_recipient(email("a\\@b.com"), err));
}

TEST(BMessageBuild, RejectsControlCharacters) {
    std::string err;
    EXPECT_FALSE(validate_recipient(tel(std::string("+1555\x01"
                                                    "234")),
                                    err));
    EXPECT_FALSE(validate_recipient(email(std::string("a@b\x7f.com")), err));
    EXPECT_FALSE(validate_recipient(tel("+1555\t234"), err));
}

// The builder refuses outright rather than escaping: a message that reaches
// the wrong person cannot be taken back, so there is no partial success here.
TEST(BMessageBuild, BuildRefusesAnInvalidRecipientEntirely) {
    EXPECT_EQ(build_bmessage({tel("+15551234567\r\nTEL:+15559999999")}, "hi"), "");
    EXPECT_EQ(build_bmessage({}, "hi"), "");

    // One bad recipient poisons the whole message, including the valid one.
    EXPECT_EQ(build_bmessage({tel("+15551234567"), email("a@b.com\r\nEMAIL:evil@example.com")}, "hi"), "");
}

// The end-to-end proof: whatever an attacker puts in the address, the built
// message must never contain more recipients than were asked for.
TEST(BMessageBuild, InjectedRecipientNeverSurvivesIntoAParsedMessage) {
    const std::string out = build_bmessage({tel("+15551234567")}, "hi");
    ASSERT_FALSE(out.empty());

    BMessage parsed = parse_bmessage(out);
    ASSERT_TRUE(parsed.valid);
    ASSERT_EQ(parsed.recipients.size(), 1u);
    EXPECT_EQ(parsed.recipients[0].tel, "+15551234567");
}

TEST(BMessageBuild, AcceptsAddressesPeopleActuallyWrite) {
    std::string err;
    EXPECT_TRUE(validate_recipient(tel("+1 (555) 123-4567"), err)) << err;
    EXPECT_TRUE(validate_recipient(tel("5551234567"), err)) << err;
    EXPECT_TRUE(validate_recipient(email("first.last+tag@example.co.uk"), err)) << err;
}

TEST(BMessageBuild, RejectsAddressesThatAreNotAddresses) {
    std::string err;
    EXPECT_FALSE(validate_recipient(tel(""), err));
    EXPECT_FALSE(validate_recipient(tel("no-digits-here"), err));
    EXPECT_FALSE(validate_recipient(email("missing-at-sign"), err));
    EXPECT_FALSE(validate_recipient(email("two@at@signs.com"), err));
    EXPECT_FALSE(validate_recipient(email("@nolocal.com"), err));
    EXPECT_FALSE(validate_recipient(email("nodomain@"), err));
    EXPECT_FALSE(validate_recipient(tel(std::string(300, '5')), err)) << "absurd length must not reach the phone";
}

// --- Byte stuffing -------------------------------------------------------

TEST(BMessageBuild, StuffsBodyLinesThatLookStructural) {
    const std::string out = build_bmessage({tel("+15551234567")}, "END:MSG");

    // Exactly one real terminator: the body's copy is stuffed and does not end
    // the message early.
    EXPECT_EQ(count_occurrences(out, "\r\nEND:MSG\r\n"), 1u);
    EXPECT_NE(out.find(" END:MSG"), std::string::npos);
}

TEST(BMessageBuild, StuffedBodySurvivesARoundTrip) {
    for (const std::string& body : {std::string("END:MSG"),
                                    std::string("BEGIN:VCARD"),
                                    std::string("line one\nEND:BMSG\nline three"),
                                    std::string(" BEGIN:MSG"),
                                    std::string("begin:msg")}) {
        const std::string out = build_bmessage({tel("+15551234567")}, body);
        ASSERT_FALSE(out.empty()) << body;

        BMessage parsed = parse_bmessage(out);
        EXPECT_TRUE(parsed.valid) << body;
        EXPECT_EQ(parsed.body, body) << "body must survive stuffing and unstuffing unchanged";
        EXPECT_EQ(parsed.recipients.size(), 1u) << "a body that looks like a vCard must not become a recipient";
    }
}

TEST(BMessageBuild, PreservesUtf8Bodies) {
    const std::string body = "emoji \xF0\x9F\x92\xA9 and accents \xC3\xA9\xC3\xA8";
    BMessage parsed = parse_bmessage(build_bmessage({tel("+15551234567")}, body));

    EXPECT_TRUE(parsed.valid);
    EXPECT_EQ(parsed.body, body);
}

TEST(BMessageBuild, HandlesMultiLineAndEmptyBodies) {
    BMessage multi = parse_bmessage(build_bmessage({tel("+15551234567")}, "one\ntwo\nthree"));
    EXPECT_EQ(multi.body, "one\ntwo\nthree");

    BMessage empty = parse_bmessage(build_bmessage({tel("+15551234567")}, ""));
    EXPECT_TRUE(empty.valid);
    EXPECT_EQ(empty.body, "");
}

// --- Thread keys ---------------------------------------------------------

TEST(BMessageBuild, ParsesRecipientsFromThreadKeys) {
    Recipient out;
    std::string err;

    ASSERT_TRUE(recipient_from_thread_key("tel:+15551234567", out, err)) << err;
    EXPECT_EQ(out.kind, RecipientKind::Tel);
    EXPECT_EQ(out.address, "+15551234567");

    ASSERT_TRUE(recipient_from_thread_key("email:a@b.com", out, err)) << err;
    EXPECT_EQ(out.kind, RecipientKind::Email);
    EXPECT_EQ(out.address, "a@b.com");
}

// Group threads need their own routing rules, so replying to one must fail
// closed rather than guessing a recipient set.
TEST(BMessageBuild, RefusesThreadKeysItDoesNotUnderstand) {
    Recipient out;
    std::string err;

    EXPECT_FALSE(recipient_from_thread_key("group:weekend-trip", out, err));
    EXPECT_FALSE(err.empty());
    EXPECT_FALSE(recipient_from_thread_key("+15551234567", out, err));
    EXPECT_FALSE(recipient_from_thread_key("", out, err));
    EXPECT_FALSE(recipient_from_thread_key("tel:+1555\r\nTEL:+1999", out, err))
        << "an injected key must not pass just because its prefix is known";
}

// What the compose field hands over: whatever the user typed, in whatever shape
// they habitually write it.
TEST(BMessageBuild, ParsesRecipientsFromTypedInput) {
    Recipient out;
    std::string err;

    ASSERT_TRUE(recipient_from_input("+1 (555) 123-4567", out, err)) << err;
    EXPECT_EQ(out.kind, RecipientKind::Tel);
    EXPECT_EQ(thread_key_for(out), "tel:+15551234567");

    ASSERT_TRUE(recipient_from_input("  Alice@iCloud.com ", out, err)) << err;
    EXPECT_EQ(out.kind, RecipientKind::Email);
    EXPECT_EQ(thread_key_for(out), "email:alice@icloud.com");

    // The key a composed message gets has to be the one the thread it belongs to
    // already uses, or the reply lands in a second conversation with the same
    // person.
    ASSERT_TRUE(recipient_from_input("555-1234", out, err)) << err;
    Recipient round_trip;
    ASSERT_TRUE(recipient_from_thread_key(thread_key_for(out), round_trip, err)) << err;
    EXPECT_EQ(round_trip.kind, out.kind);
    EXPECT_EQ(thread_key_for(round_trip), thread_key_for(out));
}

TEST(BMessageBuild, RefusesTypedInputThatIsNotAnAddress) {
    Recipient out;
    std::string err;

    // A contact name typed without picking a completion. Rejected on the text as
    // written, so the reason names the letters rather than reporting an empty
    // number normalization already discarded.
    EXPECT_FALSE(recipient_from_input("Alice", out, err));
    EXPECT_FALSE(err.empty());

    EXPECT_FALSE(recipient_from_input("", out, err));
    EXPECT_FALSE(recipient_from_input("   \t ", out, err));
    EXPECT_FALSE(recipient_from_input("a@b;c@d", out, err)) << "a vCard delimiter must not reach the phone";
    EXPECT_FALSE(recipient_from_input("a@b\r\nTEL:+1999", out, err));
    EXPECT_FALSE(recipient_from_input("a@b@c.com", out, err));
    EXPECT_FALSE(recipient_from_input("@nobody.com", out, err));
}
