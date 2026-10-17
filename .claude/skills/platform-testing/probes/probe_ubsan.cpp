// probe_ubsan.cpp — positive control: signed overflow
#include <climits>
int main(int argc, char**) {
    volatile int big = INT_MAX;
    int r = big + argc;
    return r == 0 ? 1 : 0;
}
