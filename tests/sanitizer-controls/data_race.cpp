// Positive control (tsan preset only): two unsynchronized writes to one global, which TSan must
// report. The sleep orders the writes in time but is NOT synchronization, so it is still a data
// race. It makes detection reliable (without it, the report was observed to be intermittent). The
// global has external linkage so the optimizer can't delete the unread stores. Deliberately buggy;
// not linted (see .clang-tidy).
#include <chrono>
#include <thread>

int g_sanitizer_control_shared = 0;

int main() {
    std::thread t([] { g_sanitizer_control_shared = 1; });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    g_sanitizer_control_shared = 2;
    t.join();
    return 0;
}
