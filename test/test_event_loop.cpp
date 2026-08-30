#include <gtest/gtest.h>
#include <tether/event_loop.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

using namespace tether;

TEST(EventLoopTest, RunsPostedWorkOnTheLoopThread) {
    EpollEventLoop loop;
    std::atomic<int> ran{0};
    std::thread::id loop_thread;

    std::thread poster([&] {
        loop.post([&] { ++ran; });
        loop.post([&] {
            ++ran;
            loop_thread = std::this_thread::get_id();
            loop.stop();
        });
    });

    loop.run();
    poster.join();

    EXPECT_EQ(ran.load(), 2);
    EXPECT_EQ(loop_thread, std::this_thread::get_id());
}

TEST(EventLoopTest, RunsWorkPostedBeforeRun) {
    EpollEventLoop loop;
    bool ran = false;

    loop.post([&] {
        ran = true;
        loop.stop();
    });
    loop.run();

    EXPECT_TRUE(ran);
}

TEST(EventLoopTest, StopFromAnotherThreadWakesTheLoop) {
    EpollEventLoop loop;
    auto run_future = std::async(std::launch::async, [&] { loop.run(); });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    loop.stop();

    EXPECT_EQ(run_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
}
