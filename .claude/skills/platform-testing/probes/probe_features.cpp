// probe_features.cpp — C++23 library floor probe (/setup step 4, platform-testing P2).
//
// Run it only through the probe runner, so it is compiled with the preset's own flags in the real
// env:
//   nice pixi run cxx-probe .claude/skills/platform-testing/probes/probe_features.cpp
//   nice pixi run -e gcc cxx-probe .claude/skills/platform-testing/probes/probe_features.cpp
//
// Contract:
//   - A REQUIRED feature below its floor is a compile-time #error, so cxx-probe exits 125.
//     That means the toolchain is below the studio's C++23 floor. Never edit this file to make it
//     pass.
//   - Otherwise it prints one line per macro and exits 0:
//       probe-features toolchain <field> <value>
//       probe-features required <macro> <value>
//       probe-features report <macro> <value|absent>
//       probe-features-ok
//     "report" lines are informational only. Never gate on them; feature-test-guard any use
//     instead.
//
// Thresholds are the confirmed floors (fact-check B1/B2), not the newest values. Do not raise them.
// For example, __cpp_lib_print stays at 202207L even where the library reports 202406L.

#include <cstdio>
#include <version>

// ---- required -------------------------------------------------------------------------------

#if !defined(__cpp_lib_expected) || __cpp_lib_expected < 202211L
#error                                                                                             \
    "probe-features: required __cpp_lib_expected >= 202211L (std::expected with and_then/transform/or_else)"
#endif

#if !defined(__cpp_lib_print) || __cpp_lib_print < 202207L
#error "probe-features: required __cpp_lib_print >= 202207L (std::print / std::println)"
#endif

// libc++ 18 ships std::format but leaves __cpp_lib_format undefined; libc++ 19 is the first to
// define it. On libc++ 18 only, the __cpp_lib_print check above stands in for format. libc++ 19+ is
// recommended.
#if defined(_LIBCPP_VERSION) && _LIBCPP_VERSION >= 180000 && _LIBCPP_VERSION < 190000
#define PROBE_FORMAT_VIA_PRINT_PROXY 1
#elif !defined(__cpp_lib_format) || __cpp_lib_format < 202106L
#error                                                                                             \
    "probe-features: required __cpp_lib_format >= 202106L (compile-time checked std::format strings)"
#endif

#if !defined(__cpp_lib_ranges_to_container) || __cpp_lib_ranges_to_container < 202202L
#error "probe-features: required __cpp_lib_ranges_to_container >= 202202L (std::ranges::to)"
#endif

#if !defined(__cpp_lib_to_underlying)
#error "probe-features: required __cpp_lib_to_underlying (std::to_underlying)"
#endif

#if !defined(__cpp_lib_unreachable)
#error "probe-features: required __cpp_lib_unreachable (std::unreachable)"
#endif

