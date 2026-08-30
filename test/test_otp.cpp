#include <gtest/gtest.h>
#include <tether/otp.hpp>

using namespace tether;

namespace {
    class OtpTest : public ::testing::Test {
      protected:
        void SetUp() override { otp_reset(); }
        void TearDown() override { otp_reset(); }

        OtpClock::time_point t0 = OtpClock::now();
    };
} // namespace

TEST_F(OtpTest, EmptyVaultServesNothing) {
    EXPECT_FALSE(otp_peek(t0));
    EXPECT_FALSE(otp_take_for_host("a.com", t0));
    EXPECT_FALSE(otp_is_claimed());
}

TEST_F(OtpTest, StoreAndPeek) {
    uint64_t id = otp_store("123456", "", t0);
    EXPECT_NE(id, 0u);

    Otp got = otp_peek(t0);
    EXPECT_TRUE(got);
    EXPECT_EQ(got.code, "123456");
    EXPECT_EQ(got.id, id);
}

TEST_F(OtpTest, IdsAreUniqueAndIncreasing) {
    uint64_t first = otp_store("111111", "", t0);
    uint64_t second = otp_store("222222", "", t0);
    EXPECT_GT(second, first);
}

TEST_F(OtpTest, StoringEmptyCodeClearsVault) {
    otp_store("123456", "", t0);
    EXPECT_EQ(otp_store("", "", t0), 0u);
    EXPECT_FALSE(otp_peek(t0));
}

TEST_F(OtpTest, ExpiresAfterTtl) {
    otp_store("123456", "", t0);
    EXPECT_TRUE(otp_peek(t0 + std::chrono::minutes(4)));
    EXPECT_FALSE(otp_peek(t0 + kOtpTtl));
    // Expiry is destructive: the slot is empty even back at t0.
    EXPECT_FALSE(otp_peek(t0));
}

// The reported bug: a code taken by one site must not be served to the next site
// the user visits, even while it is still within its TTL.
TEST_F(OtpTest, CodeIsClaimedByTheFirstHostThatTakesIt) {
    otp_store("123456", "", t0);

    Otp a = otp_take_for_host("a.com", t0);
    ASSERT_TRUE(a);
    EXPECT_EQ(a.code, "123456");
    EXPECT_TRUE(otp_is_claimed());

    EXPECT_FALSE(otp_take_for_host("b.com", t0)) << "b.com must not receive a.com's code";
}

TEST_F(OtpTest, ClaimingHostKeepsPollingSuccessfully) {
    otp_store("123456", "", t0);
    ASSERT_TRUE(otp_take_for_host("a.com", t0));
    // a.com's content script polls every 2s until it fills; it must keep getting the code.
    Otp again = otp_take_for_host("a.com", t0);
    EXPECT_TRUE(again);
    EXPECT_EQ(again.code, "123456");
}

TEST_F(OtpTest, NewCodeResetsTheClaim) {
    otp_store("111111", "", t0);
    ASSERT_TRUE(otp_take_for_host("a.com", t0));
    EXPECT_FALSE(otp_take_for_host("b.com", t0));

    // A fresh email arrives; b.com is now entitled to it.
    otp_store("222222", "", t0);
    EXPECT_FALSE(otp_is_claimed());
    Otp b = otp_take_for_host("b.com", t0);
    ASSERT_TRUE(b);
    EXPECT_EQ(b.code, "222222");
}

TEST_F(OtpTest, UnknownHostTakesButDoesNotClaim) {
    // Older extension builds don't send `url`. They may still fill, but must not
    // lock every other site out of the code.
    otp_store("123456", "", t0);
    EXPECT_TRUE(otp_take_for_host("", t0));
    EXPECT_FALSE(otp_is_claimed());
    EXPECT_TRUE(otp_take_for_host("a.com", t0));
}

TEST_F(OtpTest, UnknownHostDoesNotStealAClaimedCode) {
    otp_store("123456", "", t0);
    ASSERT_TRUE(otp_take_for_host("a.com", t0));
    EXPECT_FALSE(otp_take_for_host("", t0));
}

TEST_F(OtpTest, PinnedCodeServesOnlyMatchingDomain) {
    otp_store("123456", "amazon.com", t0);

    EXPECT_FALSE(otp_take_for_host("evil.com", t0));
    EXPECT_FALSE(otp_is_claimed()) << "a mismatched host must not claim the code";

    Otp got = otp_take_for_host("amazon.com", t0);
    ASSERT_TRUE(got);
    EXPECT_EQ(got.code, "123456");
}

TEST_F(OtpTest, PinnedCodeServesSubdomains) {
    otp_store("123456", "amazon.com", t0);
    EXPECT_TRUE(otp_take_for_host("www.amazon.com", t0));
}

