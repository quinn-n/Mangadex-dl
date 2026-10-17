// probe_lsan.cpp — positive control (informational; retry once at -O0 if silent — stack residue can hide leaks)
#include <cstdlib>
[[gnu::noinline]] static void leak() { void* volatile p = std::malloc(1337); (void)p; }
int main() { leak(); return 0; }
