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

        bool device_present() const override { return present; }
        bool device_paired() const override { return paired; }
        bool classic_connected() const override { return classic; }
        bool le_bearer_available() const override { return le_available; }
        bool le_connected() const override { return le; }

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

        // Models BlueZ holding the accept-list registration open. Tests that
        // care set this; the default of "nothing outstanding" keeps the older
        // cases reading as they did.
        bool outstanding = false;
        bool le_connect_outstanding() const override { return outstanding; }
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

// BlueZ implements an LE connect as an accept-list registration plus a
// background scan -- it waits for the phone to advertise rather than dialling
// it. Captured 2026-08-19: six Connect calls produced six "Add Device, action
// auto-connect" and not one LE Create Connection. So exactly one registration
// should be held open while LE is down, and repeating it is not progress.
TEST(BearerSupervisor, HoldsOneLeRegistrationRatherThanRepeatingIt) {
    class ListeningBearer : public FakeBearer {
    public:
        ConnectResult connect_le(std::string&) override {
            ++le_attempts;
            outstanding = true; // BlueZ now holds it until the request completes
            return ConnectResult::Requested;
        }
    } listening;

    BearerSupervisor sup(listening, true);
    int64_t now = 0;
    sup.tick(now);
    sup.tick(++now);
    now += BEARER_SETTLE_SECONDS;
    sup.tick(now);
    EXPECT_EQ(listening.le_attempts, 1);

    // Ticks throughout the dial window must not add a second one.
    for (int i = 0; i < LE_DIAL_WINDOW_SECONDS - BEARER_SETTLE_SECONDS - 3; ++i)
        sup.tick(++now);
    EXPECT_EQ(listening.le_attempts, 1) << "re-registered on top of a live registration";
    EXPECT_NE(sup.status().reason.find("Waiting for the iPhone to open the LE link"), std::string::npos);

    // When the request finally completes without a link, one fresh registration
    // replaces it -- still only ever one at a time.
    listening.outstanding = false;
    sup.tick(++now);
    EXPECT_EQ(listening.le_attempts, 2);
}

// InProgress means BlueZ already holds a registration. That is the state worth
// being in, so it must not be counted as a failure or earn a backoff -- treating
// it as one is what turned a healthy wait into an escalating retry.
TEST(BearerSupervisor, InProgressIsListeningNotFailing) {
    class InProgressBearer : public FakeBearer {
    public:
        ConnectResult connect_le(std::string& err) override {
            ++le_attempts;
            err = "GDBus.Error:org.bluez.Error.InProgress: In Progress";
            return ConnectResult::Failed;
        }
    } busy;

    BearerSupervisor sup(busy, true);
    int64_t now = 0;
    sup.tick(now);
    sup.tick(++now);
    now += BEARER_SETTLE_SECONDS;
    sup.tick(now);

    EXPECT_EQ(sup.status().le_backoff, 0) << "an outstanding registration was treated as a refusal";
    EXPECT_NE(sup.status().reason.find("Waiting for the iPhone"), std::string::npos);
}

// A registration BlueZ refuses outright is different: nothing is listening, and
// the status has to say so rather than implying the phone is being waited on.
TEST(BearerSupervisor, ARefusedRegistrationSaysNothingIsListening) {
    FakeBearer ops;
    ops.le_succeeds = false;
    ops.le_error = "org.bluez.Error.AuthenticationFailed";
    BearerSupervisor sup(ops, true);

    int64_t now = 0;
    sup.tick(now);
    sup.tick(++now);
    now += BEARER_SETTLE_SECONDS;
    while (ops.le_attempts < LE_ATTEMPTS_BEFORE_ADVICE) {
        sup.tick(now);
        now += BEARER_BACKOFF_MAX_SECONDS;
    }
    sup.tick(now);

    EXPECT_NE(sup.status().reason.find("Nothing is listening"), std::string::npos);
    EXPECT_NE(sup.status().reason.find("systemctl restart bluetooth"), std::string::npos);
    EXPECT_NE(sup.status().reason.find("Messages and contacts are unaffected"), std::string::npos);
    EXPECT_GT(sup.status().le_backoff, BEARER_BACKOFF_MIN_SECONDS) << "a refusal must still back off";
}

