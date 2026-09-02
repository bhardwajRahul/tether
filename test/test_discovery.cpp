#include <gtest/gtest.h>
#include <tether/discovery.hpp>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

TEST(DiscoveryTest, GroupDiscoveredHosts_EmptyInput) {
    std::vector<tether::DiscoveredHost> hosts;
    auto result = tether::group_discovered_hosts(hosts);
    EXPECT_TRUE(result.empty());
}

TEST(DiscoveryTest, GroupDiscoveredHosts_SingleHost) {
    std::vector<tether::DiscoveredHost> hosts = {
        {"device.local", "192.168.1.10", 5134, "abc123"}
    };
    auto result = tether::group_discovered_hosts(hosts);
    ASSERT_EQ(result.size(), 1);
    EXPECT_EQ(result[0].name, "device.local");
    EXPECT_EQ(result[0].fingerprint, "abc123");
    ASSERT_EQ(result[0].addresses.size(), 1);
    EXPECT_EQ(result[0].addresses[0].address, "192.168.1.10");
    EXPECT_EQ(result[0].addresses[0].port, 5134);
}

TEST(DiscoveryTest, GroupDiscoveredHosts_MultipleAddressesSameFingerprint) {
    std::vector<tether::DiscoveredHost> hosts = {
        {"device.local", "192.168.1.10", 5134, "abc123"},
        {"device.local", "192.168.1.11", 5134, "abc123"},
        {"device.local", "::1", 5134, "abc123"}
    };
    auto result = tether::group_discovered_hosts(hosts);
    ASSERT_EQ(result.size(), 1);
    EXPECT_EQ(result[0].fingerprint, "abc123");
    ASSERT_EQ(result[0].addresses.size(), 3);
    EXPECT_EQ(result[0].addresses[0].address, "192.168.1.10");
    EXPECT_EQ(result[0].addresses[1].address, "192.168.1.11");
    EXPECT_EQ(result[0].addresses[2].address, "::1");
}

TEST(DiscoveryTest, GroupDiscoveredHosts_MultipleDevices) {
    std::vector<tether::DiscoveredHost> hosts = {
        {"device1.local", "192.168.1.10", 5134, "fp1"},
        {"device2.local", "192.168.1.20", 5134, "fp2"}
    };
    auto result = tether::group_discovered_hosts(hosts);
    ASSERT_EQ(result.size(), 2);
    EXPECT_EQ(result[0].fingerprint, "fp1");
    EXPECT_EQ(result[1].fingerprint, "fp2");
}

TEST(DiscoveryTest, GroupDiscoveredHosts_EmptyFingerprintGroupsByName) {
    std::vector<tether::DiscoveredHost> hosts = {
        {"device.local", "192.168.1.10", 5134, ""},
        {"device.local", "192.168.1.11", 5134, ""}
    };
    auto result = tether::group_discovered_hosts(hosts);
    ASSERT_EQ(result.size(), 1);
    EXPECT_EQ(result[0].name, "device.local");
    EXPECT_EQ(result[0].fingerprint, "");
    ASSERT_EQ(result[0].addresses.size(), 2);
}

TEST(DiscoveryTest, GroupDiscoveredHosts_IPv4Sorting) {
    std::vector<tether::DiscoveredHost> hosts = {
        {"device.local", "::1", 5134, "fp1"},
        {"device.local", "192.168.1.10", 5134, "fp1"},
        {"device.local", "192.168.1.5", 5134, "fp1"}
    };
    auto result = tether::group_discovered_hosts(hosts);
    ASSERT_EQ(result.size(), 1);
    EXPECT_EQ(result[0].addresses[0].address, "192.168.1.10");
    EXPECT_EQ(result[0].addresses[1].address, "192.168.1.5");
    EXPECT_EQ(result[0].addresses[2].address, "::1");
}

TEST(DiscoveryTest, GroupDiscoveredHosts_MixedFingerprintsAndNames) {
    std::vector<tether::DiscoveredHost> hosts = {
        {"device1", "192.168.1.10", 5134, "fp1"},
        {"device2", "192.168.1.20", 5134, ""},
        {"device1", "192.168.1.11", 5134, "fp1"},
        {"device2", "192.168.1.21", 5134, ""}
    };
    auto result = tether::group_discovered_hosts(hosts);
    ASSERT_EQ(result.size(), 2);
}

// A node advertises itself on the same LAN it browses, so without this filter it
// lists itself as a pairable peer.
TEST(DiscoveryTest, GroupDiscoveredHosts_ExcludesOwnFingerprint) {
    std::vector<tether::DiscoveredHost> hosts = {
        {"self", "192.168.1.10", 5134, "mine"},
        {"peer", "192.168.1.20", 5134, "theirs"},
        {"self", "10.0.0.5", 5134, "mine"}
    };

    auto result = tether::group_discovered_hosts(hosts, "mine");
    ASSERT_EQ(result.size(), 1);
    EXPECT_EQ(result[0].fingerprint, "theirs");

    // An empty exclusion keeps every device, so non-daemon callers are unaffected.
    EXPECT_EQ(tether::group_discovered_hosts(hosts, "").size(), 2);
}

// Publishing and continuous browse both watch the same daemon and each walks
// the same client states, so the availability report must collapse repeats:
// consecutive identical values would flap the UI indicator.
TEST(DiscoveryTest, StateCallbackReportsOnlyChanges) {
    tether::Discovery discovery;

    std::mutex mutex;
    std::vector<bool> reports;
    discovery.set_state_callback([&](bool available) {
        std::lock_guard<std::mutex> lock(mutex);
        reports.push_back(available);
    });

    discovery.publish("tether-test", 5134, "testfingerprint");
    discovery.start_continuous_browse([](const std::vector<tether::DiscoveredDevice>&) {});
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    std::lock_guard<std::mutex> lock(mutex);
    ASSERT_FALSE(reports.empty()) << "availability was never reported";
    for (size_t i = 1; i < reports.size(); ++i) {
        EXPECT_NE(reports[i], reports[i - 1]) << "duplicate report at index " << i;
    }
}
