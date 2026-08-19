#include <gtest/gtest.h>
#include <tether/bluetooth/bearer_supervisor.hpp>
#include <tether/bluetooth/profile_supervisor.hpp>

#include <string>
#include <vector>

using namespace tether::bluetooth;

namespace {

    class FakeBearer : public BearerOps {
    public:
        bool present = true;
        bool paired = true;
        bool classic = false;
        bool le_available = true;
        bool le = false;
        bool classic_succeeds = true;
        bool le_succeeds = true;

        int classic_attempts = 0;
        int le_attempts = 0;
        std::vector<std::string> preferred;

        bool device_present() const override { return present; }
        bool device_paired() const override { return paired; }
        bool classic_connected() const override { return classic; }
        bool le_bearer_available() const override { return le_available; }
        bool le_connected() const override { return le; }

        void set_preferred_bearer(const std::string& bearer) override { preferred.push_back(bearer); }

        std::string classic_error = "br/edr refused";
        std::string le_error = "le refused";

        ConnectResult connect_classic(std::string& err) override {
            ++classic_attempts;
            if (!classic_succeeds) {
                err = classic_error;
                return ConnectResult::Failed;
            }
            classic = true;
            return ConnectResult::Requested;
        }

        ConnectResult connect_le(std::string& err) override {
            ++le_attempts;
            if (!le_succeeds) {
                err = le_error;
                return ConnectResult::Failed;
            }
            le = true;
            return ConnectResult::Requested;
        }

        int le_disconnects = 0;
        bool disconnect_le_succeeds = true;

        ConnectResult disconnect_le(std::string& err) override {
            ++le_disconnects;
            if (!disconnect_le_succeeds) {
                err = "no LE bearer";
                return ConnectResult::Failed;
            }
            le = false;
            return ConnectResult::Requested;
        }
    };

    class FakeProfiles : public ProfileOps {
    public:
        std::string map_error;
        std::string pbap_error;
        int map_attempts = 0;
        int pbap_attempts = 0;
        std::vector<std::string> removed;
        int session_counter = 0;

        std::string create_session(const std::string& target, std::string& err) override {
            const bool is_map = target == "map";
            (is_map ? map_attempts : pbap_attempts)++;
            const std::string& failure = is_map ? map_error : pbap_error;
            if (!failure.empty()) {
                err = failure;
                return {};
            }
            return "/session" + std::to_string(++session_counter);
        }

        void remove_session(const std::string& path) override { removed.push_back(path); }
    };

} // namespace

// BR/EDR must come up first; LE must wait out the settle window. Connecting LE
// too early leaves the bond half-connected and flapping.
TEST(BearerSupervisor, ConnectsClassicThenLeAfterSettling) {
    FakeBearer ops;
    BearerSupervisor sup(ops, true);

    sup.tick(0);
    EXPECT_EQ(ops.classic_attempts, 1);
    EXPECT_EQ(ops.le_attempts, 0) << "LE must not be attempted before Classic is up";

    // Classic is up at t=1, but the settle window has not elapsed.
    sup.tick(1);
    EXPECT_EQ(ops.le_attempts, 0);
    sup.tick(1 + BEARER_SETTLE_SECONDS - 1);
    EXPECT_EQ(ops.le_attempts, 0) << "LE attempted before the settle window elapsed";

    sup.tick(1 + BEARER_SETTLE_SECONDS);
    EXPECT_EQ(ops.le_attempts, 1);
    EXPECT_FALSE(sup.status().le_connected) << "BlueZ accepting a connect is not a link";

    // Only an observed Connected is one.
    sup.tick(2 + BEARER_SETTLE_SECONDS);
    EXPECT_TRUE(sup.status().le_connected);
}

// PreferredBearer is an instruction for the next connection, not a durable
// setting. Leaving LE preferred while idle changes later reconnect behavior.
TEST(BearerSupervisor, NeverLeavesLePreferredWhileIdle) {
    FakeBearer ops;
    BearerSupervisor sup(ops, true);

    sup.tick(0);
    sup.tick(1);
    sup.tick(1 + BEARER_SETTLE_SECONDS);

    ASSERT_FALSE(ops.preferred.empty());
    EXPECT_EQ(ops.preferred.back(), "bredr");

    // Every "le" selection must be immediately followed by a return to "bredr".
    for (size_t i = 0; i < ops.preferred.size(); ++i) {
        if (ops.preferred[i] == "le") {
            ASSERT_LT(i + 1, ops.preferred.size()) << "left LE preferred at the end";
            EXPECT_EQ(ops.preferred[i + 1], "bredr");
        }
    }
}

