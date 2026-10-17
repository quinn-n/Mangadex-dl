// probe_clean.cpp — must exit 0, stdout "probe-clean-ok 3 42", stderr free of sanitizer markers
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
int main() {
    std::vector<std::string> v{"alpha", "a-string-long-enough-to-defeat-sso"};
    v.push_back("gamma");
    auto p = std::make_unique<int>(41);
    *p += 1;
    std::printf("probe-clean-ok %zu %d\n", v.size(), *p);
    return 0;
}