TEST_F(OtpTest, PinnedCodeRejectsSuffixSpoofs) {
    otp_store("123456", "amazon.com", t0);
    EXPECT_FALSE(otp_take_for_host("amazon.com.evil.com", t0));
    EXPECT_FALSE(otp_take_for_host("evilamazon.com", t0));
    EXPECT_FALSE(otp_take_for_host("notamazon.com", t0));
}

TEST_F(OtpTest, PinnedCodeRejectsUnknownHost) {
    otp_store("123456", "amazon.com", t0);
    EXPECT_FALSE(otp_take_for_host("", t0));
}

TEST_F(OtpTest, UnpinnedCodeKeepsLegacyBehaviour) {
    otp_store("123456", "", t0);
    EXPECT_TRUE(otp_take_for_host("", t0));
    EXPECT_FALSE(otp_is_claimed());
    EXPECT_TRUE(otp_take_for_host("a.com", t0));
}

TEST_F(OtpTest, PinnedCodeClaimIsScopedToHostWithinDomain) {
    otp_store("111111", "amazon.com", t0);
    ASSERT_TRUE(otp_take_for_host("www.amazon.com", t0));
    // The claiming host keeps polling successfully.
    EXPECT_TRUE(otp_take_for_host("www.amazon.com", t0));
    // A different host — even within the same domain — does not steal the claim.
    EXPECT_FALSE(otp_take_for_host("amazon.com", t0));
    // A different domain still gets nothing.
    EXPECT_FALSE(otp_take_for_host("evil.com", t0));
}

TEST_F(OtpTest, SenderDomainIsNormalized) {
    otp_store("123456", "Amazon.COM.", t0);
    EXPECT_TRUE(otp_take_for_host("www.amazon.com", t0));
    EXPECT_FALSE(otp_take_for_host("amazon.com.evil.com", t0));
}

TEST_F(OtpTest, ConsumeClearsMatchingId) {
    uint64_t id = otp_store("123456", "", t0);
    otp_consume(id);
    EXPECT_FALSE(otp_peek(t0));
}

// A consume can arrive late (service worker wake-up, native host restart). It must
// not delete the code that arrived in the meantime for the page now waiting.
TEST_F(OtpTest, StaleConsumeDoesNotClearNewerCode) {
    uint64_t first = otp_store("111111", "", t0);
    uint64_t second = otp_store("222222", "", t0);

    otp_consume(first);

    Otp got = otp_peek(t0);
    ASSERT_TRUE(got);
    EXPECT_EQ(got.code, "222222");
    EXPECT_EQ(got.id, second);
}

TEST_F(OtpTest, ConsumeWithoutIdClearsUnconditionally) {
    // Backwards compatibility with extension builds that predate otp_id.
    otp_store("123456", "", t0);
    otp_consume(0);
    EXPECT_FALSE(otp_peek(t0));
}

TEST_F(OtpTest, ConsumeIsIdempotent) {
    uint64_t id = otp_store("123456", "", t0);
    otp_consume(id);
    otp_consume(id);
    EXPECT_FALSE(otp_peek(t0));
}

TEST_F(OtpTest, ConsumingClearsTheClaim) {
    otp_store("111111", "", t0);
    uint64_t id = otp_take_for_host("a.com", t0).id;
    otp_consume(id);
    EXPECT_FALSE(otp_is_claimed());
}

TEST(OtpExtractTest, TypicalSmsCodes) {
    EXPECT_EQ(otp_extract("Your verification code is 123456"), "123456");
    EXPECT_EQ(otp_extract("847291 is your Amazon OTP. Do not share it."), "847291");
    EXPECT_EQ(otp_extract("G-593012 is your Google verification code."), "593012");
    EXPECT_EQ(otp_extract("Your login code: 123 456"), "123456");
    EXPECT_EQ(otp_extract("Your code is 4821. Expires in 10 minutes."), "4821");
    EXPECT_EQ(otp_extract("Use passcode X7F2QK9 to sign in."), "X7F2QK9");
}

TEST(OtpExtractTest, IgnoresTextWithoutAnOtpCue) {
    EXPECT_EQ(otp_extract("Running late, be there at 1830"), "");
    EXPECT_EQ(otp_extract("Call me back on 5551234"), "");
    EXPECT_EQ(otp_extract(""), "");
}

TEST(OtpExtractTest, RejectsLookalikes) {
    EXPECT_EQ(otp_extract("Your order 8891234 shipped, tracking code below"), "");
    EXPECT_EQ(otp_extract("Security alert: charge of $4500 confirmed"), "");
    EXPECT_EQ(otp_extract("Your code is here"), "");
}

TEST(OtpExtractTest, PrefersTheNumberNearestTheCue) {
    EXPECT_EQ(otp_extract("Ref 987654 for the ticket. Your verification code is 112233."), "112233");
}
