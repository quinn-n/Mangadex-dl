// Positive control: a 1-byte heap overflow (argc == 1 -> p[8] on an 8-byte block). volatile and
// argc stop the optimizer from folding the bug away at -O1. Deliberately buggy; not linted (see
// .clang-tidy).
#include <cstdlib>

int main(int argc, char** /*argv*/) {
    char* volatile p = static_cast<char*>(std::malloc(8));
    p[argc + 7] = 'x';
    std::free(p);
    return 0;
}
