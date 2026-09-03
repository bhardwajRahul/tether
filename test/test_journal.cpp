#include "scoped_env.hpp"

#include <gtest/gtest.h>
#include <tether/bluetooth/journal.hpp>
#include <tether/secret_store.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using namespace tether;
using namespace tether::bluetooth;

namespace {

    Message make(const std::string& handle, int64_t timestamp, const std::string& body = "hi") {
        Message m;
        m.handle = handle;
        m.thread_key = "tel:+15551234567";
        m.peer_address = "+15551234567";
        m.body = body;
        m.timestamp = timestamp;
        m.folder = "inbox";
        return m;
    }

    constexpr int64_t NOW = 1786848300;
    constexpr int64_t DAY = 24 * 3600;

} // namespace

namespace {

    using tether::testing::ScopedEnv;

    // journal_path() is derived from $HOME, so the only way to point the journal
    // at a scratch file is to move HOME for the duration of the test. The store
    // key follows HOME, so scoping it also keeps the key out of the real wallet.
    class ScopedHome {
    public:
        explicit ScopedHome(const std::filesystem::path& dir, Retention mode = Retention::Encrypted)
            : home_("HOME", dir) {
            secret::reset_for_test();
            secret::set_retention(mode);
        }

        ~ScopedHome() { secret::reset_for_test(); }

    private:
        ScopedEnv home_;
    };

    std::vector<std::string> lines_of(const std::filesystem::path& path) {
        std::vector<std::string> out;
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line))
            if (!line.empty())
                out.push_back(line);
        return out;
    }

    std::filesystem::path scratch(const std::string& name) {
        const auto dir = std::filesystem::temp_directory_path() / ("tether-journal-" + name);
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        return dir;
    }

} // namespace

TEST(Journal, RoundTripsAMessage) {
    const auto home = scratch("round-trip");
    ScopedHome scoped(home);

    Message original = make("/msg/1", NOW, "hello \xF0\x9F\x92\xA9");
    original.read = true;
    original.outgoing = true;

    Message restored;
    ASSERT_TRUE(deserialize_message_line(serialize_message_line(original), restored));
    EXPECT_EQ(restored, original);
}

TEST(Journal, SerializesToASingleLine) {
    const auto home = scratch("single-line");
    ScopedHome scoped(home);

    const std::string line = serialize_message_line(make("/msg/1", NOW, "two\nlines"));
    EXPECT_EQ(line.find('\n'), std::string::npos) << "an embedded newline would split one record into two";
}

// One bad write must not cost the whole history, so unreadable lines are
// skipped rather than aborting the load.
TEST(Journal, RejectsMalformedLines) {
    const auto home = scratch("malformed");
    ScopedHome scoped(home);

    Message out;
    EXPECT_FALSE(deserialize_message_line("", out));
    EXPECT_FALSE(deserialize_message_line("{not json", out));
    EXPECT_FALSE(deserialize_message_line("[]", out));
    EXPECT_FALSE(deserialize_message_line("{\"body\":\"orphan\"}", out)) << "no handle means it can never dedupe";
    EXPECT_FALSE(deserialize_message_line("{\"handle\":\"/m/1\"}", out)) << "no thread key means it belongs nowhere";
}

TEST(Journal, RetentionDropsMessagesPastTheAgeLimit) {
    std::vector<Message> messages{
        make("/msg/old", NOW - 120 * DAY),
        make("/msg/recent", NOW - DAY),
    };

    auto kept = apply_retention(messages, NOW);
    ASSERT_EQ(kept.size(), 1u);
    EXPECT_EQ(kept[0].handle, "/msg/recent");
}

TEST(Journal, RetentionKeepsTheNewestWhenOverCount) {
    std::vector<Message> messages;
    for (int i = 0; i < 10; ++i)
        messages.push_back(make("/msg/" + std::to_string(i), NOW - (10 - i)));

    auto kept = apply_retention(messages, NOW, 3);
    ASSERT_EQ(kept.size(), 3u);
    EXPECT_EQ(kept[0].handle, "/msg/7");
    EXPECT_EQ(kept[2].handle, "/msg/9");
}