TEST(BearerSupervisor, ClassicBackoffGrowsAndCaps) {
    FakeBearer ops;
    ops.classic_succeeds = false;
    BearerSupervisor sup(ops, true);

    sup.tick(0);
    EXPECT_EQ(ops.classic_attempts, 1);
    EXPECT_EQ(sup.status().classic_backoff, BEARER_BACKOFF_MIN_SECONDS);

    // Ticking inside the backoff window must not retry.
    sup.tick(BEARER_BACKOFF_MIN_SECONDS - 1);
    EXPECT_EQ(ops.classic_attempts, 1) << "retried before the backoff expired";

    sup.tick(BEARER_BACKOFF_MIN_SECONDS);
    EXPECT_EQ(ops.classic_attempts, 2);
    EXPECT_EQ(sup.status().classic_backoff, BEARER_BACKOFF_MIN_SECONDS * 2);

    // Drive many failures and confirm the cap holds.
    int64_t now = BEARER_BACKOFF_MIN_SECONDS;
    for (int i = 0; i < 20; ++i) {
        now += sup.status().classic_backoff;
        sup.tick(now);
    }
    EXPECT_EQ(sup.status().classic_backoff, BEARER_BACKOFF_MAX_SECONDS);
}

// InProgress means BlueZ is still finishing an operation of its own, which
// clears without help. Treating it like a refusal pushed the retry out to
// minutes and left LE — and so ANCS — down long after the cause was gone.
// InProgress means BlueZ is still working on the last connect, which it keeps
// doing long after a caller gives up. Retrying at the same cadence guarantees
// the next attempt collides with it too, so one stalled connect becomes a
// permanent storm. Standing further back is the only thing that clears it.
TEST(BearerSupervisor, InProgressWidensTheClassicWindow) {
    FakeBearer ops;
    ops.classic_succeeds = false;
    ops.classic_error = "GDBus.Error:org.bluez.Error.InProgress: In Progress";
    BearerSupervisor sup(ops, true);

    int64_t now = 0;
    int previous = 0;
    for (int i = 0; i < 5; ++i) {
        sup.tick(now);
        EXPECT_GT(sup.status().classic_backoff, previous) << "collided again at the same cadence";
        previous = sup.status().classic_backoff;
        now += previous;
    }
    EXPECT_EQ(ops.classic_attempts, 5);
}

TEST(BearerSupervisor, InProgressWidensTheLeWindow) {
    FakeBearer ops;
    ops.le_succeeds = false;
    ops.le_error = "GDBus.Error:org.bluez.Error.InProgress: In Progress";
    BearerSupervisor sup(ops, true);

    // Classic is observed connected on the tick after it is dialed, and the
    // settle window runs from there.
    sup.tick(0);
    sup.tick(1);
    int64_t now = 1 + BEARER_SETTLE_SECONDS;
    int previous = 0;
    for (int i = 0; i < LE_ATTEMPTS_BEFORE_ADVICE; ++i) {
        sup.tick(now);
        EXPECT_GT(sup.status().le_backoff, previous) << "collided again at the same cadence";
        previous = sup.status().le_backoff;
        now += previous;
    }
    EXPECT_EQ(ops.le_attempts, LE_ATTEMPTS_BEFORE_ADVICE);
}

// A refusal is different: the phone said no, and hammering it was observed to
// hold the bond in a half-connected flapping state.
TEST(BearerSupervisor, RefusalStillGrowsTheLeBackoff) {
    FakeBearer ops;
    ops.le_succeeds = false;
    BearerSupervisor sup(ops, true);

    sup.tick(0);
    sup.tick(1);
    sup.tick(1 + BEARER_SETTLE_SECONDS);
    EXPECT_EQ(sup.status().le_backoff, BEARER_BACKOFF_MIN_SECONDS);

    sup.tick(1 + BEARER_SETTLE_SECONDS + BEARER_BACKOFF_MIN_SECONDS);
    EXPECT_EQ(sup.status().le_backoff, BEARER_BACKOFF_MIN_SECONDS * 2);
}

TEST(BearerSupervisor, LeBackoffDoesNotBlockClassic) {
    FakeBearer ops;
    ops.le_succeeds = false;
    BearerSupervisor sup(ops, true);

    sup.tick(0);
    sup.tick(1);
    sup.tick(1 + BEARER_SETTLE_SECONDS);
    EXPECT_EQ(ops.le_attempts, 1);
    EXPECT_GT(sup.status().le_backoff, 0);
    EXPECT_TRUE(sup.status().classic_connected) << "an LE failure must not tear down Classic";
}

TEST(BearerSupervisor, RecoversAfterDisconnect) {
    FakeBearer ops;
    BearerSupervisor sup(ops, true);

    sup.tick(0);
    sup.tick(1);
    sup.tick(1 + BEARER_SETTLE_SECONDS);
    sup.tick(2 + BEARER_SETTLE_SECONDS);
    ASSERT_TRUE(sup.status().le_connected);

    // The phone walks out of range.
    ops.classic = false;
    ops.le = false;
    sup.tick(100);
    EXPECT_FALSE(sup.status().classic_connected);
    EXPECT_FALSE(sup.status().le_connected);
    EXPECT_EQ(ops.classic_attempts, 2);

    // ...and comes back.
    sup.tick(101);
    sup.tick(101 + BEARER_SETTLE_SECONDS);
    sup.tick(102 + BEARER_SETTLE_SECONDS);
    EXPECT_TRUE(sup.status().classic_connected);
    EXPECT_TRUE(sup.status().le_connected);
}

