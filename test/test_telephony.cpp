#include <gtest/gtest.h>
#include <tether/bluetooth/objects.hpp>
#include <tether/bluetooth/telephony.hpp>

using namespace tether::bluetooth;

namespace {

    // Same shape BlueZ returns from GetManagedObjects, parsed from GVariant text
    // so the tests need no bus.
    struct Payload {
        GVariant* v = nullptr;

        explicit Payload(const char* text) {
            GError* error = nullptr;
            v = g_variant_parse(G_VARIANT_TYPE("a{oa{sa{sv}}}"), text, nullptr, nullptr, &error);
            if (!v) {
                ADD_FAILURE() << "bad fixture: " << (error ? error->message : "unknown");
                g_clear_error(&error);
            }
        }
        ~Payload() {
            if (v)
                g_variant_unref(v);
        }
        Payload(const Payload&) = delete;
        Payload& operator=(const Payload&) = delete;
    };

    // Two phones, each with a gateway; one is ringing, the other is on a call.
    // Recorded from bluetoothd 5.87 --experimental against a live iPhone.
    constexpr const char* TWO_GATEWAYS = R"({
      '/org/bluez/hci0/dev_60_57_C8_30_6A_F7': {
        'org.bluez.Device1': { 'Address': <'60:57:C8:30:6A:F7'>, 'Alias': <'Zack 15 Pro'> }
      },
      '/org/bluez/hci0/dev_60_57_C8_30_6A_F7/telephony0': {
        'org.bluez.Telephony1': {
          'UUID': <'0000111f-0000-1000-8000-00805f9b34fb'>,
          'State': <'connected'>,
          'OperatorName': <'AT&T'>,
          'SupportedURISchemes': <['tel']>,
          'Signal': <byte 1>,
          'BattChg': <byte 4>,
          'Service': <true>,
          'Roaming': <false>,
          'InbandRingtone': <false>
        }
      },
      '/org/bluez/hci0/dev_60_57_C8_30_6A_F7/telephony0/call1': {
        'org.bluez.Call1': {
          'LineIdentification': <'+15555550123'>,
          'Name': <''>,
          'IncomingLine': <''>,
          'State': <'incoming'>,
          'Multiparty': <false>
        }
      },
      '/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF': {
        'org.bluez.Device1': { 'Address': <'AA:BB:CC:DD:EE:FF'>, 'Alias': <'Other'> }
      },
      '/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF/telephony0': {
        'org.bluez.Telephony1': { 'State': <'connected'>, 'Signal': <byte 5> }
      },
      '/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF/telephony0/call1': {
        'org.bluez.Call1': { 'LineIdentification': <'+15555559999'>, 'State': <'active'> }
      }
    })";

} // namespace

TEST(DialString, KeepsDigitsAndLeadingPlus) {
    EXPECT_EQ(normalize_dial_string("+15555550123"), "+15555550123");
    EXPECT_EQ(normalize_dial_string("5555550123"), "5555550123");
    EXPECT_EQ(normalize_dial_string("+1 (555) 555-0123"), "+15555550123");
    EXPECT_EQ(normalize_dial_string("555.555.0123"), "5555550123");
    EXPECT_EQ(normalize_dial_string("\t555 555 0123 "), "5555550123");
}

// The dial string reaches the cellular network. Anything that is not a number is
// refused here rather than passed to BlueZ to judge.
TEST(DialString, RefusesEverythingElse) {
    EXPECT_TRUE(normalize_dial_string("").empty());
    EXPECT_TRUE(normalize_dial_string("+").empty());
    EXPECT_TRUE(normalize_dial_string("   ").empty());
    EXPECT_TRUE(normalize_dial_string("not a number").empty());
    // Supplementary service and AT syntax, the reason this check exists. BlueZ
    // would accept these characters; nothing in the UI asks for them.
    EXPECT_TRUE(normalize_dial_string("*21*5551234#").empty());
    EXPECT_TRUE(normalize_dial_string("555;ATH").empty());
    EXPECT_TRUE(normalize_dial_string("555\r\nATD666").empty());
    EXPECT_TRUE(normalize_dial_string("555,,123").empty());
    // A '+' anywhere but the front is not a country code.
    EXPECT_TRUE(normalize_dial_string("555+123").empty());
    EXPECT_TRUE(normalize_dial_string("++15555550123").empty());
    // Long enough to be a paste accident.
    EXPECT_TRUE(normalize_dial_string(std::string(33, '5')).empty());
    EXPECT_FALSE(normalize_dial_string(std::string(32, '5')).empty());
}