// The two routes to an LE link cannot run together. Measured 2026-08-19 with the
// PreferredBearer race already removed: advert off -> connects first attempt, zero
// aborts; advert on -> four le-connection-abort-by-local and no link. So they
// alternate, and the phase has to be bounded by time rather than by failures --
// an outstanding registration suppresses new attempts, so failures stop accruing
// and a failure-counted phase would never end, leaving the advert off forever.
TEST(BearerSupervisor, AlternatesBetweenDiallingAndSoliciting) {
    class ListeningBearer : public FakeBearer {
    public:
        ConnectResult connect_le(std::string&) override {
            ++le_attempts;
            outstanding = true; // BlueZ holds the registration, as it really does
            return ConnectResult::Requested;
        }
    } ops;

    BearerSupervisor sup(ops, true);
    int64_t now = 0;
    sup.tick(now);
    sup.tick(++now);
    now += BEARER_SETTLE_SECONDS;
    sup.tick(now);
    EXPECT_TRUE(sup.status().le_dialling) << "the dial window should come first";
    EXPECT_EQ(ops.le_attempts, 1);

    // The clock runs out while BlueZ is still connecting. Soliciting now would
    // abort that connect, so the window stays open until the reply lands.
    now += LE_DIAL_WINDOW_SECONDS;
    sup.tick(now);
    EXPECT_TRUE(sup.status().le_dialling) << "solicited on top of a dial that was still in flight";

    // connect_le() abandons the call at its own timeout, which is what ends it.
    ops.outstanding = false;
    sup.tick(++now);
    EXPECT_FALSE(sup.status().le_dialling) << "deadlocked: an open registration froze the dial phase";

    // No connect may be issued anywhere in the solicitation window.
    const int attempts = ops.le_attempts;
    const int64_t cycle = LE_DIAL_WINDOW_SECONDS + LE_SOLICIT_WINDOW_SECONDS;
    while (now < cycle - 2) {
        sup.tick(++now);
        ASSERT_FALSE(sup.status().le_dialling) << "left the solicitation window early, at t=" << now;
    }
    EXPECT_EQ(ops.le_attempts, attempts) << "dialled while the solicitation was broadcasting";

    // ...and then it comes back around to dialling.
    now = cycle + 1;
    sup.tick(now);
    EXPECT_TRUE(sup.status().le_dialling) << "never returned to the dial route";
}

// A link that is up needs neither route, so nothing should be suppressed by it.
TEST(BearerSupervisor, NotDiallingOnceLeIsUp) {
    FakeBearer ops;
    BearerSupervisor sup(ops, true);

    sup.tick(0);
    sup.tick(1);
    sup.tick(1 + BEARER_SETTLE_SECONDS);
    sup.tick(2 + BEARER_SETTLE_SECONDS);
    ASSERT_TRUE(sup.status().le_connected);
    EXPECT_FALSE(sup.status().le_dialling);
}