// reset() models a resume: recovery should start at once, not after waiting out
// a backoff accumulated before the machine slept.
TEST(BearerSupervisor, ResetClearsBackoffForImmediateRetry) {
    FakeBearer ops;
    ops.classic_succeeds = false;
    BearerSupervisor sup(ops, true);

    sup.tick(0);
    sup.tick(BEARER_BACKOFF_MIN_SECONDS);
    ASSERT_EQ(ops.classic_attempts, 2);

    sup.reset();
    ops.classic_succeeds = true;
    sup.tick(BEARER_BACKOFF_MIN_SECONDS + 1);
    EXPECT_EQ(ops.classic_attempts, 3) << "reset must allow an immediate retry";
}

TEST(BearerSupervisor, SkipsLeWhenAncsDisabled) {
    FakeBearer ops;
    BearerSupervisor sup(ops, false);

    sup.tick(0);
    sup.tick(1);
    sup.tick(1 + BEARER_SETTLE_SECONDS + 10);
    EXPECT_EQ(ops.le_attempts, 0);
    EXPECT_TRUE(sup.status().classic_connected);
}

TEST(BearerSupervisor, SkipsLeWhenBearerUnavailable) {
    FakeBearer ops;
    ops.le_available = false;
    BearerSupervisor sup(ops, true);

    sup.tick(0);
    sup.tick(1);
    sup.tick(1 + BEARER_SETTLE_SECONDS + 10);
    EXPECT_EQ(ops.le_attempts, 0);
    EXPECT_FALSE(sup.status().le_available);
}

TEST(BearerSupervisor, ReportsMissingDevice) {
    FakeBearer ops;
    ops.present = false;
    BearerSupervisor sup(ops, true);

    sup.tick(0);
    EXPECT_FALSE(sup.status().device_present);
    EXPECT_EQ(ops.classic_attempts, 0) << "must not dial a device BlueZ does not know";
}

// A known-but-unpaired device happens after an unpair or after the phone forgets
// this computer. Dialing it collides with the pairing transaction, and BlueZ
// answers both attempts with br-connection-busy.
TEST(BearerSupervisor, StandsDownWhenDeviceIsUnpaired) {
    FakeBearer ops;
    ops.paired = false;
    BearerSupervisor sup(ops, true);

    sup.tick(0);
    sup.tick(BEARER_POLL_SECONDS * 20);
    EXPECT_TRUE(sup.status().device_present);
    EXPECT_FALSE(sup.status().device_paired);
    EXPECT_EQ(ops.classic_attempts, 0) << "must not dial an unpaired device";
    EXPECT_EQ(ops.le_attempts, 0);
    EXPECT_TRUE(ops.preferred.empty()) << "must not touch PreferredBearer before a bond exists";
}

// Forbidden and Busy look similar but call for opposite advice. Reporting a
// permissions problem as a busy session sends the user off re-pairing for
// nothing, and vice versa.
TEST(ObexErrors, DistinguishesForbiddenFromBusy) {
    EXPECT_EQ(classify_obex_error("Forbidden"), ObexError::Forbidden);
    EXPECT_EQ(classify_obex_error("org.bluez.obex.Error.Forbidden: Forbidden"), ObexError::Forbidden);
    EXPECT_EQ(classify_obex_error("OBEX response 0x43"), ObexError::Forbidden);

    EXPECT_EQ(classify_obex_error("Connection refused (111)"), ObexError::Busy);
    EXPECT_EQ(classify_obex_error("connection refused"), ObexError::Busy);
}

// Observed on real hardware with the iPhone bonded, connected, and all three
// permission toggles verified on. Classifying it as Forbidden would send the
// user to settings that are already correct.
TEST(ObexErrors, ServiceRecordMissingIsNotAPermissionProblem) {
    const std::string real = "GDBus.Error:org.bluez.obex.Error.Failed: Unable to find service record";
    EXPECT_EQ(classify_obex_error(real), ObexError::NoRecord);
    EXPECT_NE(classify_obex_error(real), ObexError::Forbidden);

    // obexd reports a missing record whenever its SDP fetch is refused, so the
    // advice must name both plausible causes and must not send the user
    // re-pairing, which fixes neither.
    const std::string advice = obex_error_advice(ObexError::NoRecord, "map");
    EXPECT_NE(advice.find("another computer"), std::string::npos);
    EXPECT_NE(advice.find("Show Message Notifications"), std::string::npos);
    EXPECT_NE(advice.find("re-pairing is not the fix"), std::string::npos);
}

