#include <gtest/gtest.h>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>
#include <tether/log.hpp>
#include <thread>
#include <vector>

// The daemon logs from several threads at once. Every line must arrive whole:
// a line assembled from separate stream inserters interleaves mid-line.
TEST(LogTest, ConcurrentLinesAreNotInterleaved) {
    constexpr int threads = 8;
    constexpr int lines_per_thread = 200;

    std::ostringstream captured;
    std::streambuf* previous = std::cerr.rdbuf(captured.rdbuf());

    std::vector<std::thread> workers;
    for (int t = 0; t < threads; ++t) {
        workers.emplace_back([t] {
            for (int i = 0; i < lines_per_thread; ++i)
                debug::log(INFO, "thread {} line {}", t, i);
        });
    }
    for (auto& worker : workers)
        worker.join();

    std::cerr.rdbuf(previous);

    const std::regex expected(R"(^\[\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3}\] \[INFO\] thread \d+ line \d+$)");
    std::istringstream stream(captured.str());
    std::string line;
    int count = 0;
    while (std::getline(stream, line)) {
        ++count;
        EXPECT_TRUE(std::regex_match(line, expected)) << "torn line: " << line;
    }
    EXPECT_EQ(count, threads * lines_per_thread);
}
