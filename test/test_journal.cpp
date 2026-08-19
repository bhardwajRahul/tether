#include <gtest/gtest.h>
#include <tether/bluetooth/journal.hpp>

#include <cstdlib>
#include <filesystem>
#include <string>

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

TEST(Journal, RoundTripsAMessage) {
    Message original = make("/msg/1", NOW, "hello \xF0\x9F\x92\xA9");
    original.read = true;
    original.outgoing = true;

    Message restored;
    ASSERT_TRUE(deserialize_message_line(serialize_message_line(original), restored));
    EXPECT_EQ(restored, original);
}

TEST(Journal, SerializesToASingleLine) {
    const std::string line = serialize_message_line(make("/msg/1", NOW, "two\nlines"));
    EXPECT_EQ(line.find('\n'), std::string::npos) << "an embedded newline would split one record into two";
}

// One bad write must not cost the whole history, so unreadable lines are
// skipped rather than aborting the load.
TEST(Journal, RejectsMalformedLines) {
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

namespace {

    // journal_path() is derived from $HOME, so the only way to point the journal
    // at a scratch file is to move HOME for the duration of the test.
    class ScopedHome {
    public:
        explicit ScopedHome(const std::filesystem::path& dir) {
            if (const char* previous = getenv("HOME"))
                previous_ = previous;
            setenv("HOME", dir.c_str(), 1);
        }
        ~ScopedHome() {
            if (previous_.empty())
                unsetenv("HOME");
            else
                setenv("HOME", previous_.c_str(), 1);
        }

    private:
        std::string previous_;
    };

} // namespace

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