TEST(ObexErrors, ClassifiesUnavailableAndOther) {
    EXPECT_EQ(classify_obex_error("Host is down"), ObexError::Unavailable);
    EXPECT_EQ(classify_obex_error("Page Timeout"), ObexError::Unavailable);
    EXPECT_EQ(classify_obex_error(""), ObexError::None);
    EXPECT_EQ(classify_obex_error("something entirely new"), ObexError::Other);
}

TEST(ObexErrors, MissingObexdIsNotBlamedOnThePhone) {
    // Verbatim from a machine where bluez-obex had been uninstalled.
    const std::string real = "GDBus.Error:org.freedesktop.DBus.Error.ServiceUnknown: The name is not activatable";
    EXPECT_EQ(classify_obex_error(real), ObexError::NoDaemon);

    const std::string advice = obex_error_advice(ObexError::NoDaemon, "map");
    EXPECT_NE(advice.find("bluez-obex"), std::string::npos);
    // Nothing about this is fixable on the phone, so the advice must not say so.
    EXPECT_EQ(advice.find("iPhone's Settings"), std::string::npos);
    EXPECT_EQ(advice.find("re-pair"), std::string::npos);
}

TEST(ObexErrors, AdviceNamesThePermissionToggle) {
    const std::string map_advice = obex_error_advice(ObexError::Forbidden, "map");
    EXPECT_NE(map_advice.find("Show Message Notifications"), std::string::npos);
    // Must not send the user re-pairing over a permission.
    EXPECT_EQ(map_advice.find("re-pair"), std::string::npos);

    EXPECT_NE(obex_error_advice(ObexError::Forbidden, "pbap").find("Sync Contacts"), std::string::npos);
    EXPECT_NE(obex_error_advice(ObexError::Busy, "map").find("another computer"), std::string::npos);
}

TEST(ProfileSupervisor, OpensBothSessionsWhenLinkReady) {
    FakeProfiles ops;
    ProfileSupervisor sup(ops);

    sup.tick(0, true);
    EXPECT_TRUE(sup.status().map_open);
    EXPECT_TRUE(sup.status().pbap_open);
    EXPECT_FALSE(sup.map_session().empty());
    EXPECT_FALSE(sup.pbap_session().empty());
}

TEST(ProfileSupervisor, DoesNothingUntilLinkIsReady) {
    FakeProfiles ops;
    ProfileSupervisor sup(ops);

    sup.tick(0, false);
    EXPECT_EQ(ops.map_attempts, 0);
    EXPECT_EQ(ops.pbap_attempts, 0);
}

// A Forbidden may be a stale obexd session from a previous run, so exactly one
// retry is allowed. It must never escalate to restarting obexd.
TEST(ProfileSupervisor, RetriesOnceAfterForbidden) {
    FakeProfiles ops;
    ops.map_error = "Forbidden";
    ProfileSupervisor sup(ops);

    sup.tick(0, true);
    EXPECT_EQ(ops.map_attempts, 2) << "expected exactly one retry after Forbidden";
    EXPECT_FALSE(sup.status().map_open);
    EXPECT_EQ(sup.status().map_error, ObexError::Forbidden);

    // PBAP opened, so the supervisor is on the steady cadence even though MAP is
    // still failing. The extra Forbidden retry is spent, so this poll makes
    // exactly one attempt — a persistent permission problem must not turn into a
    // retry burst against obexd.
    sup.tick(PROFILE_STEADY_POLL_SECONDS, true);
    EXPECT_EQ(ops.map_attempts, 3);
}

// While nothing has opened yet, the fast cadence applies so a user flipping the
// permission toggles is picked up promptly.
TEST(ProfileSupervisor, UsesFastCadenceUntilSomethingOpens) {
    FakeProfiles ops;
    ops.map_error = "Forbidden";
    ops.pbap_error = "Forbidden";
    ProfileSupervisor sup(ops);

    sup.tick(0, true);
    const int attempts = ops.map_attempts;

    sup.tick(PROFILE_INITIAL_POLL_SECONDS - 1, true);
    EXPECT_EQ(ops.map_attempts, attempts);

    sup.tick(PROFILE_INITIAL_POLL_SECONDS, true);
    EXPECT_GT(ops.map_attempts, attempts);
}

TEST(ProfileSupervisor, PartialSuccessKeepsRetryingTheOtherProfile) {
    FakeProfiles ops;
    ops.pbap_error = "Forbidden";
    ProfileSupervisor sup(ops);

    sup.tick(0, true);
    EXPECT_TRUE(sup.status().map_open);
    EXPECT_FALSE(sup.status().pbap_open);

    const int map_attempts_after_open = ops.map_attempts;
    ops.pbap_error.clear();
    sup.tick(PROFILE_STEADY_POLL_SECONDS, true);
    EXPECT_TRUE(sup.status().pbap_open);
    EXPECT_EQ(ops.map_attempts, map_attempts_after_open) << "must not reopen an already-open session";
}