namespace {

void line(const char* kind, const char* name, long value) {
    if (value == 0) {
        std::printf("probe-features %s %s absent\n", kind, name);
    } else {
        std::printf("probe-features %s %s %ld\n", kind, name, value);
    }
}

void toolchain() {
#if defined(__clang__)
    std::printf("probe-features toolchain compiler clang %d.%d.%d\n", __clang_major__,
                __clang_minor__, __clang_patchlevel__);
#elif defined(__GNUC__)
    std::printf("probe-features toolchain compiler gcc %d.%d.%d\n", __GNUC__, __GNUC_MINOR__,
                __GNUC_PATCHLEVEL__);
#else
    std::printf("probe-features toolchain compiler unknown 0\n");
#endif
#if defined(_LIBCPP_VERSION)
    std::printf("probe-features toolchain stdlib libc++ %ld\n", static_cast<long>(_LIBCPP_VERSION));
#elif defined(_GLIBCXX_RELEASE)
    std::printf("probe-features toolchain stdlib libstdc++ %ld\n",
                static_cast<long>(_GLIBCXX_RELEASE));
#else
    std::printf("probe-features toolchain stdlib unknown 0\n");
#endif
    std::printf("probe-features toolchain cplusplus %ld\n", static_cast<long>(__cplusplus));
#if defined(PROBE_FORMAT_VIA_PRINT_PROXY)
    std::printf("probe-features toolchain format-check libc++18-print-proxy\n");
#else
    std::printf("probe-features toolchain format-check macro\n");
#endif
}

void required() {
    line("required", "__cpp_lib_expected", static_cast<long>(__cpp_lib_expected));
    line("required", "__cpp_lib_print", static_cast<long>(__cpp_lib_print));
#if defined(__cpp_lib_format)
    line("required", "__cpp_lib_format", static_cast<long>(__cpp_lib_format));
#else
    line("required", "__cpp_lib_format", 0L); // only reachable on the libc++ 18 proxy path
#endif
    line("required", "__cpp_lib_ranges_to_container",
         static_cast<long>(__cpp_lib_ranges_to_container));
    line("required", "__cpp_lib_to_underlying", static_cast<long>(__cpp_lib_to_underlying));
    line("required", "__cpp_lib_unreachable", static_cast<long>(__cpp_lib_unreachable));
}

void report_only() {
#if defined(__cpp_lib_ranges_zip)
    line("report", "__cpp_lib_ranges_zip", static_cast<long>(__cpp_lib_ranges_zip));
#else
    line("report", "__cpp_lib_ranges_zip", 0L);
#endif
#if defined(__cpp_lib_ranges_enumerate)
    line("report", "__cpp_lib_ranges_enumerate", static_cast<long>(__cpp_lib_ranges_enumerate));
#else
    line("report", "__cpp_lib_ranges_enumerate", 0L);
#endif
#if defined(__cpp_lib_ranges_chunk)
    line("report", "__cpp_lib_ranges_chunk", static_cast<long>(__cpp_lib_ranges_chunk));
#else
    line("report", "__cpp_lib_ranges_chunk", 0L);
#endif
#if defined(__cpp_lib_ranges_chunk_by)
    line("report", "__cpp_lib_ranges_chunk_by", static_cast<long>(__cpp_lib_ranges_chunk_by));
#else
    line("report", "__cpp_lib_ranges_chunk_by", 0L);
#endif
#if defined(__cpp_lib_ranges_join_with)
    line("report", "__cpp_lib_ranges_join_with", static_cast<long>(__cpp_lib_ranges_join_with));
#else
    line("report", "__cpp_lib_ranges_join_with", 0L);
#endif
#if defined(__cpp_lib_ranges_stride)
    line("report", "__cpp_lib_ranges_stride", static_cast<long>(__cpp_lib_ranges_stride));
#else
    line("report", "__cpp_lib_ranges_stride", 0L);
#endif
#if defined(__cpp_lib_ranges_cartesian_product)
    line("report", "__cpp_lib_ranges_cartesian_product",
         static_cast<long>(__cpp_lib_ranges_cartesian_product));
#else
    line("report", "__cpp_lib_ranges_cartesian_product", 0L);
#endif
#if defined(__cpp_lib_generator)
    line("report", "__cpp_lib_generator", static_cast<long>(__cpp_lib_generator));
#else
    line("report", "__cpp_lib_generator", 0L);
#endif
#if defined(__cpp_lib_flat_map)
    line("report", "__cpp_lib_flat_map", static_cast<long>(__cpp_lib_flat_map));
#else
    line("report", "__cpp_lib_flat_map", 0L);
#endif
#if defined(__cpp_lib_mdspan)
    line("report", "__cpp_lib_mdspan", static_cast<long>(__cpp_lib_mdspan));
#else
    line("report", "__cpp_lib_mdspan", 0L);
#endif
#if defined(__cpp_lib_stacktrace)
    line("report", "__cpp_lib_stacktrace", static_cast<long>(__cpp_lib_stacktrace));
#else
    line("report", "__cpp_lib_stacktrace", 0L);
#endif
#if defined(__cpp_lib_move_only_function)
    line("report", "__cpp_lib_move_only_function", static_cast<long>(__cpp_lib_move_only_function));
#else
    line("report", "__cpp_lib_move_only_function", 0L);
#endif
#if defined(__cpp_lib_modules)
    line("report", "__cpp_lib_modules", static_cast<long>(__cpp_lib_modules));
#else
    line("report", "__cpp_lib_modules", 0L);
#endif
}

} // namespace

int main() {
    toolchain();
    required();
    report_only();
    std::printf("probe-features-ok\n");
    return 0;
}
