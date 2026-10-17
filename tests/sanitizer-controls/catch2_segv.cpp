// Positive control: a null dereference inside a Catch2 TEST_CASE. Catch2 installs its own POSIX
// signal handlers; with ASAN_OPTIONS=handle_segv=2 ASan's accurate "ERROR: AddressSanitizer: SEGV"
// report must still appear. If it doesn't, Catch2 is hiding crash reports in this env. UBSan's
// `null` check is switched off for the one function, so the fault reaches the signal path instead
// of being reported earlier as "runtime error: load of null pointer". Deliberately buggy; not
// linted (see .clang-tidy).
#include <catch2/catch_test_macros.hpp>

namespace {
__attribute__((no_sanitize("null"), noinline)) int read_through(const int* p) { return *p; }
} // namespace

TEST_CASE("null dereference is reported by ASan, not hidden by Catch2", "[sanitizer-control]") {
    const int* volatile p = nullptr;
    CHECK(read_through(p) == 0);
}
