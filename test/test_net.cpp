#include <gtest/gtest.h>

#include <tether/crypto.hpp>
#include <tether/event_loop.hpp>
#include <tether/log.hpp>
#include <tether/net.hpp>

#include <arpa/inet.h>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {

    // The single-instance lock fd is deliberately leaked so the OS holds it for the
    // process lifetime. Without O_CLOEXEC any helper the daemon exec's (btmgmt, the
    // pair dialog) inherits it and keeps the lock alive after the daemon is gone,
    // which permanently blocks restarts with "tetherd is already running".
    TEST(SingleInstanceLockTest, ExecedHelpersDoNotKeepTheLockAliveAfterTheDaemonExits) {
        const std::string runtime_dir = "/tmp/tether_lock_test";
        std::filesystem::remove_all(runtime_dir);
        std::filesystem::create_directories(runtime_dir);
        setenv("XDG_RUNTIME_DIR", runtime_dir.c_str(), 1);

        int pipefd[2];
        ASSERT_EQ(pipe(pipefd), 0);

        const pid_t daemon = fork();
        ASSERT_GE(daemon, 0);
        if (daemon == 0) {
            close(pipefd[0]);
            tether::ensure_single_instance();

            const pid_t helper = fork();
            if (helper == 0) {
                execlp("sleep", "sleep", "30", nullptr);
                _exit(127);
            }
            if (helper < 0)
                _exit(1);

            if (write(pipefd[1], &helper, sizeof(helper)) != static_cast<ssize_t>(sizeof(helper)))
                _exit(1);
            close(pipefd[1]);
            _exit(0);
        }

        close(pipefd[1]);
        pid_t helper = -1;
        ASSERT_EQ(read(pipefd[0], &helper, sizeof(helper)), static_cast<ssize_t>(sizeof(helper)));
        close(pipefd[0]);

        int wstatus = 0;
        ASSERT_EQ(waitpid(daemon, &wstatus, 0), daemon);
        ASSERT_TRUE(WIFEXITED(wstatus));
        ASSERT_EQ(WEXITSTATUS(wstatus), 0);

        // Between fork and exec the helper holds a plain duplicate of the lock fd;
        // O_CLOEXEC only takes effect once the exec completes.
        const std::string comm_path = "/proc/" + std::to_string(helper) + "/comm";
        bool execed = false;
        for (int attempt = 0; attempt < 200 && !execed; ++attempt) {
            std::ifstream comm(comm_path);
            std::string name;
            if (comm && std::getline(comm, name) && name == "sleep")
                execed = true;
            else
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ASSERT_TRUE(execed) << "helper never exec'd; cannot tell the lock apart from a failed exec";

        const std::string lock_path = tether::get_runtime_dir() + "/tetherd.lock";
        const int fd = open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        ASSERT_GE(fd, 0);
        EXPECT_EQ(flock(fd, LOCK_EX | LOCK_NB), 0)
            << "the exec'd helper still holds the daemon lock (errno " << errno << ")";
        close(fd);

        kill(helper, SIGKILL);
        std::filesystem::remove_all(runtime_dir);
    }

    // Captures std::cerr under the same mutex debug::log() writes with, so the
    // loop thread cannot tear a line while the test reads the buffer.
    class CerrCapture {
    public:
        CerrCapture() {
            std::lock_guard<std::mutex> lock(debug::log_mutex);
            previous_ = std::cerr.rdbuf(buffer_.rdbuf());
        }
        ~CerrCapture() { restore(); }

        std::string str() {
            std::lock_guard<std::mutex> lock(debug::log_mutex);
            return buffer_.str();
        }

        void restore() {
            std::lock_guard<std::mutex> lock(debug::log_mutex);
            if (previous_) {
                std::cerr.rdbuf(previous_);
                previous_ = nullptr;
            }
        }

    private:
        std::ostringstream buffer_;
        std::streambuf* previous_ = nullptr;
    };

    // A failed handshake used to close the socket with no trace at all, so a phone
    // stuck in a connect/retry loop looked identical to a phone that never called.
    TEST(TcpServerTest, ReportsWhyATlsHandshakeFailed) {
        const std::string home = "/tmp/tether_net_test";
        std::filesystem::remove_all(home);
        std::filesystem::create_directories(home);
        setenv("HOME", home.c_str(), 1);
        ASSERT_TRUE(tether::Crypto::instance().init());

        constexpr int port = 45134;
        tether::EpollEventLoop loop;
        tether::TcpServer server(loop, port);
        if (!server.start())
            GTEST_SKIP() << "port " << port << " is unavailable";

        CerrCapture captured;
        std::thread loop_thread([&loop] { loop.run(); });

        const int client = socket(AF_INET, SOCK_STREAM, 0);
        ASSERT_GE(client, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ASSERT_EQ(connect(client, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);

        // Plain text where a TLS ClientHello belongs.
        const char garbage[] = "this is not a tls client hello\n";
        ASSERT_GT(write(client, garbage, sizeof(garbage) - 1), 0);

        std::string log;
        for (int attempt = 0; attempt < 200; ++attempt) {
            log = captured.str();
            if (log.find("TLS handshake failed") != std::string::npos)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        close(client);
        loop.post([&loop] { loop.stop(); });
        loop_thread.join();
        captured.restore();

        EXPECT_NE(log.find("TLS handshake failed"), std::string::npos) << "captured log:\n" << log;
        EXPECT_NE(log.find("127.0.0.1"), std::string::npos) << "captured log:\n" << log;

        std::filesystem::remove_all(home);
    }

} // namespace
