// Positive control: a std::string_view into a heap-allocated, long (beyond the small-string buffer)
// std::string that is destroyed, then read through the view: heap-use-after-free. This is the
// dangling-view class the downloader's views/userdata pointers introduce. argc and reading through
// a volatile copy of the view's data pointer stop the optimizer from folding the bug away at -O1.
// Deliberately buggy; not linted (see .clang-tidy).
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

int main(int argc, char** /*argv*/) {
    const auto length = static_cast<std::size_t>(64 + argc);
    auto owner = std::make_unique<std::string>(length, 'x');
    const std::string_view view{*owner};
    owner.reset(); // the string and its heap buffer are freed; `view` now dangles
    const char* volatile data = view.data();
    const char c = data[argc];
    return c == 'x' ? 0 : 1;
}