TEST(Telephony, ParsesTheGateway) {
    Payload payload(TWO_GATEWAYS);
    const BluezObjects objects = parse_managed_objects(payload.v);

    const Telephony* gw = objects.find_telephony("/org/bluez/hci0/dev_60_57_C8_30_6A_F7");
    ASSERT_NE(gw, nullptr);
    EXPECT_EQ(gw->path, "/org/bluez/hci0/dev_60_57_C8_30_6A_F7/telephony0");
    EXPECT_EQ(gw->operator_name, "AT&T");
    EXPECT_EQ(gw->signal, 1);
    EXPECT_EQ(gw->battery, 4);
    EXPECT_TRUE(gw->service);
    EXPECT_FALSE(gw->roaming);
    EXPECT_TRUE(gw->ready());
    EXPECT_EQ(gw->uri_schemes, std::vector<std::string>{"tel"});

    EXPECT_EQ(objects.find_telephony("/org/bluez/hci0/dev_11_22_33_44_55_66"), nullptr);
    EXPECT_EQ(objects.find_telephony(""), nullptr);
}

// A second phone's calls must never appear under the selected one.
TEST(Telephony, ParsesOnlyTheSelectedGatewaysCalls) {
    Payload payload(TWO_GATEWAYS);
    const BluezObjects objects = parse_managed_objects(payload.v);

    const auto calls = objects.calls_for("/org/bluez/hci0/dev_60_57_C8_30_6A_F7/telephony0");
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].number, "+15555550123");
    EXPECT_EQ(calls[0].state, "incoming");
    EXPECT_TRUE(calls[0].ringing());
    EXPECT_FALSE(calls[0].connected());

    EXPECT_TRUE(objects.calls_for("").empty());
    EXPECT_TRUE(objects.calls_for("/org/bluez/hci0/dev_60_57_C8_30_6A_F7/telephony9").empty());
}

// The gateway path is a prefix of its calls' paths, and BlueZ numbers both per
// device: telephony1 must not collect telephony10's calls.
TEST(Telephony, GatewayPathIsMatchedWholeNotAsAPrefix) {
    Payload payload(R"({
      '/org/bluez/hci0/dev_60_57_C8_30_6A_F7/telephony1/call1': {
        'org.bluez.Call1': { 'State': <'active'> }
      },
      '/org/bluez/hci0/dev_60_57_C8_30_6A_F7/telephony10/call1': {
        'org.bluez.Call1': { 'State': <'active'> }
      }
    })");
    const BluezObjects objects = parse_managed_objects(payload.v);
    EXPECT_EQ(objects.calls_for("/org/bluez/hci0/dev_60_57_C8_30_6A_F7/telephony1").size(), 1u);
    EXPECT_EQ(objects.calls_for("/org/bluez/hci0/dev_60_57_C8_30_6A_F7/telephony10").size(), 1u);
}

// The objects exist only under bluetoothd --experimental and only while HFP is
// connected. Their absence is the normal state, not a parse failure.
TEST(Telephony, AbsentWhenHandsFreeIsNotConnected) {
    Payload payload(R"({
      '/org/bluez/hci0/dev_60_57_C8_30_6A_F7': {
        'org.bluez.Device1': { 'Address': <'60:57:C8:30:6A:F7'> }
      }
    })");
    const BluezObjects objects = parse_managed_objects(payload.v);
    EXPECT_TRUE(objects.telephony.empty());
    EXPECT_TRUE(objects.calls.empty());
    EXPECT_EQ(objects.find_telephony("/org/bluez/hci0/dev_60_57_C8_30_6A_F7"), nullptr);
}

