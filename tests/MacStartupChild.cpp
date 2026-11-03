// SPDX-FileCopyrightText: 2026 Kwi contributors
// SPDX-License-Identifier: CC0-1.0

// Small controlled shell child: observe resize without Python/import delays.
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <poll.h>
#include <string>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

namespace
{
volatile sig_atomic_t resized{1};
void resizeSignal(int)
{
    resized = 1;
}
long long clockNs()
{
    timespec value{};
    clock_gettime(CLOCK_MONOTONIC_RAW, &value);
    return value.tv_sec * 1'000'000'000LL + value.tv_nsec;
}
void save(const std::filesystem::path &path, const winsize &size)
{
    auto temporary = path;
    temporary += ".tmp";
    std::ofstream stream{temporary};
    stream << "{\"pid\":" << getpid() << ",\"parent_pid\":" << getppid() << ",\"time_ns\":" << clockNs() << ",\"rows\":" << size.ws_row
           << ",\"columns\":" << size.ws_col << "}\n";
    stream.close();
    std::filesystem::rename(temporary, path);
}
}

int main(int argc, char **argv)
{
    if (argc != 4)
        return 2;
    const std::filesystem::path directory{argv[1]};
    const int columns = std::atoi(argv[2]), rows = std::atoi(argv[3]);
    if (columns < 1 || rows < 1 || columns > 1000 || rows > 1000)
        return 2;
    struct sigaction action{};
    action.sa_handler = resizeSignal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGWINCH, &action, nullptr);
    winsize size{};
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &size) != 0)
        return 2;
    save(directory / "ready.json", size);
    const auto deadline = clockNs() + 10'000'000'000LL;
    while (clockNs() < deadline) {
        if (resized) {
            resized = 0;
            if (ioctl(STDIN_FILENO, TIOCGWINSZ, &size) != 0)
                return 2;
            if (size.ws_col == columns && size.ws_row == rows) {
                const std::string marker{"KWI_STARTUP_SETTLED\r\n"};
                size_t offset{0};
                while (offset < marker.size()) {
                    const auto count = ::write(STDOUT_FILENO, marker.data() + offset, marker.size() - offset);
                    if (count > 0)
                        offset += size_t(count);
                    else if (count == 0 || errno != EINTR)
                        return 2;
                }
                save(directory / "settled.json", size);
                // Remain alive while the parent observes presentation. HUP on
                // closing the owned terminal ends this child immediately.
                const auto finish = clockNs() + 20'000'000'000LL;
                while (clockNs() < finish)
                    (void)::poll(nullptr, 0, 100);
                return 0;
            }
        }
        (void)::poll(nullptr, 0, 100);
    }
    save(directory / "wrong-geometry.json", size);
    return 1;
}