TEST(ProfileSupervisor, RespectsPollInterval) {
    FakeProfiles ops;
    ops.map_error = "Connection refused (111)";
    ops.pbap_error = "Connection refused (111)";
    ProfileSupervisor sup(ops);

    sup.tick(0, true);
    const int attempts = ops.map_attempts;
    sup.tick(1, true);
    EXPECT_EQ(ops.map_attempts, attempts) << "retried before the poll interval elapsed";

    sup.tick(PROFILE_INITIAL_POLL_SECONDS, true);
    EXPECT_GT(ops.map_attempts, attempts);
}

TEST(ProfileSupervisor, ResetRemovesSessions) {
    FakeProfiles ops;
    ProfileSupervisor sup(ops);

    sup.tick(0, true);
    ASSERT_TRUE(sup.status().map_open);

    sup.reset();
    EXPECT_EQ(ops.removed.size(), 2u);
    EXPECT_FALSE(sup.status().map_open);
    EXPECT_TRUE(sup.map_session().empty());

    // And reopens on the next tick.
    sup.tick(100, true);
    EXPECT_TRUE(sup.status().map_open);
}

TEST(ProfileSupervisor, BusySessionKeepsPollingWithoutRetryBurst) {
    FakeProfiles ops;
    ops.map_error = "Connection refused (111)";
    ProfileSupervisor sup(ops);

    sup.tick(0, true);
    // Busy is not Forbidden, so it gets no extra immediate retry.
    EXPECT_EQ(ops.map_attempts, 1);
    EXPECT_EQ(sup.status().map_error, ObexError::Busy);
}

// A phone that walked out of the room answers with a page timeout, not a
// refusal. Escalating those to the five-minute ceiling leaves the link down long
// after the phone is back on the desk.
TEST(BearerSupervisor, TransientFailuresDoNotEscalateTheBackoff) {
    for (const char* message : {"org.bluez.Error.NotReady: Resource Not Ready",
                                "org.bluez.Error.Failed: br-connection-page-timeout",
                                "org.bluez.Error.Failed: Software caused connection abort",
                                "org.bluez.Error.Failed: le-connection-abort-by-local",
                                "org.bluez.Error.Failed: br-connection-refused"}) {
        FakeBearer ops;
        ops.classic_succeeds = false;
        ops.classic_error = message;
        BearerSupervisor sup(ops, true);

        int64_t now = 0;
        for (int attempt = 0; attempt < 6; ++attempt) {
            sup.tick(now);
            EXPECT_EQ(sup.status().classic_backoff, BEARER_BACKOFF_MIN_SECONDS) << message;
            now += sup.status().classic_backoff;
        }
    }
}

// A refusal from the phone is a real answer, and repeating it every five seconds
// helps nobody.
TEST(BearerSupervisor, PersistentFailuresStillEscalate) {
    FakeBearer ops;
    ops.classic_succeeds = false;
    ops.classic_error = "org.bluez.Error.AuthenticationFailed";
    BearerSupervisor sup(ops, true);

    sup.tick(0);
    const int first = sup.status().classic_backoff;
    sup.tick(first);
    EXPECT_GT(sup.status().classic_backoff, first);
}

// reset() is what a caller reaches for when the phone reappears. The existing
// coverage stops at a minimum backoff; the case that actually bites is a backoff
// that grew to minutes while the phone was out of range.
TEST(BearerSupervisor, ResetClearsAnEscalatedBackoff) {
    FakeBearer ops;
    ops.classic_succeeds = false;
    ops.classic_error = "org.bluez.Error.AuthenticationFailed";
    BearerSupervisor sup(ops, true);

    int64_t now = 0;
    sup.tick(now);
    now += sup.status().classic_backoff;
    sup.tick(now);
    ASSERT_GT(sup.status().classic_backoff, BEARER_BACKOFF_MIN_SECONDS);
    const int attempts = ops.classic_attempts;

    sup.reset();
    EXPECT_EQ(sup.status().classic_backoff, 0);
    sup.tick(++now);
    EXPECT_EQ(ops.classic_attempts, attempts + 1) << "reset() left the grown backoff in place";
}

// The Forbidden retry exists to drop a stale session left by a previous run.
// Removing the empty string that the failed CreateSession just wrote drops
// nothing at all.
TEST(ProfileSupervisor, ForbiddenRetryRemovesThePreviousSession) {
    FakeProfiles ops;
    ProfileSupervisor sup(ops);

    sup.tick(0, true);
    ASSERT_TRUE(sup.status().map_open);
    const std::string stale = sup.map_session();
    ASSERT_FALSE(stale.empty());

    // The session goes sour; the next attempt is refused.
    sup.drop_map();
    ops.removed.clear();
    ops.map_error = "org.bluez.obex.Error.Forbidden";
    sup.tick(PROFILE_STEADY_POLL_SECONDS * 2, true);

    EXPECT_EQ(sup.status().map_error, ObexError::Forbidden);
    EXPECT_EQ(ops.map_attempts, 3) << "the one extra retry did not happen";
}