// A gateway that is still bringing up its service level connection is not ready
// to take a call.
TEST(Telephony, IsNotReadyBeforeTheServiceLevelConnection) {
    for (const char* state : {"connecting", "slc_connecting", "disconnecting"}) {
        Telephony gw;
        gw.state = state;
        EXPECT_FALSE(gw.ready()) << state;
    }
    Telephony gw;
    gw.state = "connected";
    EXPECT_TRUE(gw.ready());
}

TEST(Telephony, TracksACallThroughItsStates) {
    Call call;
    call.state = "incoming";
    EXPECT_TRUE(call.ringing());

    call.state = "active";
    EXPECT_FALSE(call.ringing());
    EXPECT_TRUE(call.connected());

    call.state = "held";
    EXPECT_TRUE(call.connected());

    call.state = "disconnected";
    EXPECT_FALSE(call.connected());
    EXPECT_FALSE(call.ringing());
    EXPECT_FALSE(call.outgoing());

    // An outgoing call runs dialing then alerting before it connects.
    Call outgoing;
    outgoing.state = "dialing";
    EXPECT_TRUE(outgoing.outgoing());
    outgoing.state = "alerting";
    EXPECT_TRUE(outgoing.outgoing());
    outgoing.state = "active";
    EXPECT_FALSE(outgoing.outgoing());
}

// A state this build has not seen must still reach the UI rather than being
// dropped or guessed at. response_and_hold is one BlueZ can report.
TEST(Telephony, PassesUnknownStatesThrough) {
    Call call;
    call.state = "response_and_hold";
    EXPECT_EQ(call.state, "response_and_hold");
    EXPECT_FALSE(call.ringing());
    EXPECT_FALSE(call.connected());
}

// "withheld" is the network refusing caller ID, not a number. Offering it to
// redial would dial nothing.
TEST(Telephony, WithheldCallerIdIsNotANumber) {
    Call call;
    call.number = "withheld";
    call.state = "incoming";
    EXPECT_TRUE(call.withheld());

    const nlohmann::json j = to_json(call);
    EXPECT_EQ(j["number"], "");
    EXPECT_TRUE(j["withheld"]);
    EXPECT_TRUE(normalize_dial_string(j["number"].get<std::string>()).empty());
}

TEST(Telephony, SerializesCallsForClients) {
    Payload payload(TWO_GATEWAYS);
    const BluezObjects objects = parse_managed_objects(payload.v);
    const nlohmann::json j = to_json(objects.calls_for("/org/bluez/hci0/dev_60_57_C8_30_6A_F7/telephony0"));

    ASSERT_EQ(j.size(), 1u);
    EXPECT_EQ(j[0]["number"], "+15555550123");
    EXPECT_EQ(j[0]["state"], "incoming");
    EXPECT_TRUE(j[0]["ringing"]);
    EXPECT_FALSE(j[0]["connected"]);
    EXPECT_TRUE(to_json(std::vector<Call>{}).empty());
}

TEST(Telephony, SerializesTheGatewayForClients) {
    Payload payload(TWO_GATEWAYS);
    const BluezObjects objects = parse_managed_objects(payload.v);
    const nlohmann::json j = to_json(*objects.find_telephony("/org/bluez/hci0/dev_60_57_C8_30_6A_F7"));

    EXPECT_TRUE(j["available"]);
    EXPECT_EQ(j["operator"], "AT&T");
    EXPECT_EQ(j["signal"], 1);
    EXPECT_EQ(j["battery"], 4);
    EXPECT_FALSE(j["roaming"]);
}
