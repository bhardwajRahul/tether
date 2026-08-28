#include "tether/event_loop.hpp"
#include <cstdint>
#include <stdexcept>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <tether/log.hpp>
#include <unistd.h>

namespace tether {

    EpollEventLoop::EpollEventLoop() : running_(false) {
        epoll_fd_ = epoll_create1(0);
        if (epoll_fd_ < 0) {
            throw std::runtime_error("Failed to create epoll file descriptor");
        }

        post_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (post_fd_ < 0) {
            close(epoll_fd_);
            throw std::runtime_error("Failed to create eventfd");
        }
        addFd(post_fd_, [this](int) { drain_posts(); });
    }

    EpollEventLoop::~EpollEventLoop() {
        stop();
        if (post_fd_ >= 0) {
            close(post_fd_);
        }
        if (epoll_fd_ >= 0) {
            close(epoll_fd_);
        }
    }

    void EpollEventLoop::post(std::function<void()> fn) {
        if (!fn)
            return;
        {
            std::lock_guard<std::mutex> lock(post_mutex_);
            posted_.push_back(std::move(fn));
        }
        uint64_t one = 1;
        if (write(post_fd_, &one, sizeof(one)) < 0) {
            debug::log(ERR, "Failed to wake the event loop for a posted task");
        }
    }

    void EpollEventLoop::drain_posts() {
        uint64_t ticks = 0;
        ssize_t ignored = read(post_fd_, &ticks, sizeof(ticks));
        (void)ignored;

        std::vector<std::function<void()>> batch;
        {
            std::lock_guard<std::mutex> lock(post_mutex_);
            batch.swap(posted_);
        }
        for (auto& fn : batch) {
            fn();
        }
    }

    bool EpollEventLoop::addFd(int fd, Callback cb) {
        struct epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.fd = fd;

        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
            debug::log(ERR, "Failed to add fd to epoll: {}", fd);
            return false;
        }
        callbacks_[fd] = std::move(cb);
        return true;
    }

    bool EpollEventLoop::removeFd(int fd) {
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr) < 0) {
            return false;
        }
        callbacks_.erase(fd);
        return true;
    }

    void EpollEventLoop::run() {
        running_ = true;
        const int MAX_EVENTS = 10;
        struct epoll_event events[MAX_EVENTS];

        while (running_) {
            int n = epoll_wait(epoll_fd_, events, MAX_EVENTS, -1);
            if (n == -1) {
                if (errno == EINTR)
                    continue;
                debug::log(ERR, "epoll_wait failed");
                break;
            }

            for (int i = 0; i < n; ++i) {
                int fd = events[i].data.fd;
                auto it = callbacks_.find(fd);
                if (it != callbacks_.end()) {
                    // Copy the callback to ensure it stays alive even if removeFd is called inside it
                    auto cb = it->second;
                    cb(fd);
                }
            }
        }
    }

    void EpollEventLoop::stop() { running_ = false; }

} // namespace tether