// A MAP session can die while the Classic link stays up. Nothing else notices,
// because tick() short-circuits once map_open is set.
TEST(ProfileSupervisor, DropMapReopensWithoutTouchingPbap) {
    FakeProfiles ops;
    ProfileSupervisor sup(ops);

    sup.tick(0, true);
    ASSERT_TRUE(sup.status().map_open);
    ASSERT_TRUE(sup.status().pbap_open);
    const std::string first_map = sup.map_session();
    const std::string pbap = sup.pbap_session();

    sup.drop_map();
    EXPECT_FALSE(sup.status().map_open);
    EXPECT_TRUE(sup.status().pbap_open) << "contacts were dropped along with messages";
    ASSERT_EQ(ops.removed.size(), 1u);
    EXPECT_EQ(ops.removed.front(), first_map);

    // And reopens on the very next tick rather than waiting out the steady poll.
    sup.tick(1, true);
    EXPECT_TRUE(sup.status().map_open);
    EXPECT_NE(sup.map_session(), first_map);
    EXPECT_EQ(sup.pbap_session(), pbap) << "the surviving PBAP session was reopened needlessly";
}

// iOS routinely keeps an LE link while letting BR/EDR lapse. ANCS rides LE
// alone, so notification mirroring keeps working there — reporting LE as down
// because BR/EDR is would tear down a GATT session that is still delivering.
TEST(BearerSupervisor, LeStaysReportedWhenOnlyClassicIsDown) {
    FakeBearer ops;
    BearerSupervisor sup(ops, true);

    sup.tick(0);
    sup.tick(1);
    sup.tick(1 + BEARER_SETTLE_SECONDS);
    sup.tick(2 + BEARER_SETTLE_SECONDS);
    ASSERT_TRUE(sup.status().classic_connected);
    ASSERT_TRUE(sup.status().le_connected);

    // BR/EDR lapses; the LE link is untouched.
    ops.classic = false;
    ops.classic_succeeds = false;
    sup.tick(100);

    EXPECT_FALSE(sup.status().classic_connected);
    EXPECT_TRUE(sup.status().le_connected) << "an LE link that is still up must not be reported as down";
    EXPECT_GT(ops.classic_attempts, 1) << "BR/EDR must still be retried";
}

// An iPhone that refuses BR/EDR while unlocked with its permissions on is wedged
// on its own side. Only cycling Bluetooth on the phone clears it, so the status
// has to say that rather than claiming to still be connecting.
TEST(BearerSupervisor, RepeatedRefusalNamesTheRemedy) {
    FakeBearer ops;
    ops.classic_succeeds = false;
    ops.classic_error = "org.bluez.Error.Failed: br-connection-refused";
    BearerSupervisor sup(ops, true);

    int64_t now = 0;
    sup.tick(now);
    EXPECT_NE(sup.status().reason.find("Connecting"), std::string::npos)
        << "one refusal is not yet evidence of anything";

    while (ops.classic_attempts < CLASSIC_FAILURES_BEFORE_ADVICE) {
        now += sup.status().classic_backoff;
        sup.tick(now);
    }

    EXPECT_NE(sup.status().reason.find("Turn Bluetooth off and back on"), std::string::npos)
        << "the status never names the one thing that clears this";
    EXPECT_NE(sup.status().reason.find("Re-pairing is not the fix"), std::string::npos);

    // And it goes away the moment the phone accepts. The backoff has to elapse
    // first, or no attempt is made and the advice rightly still stands.
    ops.classic_succeeds = true;
    now += sup.status().classic_backoff;
    sup.tick(now);
    EXPECT_EQ(sup.status().reason.find("Turn Bluetooth off"), std::string::npos);
    sup.tick(++now);
    EXPECT_TRUE(sup.status().classic_connected);
}