// MAP sometimes supplies no usable timestamp. Treating zero as 1970 would evict
// those messages instantly, which is worse than keeping them.
TEST(Journal, RetentionKeepsUndatedMessages) {
    std::vector<Message> messages{make("/msg/undated", 0)};

    auto kept = apply_retention(messages, NOW);
    ASSERT_EQ(kept.size(), 1u);
    EXPECT_EQ(kept[0].handle, "/msg/undated");
}

TEST(Journal, RetentionIsANoOpUnderBothLimits) {
    std::vector<Message> messages{make("/msg/1", NOW - DAY), make("/msg/2", NOW)};
    EXPECT_EQ(apply_retention(messages, NOW).size(), 2u);
}

// The journal is append-only and a reconnect re-lists everything, so the same
// message is written more than once. Without collapsing, history doubles on
// every reconnect.
TEST(Journal, RetentionCollapsesRepeatedHandles) {
    Message first = make("/msg/1", NOW);
    Message again = make("/msg/1", NOW);
    again.read = true;

    auto kept = apply_retention({first, again}, NOW);
    ASSERT_EQ(kept.size(), 1u);
    EXPECT_TRUE(kept[0].read) << "the newest copy carries the current read state";
}

// The unread badge has to survive a daemon restart, so a mark-read is only
// really applied once it has been written back. The journal is append-only, and
// retention keeps the last line for a handle: re-appending is the whole fix.
TEST(Journal, ReadStateSurvivesARestart) {
    const auto home = std::filesystem::temp_directory_path() / "tether-journal-read-state";
    std::filesystem::remove_all(home);
    std::filesystem::create_directories(home);
    ScopedHome scoped(home);

    const Message arrived = make("/msg/1", NOW);
    {
        MessageJournal journal;
        ASSERT_TRUE(journal.open());
        journal.append(arrived);
    }

    // A restart in between: the message comes back exactly as it went in.
    {
        MessageJournal journal;
        ASSERT_TRUE(journal.open());
        auto loaded = journal.load(NOW);
        ASSERT_EQ(loaded.size(), 1u);
        ASSERT_FALSE(loaded[0].read);

        Message seen = arrived;
        seen.read = true;
        journal.append(seen);
    }

    MessageJournal reader;
    ASSERT_TRUE(reader.open());
    auto loaded = reader.load(NOW);
    ASSERT_EQ(loaded.size(), 1u) << "the re-appended line was not collapsed onto the original";
    EXPECT_TRUE(loaded[0].read) << "the message came back unread, so the badge would refill on every restart";

    std::filesystem::remove_all(home);
}

TEST(Journal, EncryptedRetentionLeavesNothingReadable) {
    const auto home = scratch("encrypted");
    ScopedHome scoped(home);

    MessageJournal journal;
    ASSERT_TRUE(journal.open());
    journal.append(make("/msg/1", NOW, "meet me at the usual place"));

    const auto store = home / ".local" / "share" / "tether";
    EXPECT_FALSE(std::filesystem::exists(store / "messages.ndjson"))
        << "the plaintext name must stay empty, or an older tetherd would compact the store away";

    const auto sealed = store / "messages.ndjson.enc";
    ASSERT_TRUE(std::filesystem::exists(sealed));

    std::ifstream in(sealed);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(text.find("usual place"), std::string::npos) << "the message body is on disk in the clear";
    EXPECT_EQ(text.find("+15551234567"), std::string::npos) << "the phone number is on disk in the clear";
    EXPECT_EQ(text.find('{'), std::string::npos);

    std::filesystem::remove_all(home);
}