// A dead LE link has two very different causes. A timeout is the phone ignoring
// us, which only cycling Bluetooth on the phone clears; a refusal is BlueZ
// declining to register, which needs bluetoothd restarted. Sharing one message
// sent debugging to the wrong machine.
TEST(BearerSupervisor, TellsPhoneSilenceApartFromABlueZRefusal) {
    FakeBearer silent;
    silent.le_succeeds = false;
    silent.le_error = "GDBus.Error:org.bluez.Error.Failed: Connection timed out";
    BearerSupervisor a(silent, true);
    int64_t now = 0;
    a.tick(now);
    a.tick(++now);
    now += BEARER_SETTLE_SECONDS;
    while (silent.le_attempts < LE_ATTEMPTS_BEFORE_ADVICE) {
        a.tick(now);
        now += BEARER_BACKOFF_MAX_SECONDS;
    }
    a.tick(now);
    EXPECT_NE(a.status().reason.find("not answering on LE"), std::string::npos);
    EXPECT_NE(a.status().reason.find("turn Bluetooth off and back on on the iPhone"), std::string::npos);
    EXPECT_EQ(a.status().reason.find("systemctl"), std::string::npos) << "blamed the local stack for the phone";

    FakeBearer refused;
    refused.le_succeeds = false;
    refused.le_error = "org.bluez.Error.AuthenticationFailed";
    BearerSupervisor b(refused, true);
    now = 0;
    b.tick(now);
    b.tick(++now);
    now += BEARER_SETTLE_SECONDS;
    while (refused.le_attempts < LE_ATTEMPTS_BEFORE_ADVICE) {
        b.tick(now);
        now += BEARER_BACKOFF_MAX_SECONDS;
    }
    b.tick(now);
    EXPECT_NE(b.status().reason.find("Nothing is listening"), std::string::npos);
    EXPECT_EQ(b.status().reason.find("not answering"), std::string::npos);
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

// The advice threshold names the message, not the end of the retry. Letting it
// gate the loop meant one daemon lifetime got six LE attempts and then never
// dialled again -- the "worked all morning, then stopped for good" report.
TEST(BearerSupervisor, KeepsDiallingPastTheAdviceThreshold) {
    FakeBearer ops;
    ops.le_succeeds = false;
    ops.le_error = "GDBus.Error:org.bluez.Error.Failed: Connection timed out";
    BearerSupervisor sup(ops, true);

    int64_t now = 0;
    sup.tick(now);
    sup.tick(++now);
    now += BEARER_SETTLE_SECONDS;

    // Well past the threshold, over many dial/solicit cycles.
    const int64_t cycle = LE_DIAL_WINDOW_SECONDS + LE_SOLICIT_WINDOW_SECONDS;
    for (int i = 0; i < 20; ++i) {
        sup.tick(now);
        now += cycle;
    }
    EXPECT_GT(ops.le_attempts, LE_ATTEMPTS_BEFORE_ADVICE) << "stopped dialling for good";
    EXPECT_NE(sup.status().reason.find("not answering on LE"), std::string::npos);
}

// A dial that BlueZ has not answered yet must keep the solicitation off the air:
// raising an advert mid-connect is what produced le-connection-abort-by-local on
// every cycle, so the phone was never dialled and never solicited cleanly.
TEST(BearerSupervisor, HoldsTheSolicitationOffWhileADialIsInFlight) {
    class ListeningBearer : public FakeBearer {
    public:
        ConnectResult connect_le(std::string&) override {
            ++le_attempts;
            outstanding = true;
            return ConnectResult::Requested;
        }
    } ops;

    BearerSupervisor sup(ops, true);
    int64_t now = 0;
    sup.tick(now);
    sup.tick(++now);
    now += BEARER_SETTLE_SECONDS;
    sup.tick(now);
    ASSERT_TRUE(ops.outstanding);

    const int64_t cycle = LE_DIAL_WINDOW_SECONDS + LE_SOLICIT_WINDOW_SECONDS;
    for (int64_t t = now; t < now + 3 * cycle; t += BEARER_POLL_SECONDS) {
        sup.tick(t);
        ASSERT_TRUE(sup.status().le_dialling) << "solicited over an outstanding dial at t=" << t;
    }
}

// LE and BR/EDR back off to different ceilings on purpose. The LE failure this
// recovers from is usually cleared by the user cycling Bluetooth on the phone,
// and BR/EDR's five-minute ceiling meant a phone that had just been fixed still
// looked broken for minutes. A dial is cheap now that one is never issued while
// another is outstanding.
TEST(BearerSupervisor, LeBacksOffLessFarThanClassic) {
    static_assert(LE_BACKOFF_MAX_SECONDS < BEARER_BACKOFF_MAX_SECONDS);

    FakeBearer ops;
    ops.le_succeeds = false;
    ops.le_error = "org.bluez.Error.AuthenticationFailed"; // not transient, so it grows
    BearerSupervisor sup(ops, true);

    int64_t now = 0;
    sup.tick(now);
    sup.tick(++now);
    now += BEARER_SETTLE_SECONDS;

    // Only the dial window issues attempts, so grow it one whole cycle at a time.
    const int64_t cycle = LE_DIAL_WINDOW_SECONDS + LE_SOLICIT_WINDOW_SECONDS;
    for (int i = 0; i < 20; ++i) {
        sup.tick(now);
        now += cycle;
    }
    EXPECT_EQ(sup.status().le_backoff, LE_BACKOFF_MAX_SECONDS) << "LE inherited the BR/EDR ceiling";
}
