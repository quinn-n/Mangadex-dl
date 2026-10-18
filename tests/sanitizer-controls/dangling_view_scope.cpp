// Positive control: a std::string_view into a block-scoped stack buffer, read after the block has
// ended: stack-use-after-scope. Built at -O0 (see CMakeLists.txt) so the buffer keeps its own
// stack slot and its scope poisoning is not merged away. Deliberately buggy; not linted (see
// .clang-tidy).
#include <array>
#include <cstddef>
#include <string_view>

int main(int argc, char** /*argv*/) {
    std::string_view view{};
    {
        std::array<char, 32> buffer{};
        buffer.fill('x');
        view = std::string_view{buffer.data(), buffer.size()};
    } // `buffer` goes out of scope here; `view` now dangles
    const volatile char c = view[static_cast<std::size_t>(argc)];
    return c == 'x' ? 0 : 1;
}
