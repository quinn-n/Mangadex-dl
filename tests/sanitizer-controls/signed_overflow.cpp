// Positive control: signed integer overflow (INT_MAX + argc), undefined behaviour that UBSan must
// report. Deliberately buggy; not linted (see .clang-tidy).
#include <climits>

int main(int argc, char** /*argv*/) {
    volatile int big = INT_MAX;
    const int r = big + argc;
    return r == 0 ? 1 : 0;
}
