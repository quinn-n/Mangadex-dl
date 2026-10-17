// Positive control: a direct leak of 1337 bytes. Built at -O0 (see CMakeLists.txt) because stack
// residue at -O1 can keep the pointer reachable and hide the leak. Deliberately buggy; not linted
// (see .clang-tidy).
#include <cstdlib>

namespace {
[[gnu::noinline]] void leak() {
    void* volatile p = std::malloc(1337);
    static_cast<void>(p);
}
} // namespace

int main() {
    leak();
    return 0;
}
