// probe_tsan.cpp — only for the TSan tier, built with -fsanitize=thread -O1 -g (never with ASan)
#include <thread>
int g;
int main() { std::thread t([] { g = 1; }); g = 2; t.join(); return 0; }