// BR/EDR can stay up for days, so the LE half is the only one that ever needs
// re-dialling, and nothing else in the daemon dials it. Standing down for good
// after a run of failures left notification mirroring dead until a restart even
// though the phone had long since become willing. The backoff ceiling, not a
// attempt cap, is what keeps this from being a storm.
TEST(BearerSupervisor, KeepsDiallingLeAtTheCappedCadence) {
    // A phone that will not accept an inbound LE connect: BlueZ keeps working
    // after our call gives up, so the next attempts collide with it.
    class RefusingBearer : public FakeBearer {
    public:
        ConnectResult connect_le(std::string& err) override {
            ++le_attempts;
            err = (le_attempts % 2) ? "Timeout was reached" : "GDBus.Error:org.bluez.Error.InProgress: In Progress";
            return ConnectResult::Failed;
        }
    } refusing;

    BearerSupervisor sup(refusing, true);
    int64_t now = 0;
    sup.tick(now);
    now += BEARER_SETTLE_SECONDS + 1;

    for (int i = 0; i < 200; ++i) {
        now += BEARER_BACKOFF_MAX_SECONDS;
        sup.tick(now);
    }

    EXPECT_GT(refusing.le_attempts, LE_ATTEMPTS_BEFORE_ADVICE) << "LE was abandoned for the daemon's whole lifetime";
    EXPECT_NE(sup.status().reason.find("LE link that carries notifications"), std::string::npos);

    // ...but no faster than the ceiling. Every tick above is a full backoff
    // window apart, so one attempt per window is the most that may happen.
    EXPECT_LE(refusing.le_attempts, 202) << "the backoff ceiling is not pacing the retries";

    // And it recovers the moment the phone becomes willing, with no reset() and
    // no restart.
    refusing.le = true;
    now += BEARER_BACKOFF_MAX_SECONDS;
    sup.tick(now);
    EXPECT_TRUE(sup.status().le_connected);
}

// An LE link that needed a few tries, then held, must not resume at the backoff
// those tries grew to. Inheriting it means the first drop after a healthy day
// costs five minutes of dead notification mirroring.
TEST(BearerSupervisor, ASuccessfulLeLinkClearsTheBackoffItGrew) {
    FakeBearer ops;
    ops.le_succeeds = false;
    BearerSupervisor sup(ops, true);

    // Classic is observed connected on the tick after it is dialed, and the
    // settle window runs from there.
    sup.tick(0);
    sup.tick(1);
    int64_t now = 1 + BEARER_SETTLE_SECONDS;
    for (int i = 0; i < 4; ++i) {
        sup.tick(now);
        now += sup.status().le_backoff;
    }
    ASSERT_GT(sup.status().le_backoff, BEARER_BACKOFF_MIN_SECONDS);

    // The phone accepts, and the link holds.
    ops.le_succeeds = true;
    ops.le = true;
    sup.tick(now);
    ASSERT_TRUE(sup.status().le_connected);
    EXPECT_EQ(sup.status().le_backoff, 0) << "carried the old backoff into a healthy link";

    // It drops much later. The retry is prompt, not held over from before.
    ops.le = false;
    ops.le_succeeds = false;
    now += 100000;
    const int attempts = ops.le_attempts;
    sup.tick(now);
    EXPECT_EQ(ops.le_attempts, attempts + 1) << "sat out a backoff inherited from a previous outage";
    EXPECT_EQ(sup.status().le_backoff, BEARER_BACKOFF_MIN_SECONDS);
}

// A connect that keeps colliding is BlueZ holding one that never finished.
// Waiting does not clear it; dropping the bearer does. Without this the only
// documented remedy was restarting bluetoothd.
TEST(BearerSupervisor, DropsAStuckLeBearerBeforeRedialling) {
    class StuckBearer : public FakeBearer {
    public:
        ConnectResult connect_le(std::string& err) override {
            ++le_attempts;
            err = "GDBus.Error:org.bluez.Error.InProgress: In Progress";
            return ConnectResult::Busy;
        }
    } stuck;

    BearerSupervisor sup(stuck, true);
    int64_t now = 0;
    sup.tick(now);
    now += BEARER_SETTLE_SECONDS + 1;

    // Nothing is dropped while the run is still short enough to be ordinary.
    while (stuck.le_attempts < LE_ATTEMPTS_BEFORE_ADVICE) {
        sup.tick(now);
        now += BEARER_BACKOFF_MAX_SECONDS;
    }
    EXPECT_EQ(stuck.le_disconnects, 0) << "dropped the bearer before the collisions meant anything";

    sup.tick(now);
    EXPECT_EQ(stuck.le_disconnects, 1) << "a wedged bearer was never dropped";

    // One per backoff window at most, not one per tick.
    sup.tick(now + 1);
    EXPECT_EQ(stuck.le_disconnects, 1) << "dropped the bearer again inside its own backoff";
}

// A refusal from the phone is not a collision, and tearing the transport down
// over one would interrupt a link that is merely unwelcome, not stuck.
TEST(BearerSupervisor, DoesNotDropTheBearerOverAPlainRefusal) {
    FakeBearer ops;
    ops.le_succeeds = false;
    ops.le_error = "org.bluez.Error.Failed: le-connection-abort-by-local";
    BearerSupervisor sup(ops, true);

    int64_t now = 0;
    sup.tick(now);
    now += BEARER_SETTLE_SECONDS + 1;
    for (int i = 0; i < LE_ATTEMPTS_BEFORE_ADVICE * 3; ++i) {
        sup.tick(now);
        now += BEARER_BACKOFF_MAX_SECONDS;
    }

    EXPECT_GT(ops.le_attempts, LE_ATTEMPTS_BEFORE_ADVICE);
    EXPECT_EQ(ops.le_disconnects, 0) << "a refusal is the phone's answer, not a wedged bearer";
}

