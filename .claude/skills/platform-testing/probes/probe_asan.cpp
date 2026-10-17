// probe_asan.cpp — positive control: 1-byte heap overflow (argc==1 -> p[8] on an 8-byte block)
#include <cstdlib>
int main(int argc, char**) {
    char* volatile p = static_cast<char*>(std::malloc(8));
    p[argc + 7] = 'x';
    std::free(p);
    return 0;
}
