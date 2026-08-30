#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <vector>

namespace tether {

    class EpollEventLoop {
    public:
        EpollEventLoop();
        ~EpollEventLoop();

        // Disable copy/move
        EpollEventLoop(const EpollEventLoop&) = delete;
        EpollEventLoop& operator=(const EpollEventLoop&) = delete;

        using Callback = std::function<void(int fd)>;

        // Add a file descriptor to be watched for EPOLLIN
        bool addFd(int fd, Callback cb);

        // Queue a function to run on the loop thread. The only member safe to call
        // from another thread.
        void post(std::function<void()> fn);

        // Remove a file descriptor from being watched
        bool removeFd(int fd);

        // Run the event loop (blocks forever until stopped)
        void run();

        // Signal the event loop to stop
        void stop();

    private:
        void drain_posts();

        int epoll_fd_;
        int post_fd_ = -1;
        std::atomic<bool> running_;
        std::map<int, Callback> callbacks_;
        std::mutex callbacks_mutex_;
        std::mutex post_mutex_;
        std::vector<std::function<void()>> posted_;
    };

} // namespace tether