// A link that is up must never be torn down by the recovery path.
TEST(BearerSupervisor, NeverDropsAConnectedLeBearer) {
    FakeBearer ops;
    BearerSupervisor sup(ops, true);

    sup.tick(0);
    sup.tick(1);
    sup.tick(1 + BEARER_SETTLE_SECONDS);
    sup.tick(2 + BEARER_SETTLE_SECONDS);
    ASSERT_TRUE(sup.status().le_connected);

    for (int i = 0; i < 50; ++i)
        sup.tick(10 + BEARER_BACKOFF_MAX_SECONDS * i);
    EXPECT_EQ(ops.le_disconnects, 0);
}

// A wedged bluetoothd and a phone that declines both end in a dead LE link, but
// only one of them is fixed on the phone. Sending the user to iOS settings for a
// stuck local connect is the wrong errand.
TEST(BearerSupervisor, StuckBluezAdviceNamesTheDaemonRestart) {
    class StuckBearer : public FakeBearer {
    public:
        ConnectResult connect_le(std::string& err) override {
            ++le_attempts;
            err = "GDBus.Error:org.bluez.Error.InProgress: In Progress";
            return ConnectResult::Failed;
        }
    } stuck;

    BearerSupervisor sup(stuck, true);
    int64_t now = 0;
    sup.tick(now);
    now += BEARER_SETTLE_SECONDS + 1;
    while (stuck.le_attempts < LE_ATTEMPTS_BEFORE_ADVICE) {
        now += BEARER_BACKOFF_MAX_SECONDS;
        sup.tick(now);
    }

    EXPECT_NE(sup.status().reason.find("systemctl restart bluetooth"), std::string::npos)
        << "the one thing that clears a stuck connect is never named";
    EXPECT_NE(sup.status().reason.find("Messages and contacts are unaffected"), std::string::npos);

    // A refusal that never collides is the phone's to fix, and must not send the
    // user restarting daemons.
    FakeBearer refusing;
    refusing.le_succeeds = false;
    refusing.le_error = "org.bluez.Error.Failed: le-connection-abort-by-local";
    BearerSupervisor other(refusing, true);
    now = 0;
    other.tick(now);
    now += BEARER_SETTLE_SECONDS + 1;
    while (refusing.le_attempts < LE_ATTEMPTS_BEFORE_ADVICE) {
        now += BEARER_BACKOFF_MAX_SECONDS;
        other.tick(now);
    }

    EXPECT_NE(other.status().reason.find("not opening the LE link"), std::string::npos);
    EXPECT_EQ(other.status().reason.find("systemctl"), std::string::npos) << "nothing here says BlueZ is wedged";
}

TEST(BearerSupervisor, UndiallableLeStopsClaimingItIsComingUp) {
    // The connect reports success but the link never appears, which is exactly
    // what a Device1.Connect fallback does on an already-connected device.
    class OptimisticBearer : public FakeBearer {
    public:
        ConnectResult connect_le(std::string&) override {
            ++le_attempts;
            return ConnectResult::Requested;
        }
    } optimistic;

    BearerSupervisor sup(optimistic, true);
    int64_t now = 0;
    sup.tick(now);
    sup.tick(++now);
    now += BEARER_SETTLE_SECONDS;
    sup.tick(now);
    // The attempt reports success, so this tick believes it. The next one reads
    // the real state back and finds nothing there.
    sup.tick(++now);
    ASSERT_FALSE(sup.status().le_connected) << "the fake never actually brings LE up";
    EXPECT_NE(sup.status().reason.find("Bringing up the LE link"), std::string::npos);

    while (optimistic.le_attempts < LE_ATTEMPTS_BEFORE_ADVICE) {
        now += BEARER_BACKOFF_MIN_SECONDS;
        sup.tick(now);
    }
    // The tick that made the last attempt believed it worked; the one after reads
    // the truth back, and that is where the status has to change.
    sup.tick(++now);

    EXPECT_NE(sup.status().reason.find("not opening the LE link"), std::string::npos)
        << "the status never admits the link is not coming from this end";
    EXPECT_NE(sup.status().reason.find("Messages and contacts are unaffected"), std::string::npos)
        << "a user reading this needs to know messages still work";

    // An optimistic success must not turn into a connect attempt every tick.
    const int attempts = optimistic.le_attempts;
    sup.tick(++now);
    EXPECT_EQ(optimistic.le_attempts, attempts) << "LE was re-dialled inside its own backoff";
}