TEST(Journal, MigratesAPlaintextStoreOnFirstOpen) {
    const auto home = scratch("migrate");
    const auto store = home / ".local" / "share" / "tether";

    // A journal exactly as a pre-0.2.24 daemon left it.
    {
        ScopedHome scoped(home, Retention::Plaintext);
        MessageJournal journal;
        ASSERT_TRUE(journal.open());
        journal.append(make("/msg/1", NOW, "one"));
        journal.append(make("/msg/2", NOW + 1, "two"));
    }
    ASSERT_TRUE(std::filesystem::exists(store / "messages.ndjson"));

    {
        ScopedHome scoped(home);
        MessageJournal journal;
        ASSERT_TRUE(journal.open());

        EXPECT_FALSE(std::filesystem::exists(store / "messages.ndjson")) << "the plaintext copy outlived the migration";
        ASSERT_TRUE(std::filesystem::exists(store / "messages.ndjson.enc"));
        EXPECT_EQ(lines_of(store / "messages.ndjson.enc").size(), 2u);

        auto loaded = journal.load(NOW + 1);
        ASSERT_EQ(loaded.size(), 2u) << "migration lost messages";
        EXPECT_EQ(loaded[0].body, "one");
        EXPECT_EQ(loaded[1].body, "two");
    }

    std::filesystem::remove_all(home);
}

TEST(Journal, MigratesBackToPlaintextOnOptOut) {
    const auto home = scratch("migrate-back");
    const auto store = home / ".local" / "share" / "tether";

    {
        ScopedHome scoped(home);
        MessageJournal journal;
        ASSERT_TRUE(journal.open());
        journal.append(make("/msg/1", NOW, "one"));
    }

    // Flipping to plaintext has to move the data, not strand it behind a name
    // nothing reads any more.
    {
        ScopedHome scoped(home, Retention::Plaintext);
        MessageJournal journal;
        ASSERT_TRUE(journal.open());

        EXPECT_FALSE(std::filesystem::exists(store / "messages.ndjson.enc"));
        auto loaded = journal.load(NOW);
        ASSERT_EQ(loaded.size(), 1u);
        EXPECT_EQ(loaded[0].body, "one");
    }

    std::filesystem::remove_all(home);
}

// The regression that matters most. A locked wallet used to mean an empty read,
// and the compact that follows a read would then write that emptiness back over
// every message the user had.
TEST(Journal, ALockedWalletCannotDestroyHistory) {
    const auto home = scratch("locked");
    const auto sealed = home / ".local" / "share" / "tether" / "messages.ndjson.enc";

    {
        ScopedHome scoped(home);
        MessageJournal journal;
        ASSERT_TRUE(journal.open());
        journal.append(make("/msg/1", NOW, "one"));
        journal.append(make("/msg/2", NOW + 1, "two"));
    }
    const auto before = lines_of(sealed);
    ASSERT_EQ(before.size(), 2u);

    {
        // A different HOME is a different key, which is what a wallet that will
        // not open looks like from here.
        const auto elsewhere = scratch("locked-other");
        ScopedEnv home_env("HOME", elsewhere);
        secret::reset_for_test();

        MessageJournal journal;
        journal.open();
        std::filesystem::remove_all(elsewhere);
    }

    {
        ScopedHome scoped(home);
        MessageJournal journal;
        ASSERT_TRUE(journal.open());
        auto loaded = journal.load(NOW + 1);
        EXPECT_EQ(loaded.size(), 2u) << "history did not survive a pass with the wrong key";
    }
    EXPECT_EQ(lines_of(sealed), before) << "the journal was rewritten by a daemon that could not read it";

    std::filesystem::remove_all(home);
}

TEST(Journal, CompactRefusesToEmptyAPopulatedJournal) {
    const auto home = scratch("compact-guard");
    ScopedHome scoped(home);

    MessageJournal journal;
    ASSERT_TRUE(journal.open());
    journal.append(make("/msg/1", NOW, "one"));

    EXPECT_FALSE(journal.compact({})) << "compacting to nothing must be refused, not obeyed";
    EXPECT_EQ(journal.load(NOW).size(), 1u);

    std::filesystem::remove_all(home);
}

TEST(Journal, RetentionNoneWritesNothing) {
    const auto home = scratch("none");
    ScopedHome scoped(home, Retention::None);

    MessageJournal journal;
    EXPECT_FALSE(journal.open());
    journal.append(make("/msg/1", NOW, "one"));

    EXPECT_FALSE(std::filesystem::exists(home / ".local" / "share" / "tether" / "messages.ndjson"));
    EXPECT_FALSE(std::filesystem::exists(home / ".local" / "share" / "tether" / "messages.ndjson.enc"));

    std::filesystem::remove_all(home);
}
