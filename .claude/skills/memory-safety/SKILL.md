---
name: memory-safety
description: The dominant risk of this C++ CLI - ownership, lifetime and undefined behaviour. Holds the 19-row hazard table, the sanitized dev preset (option C and its eight conditions), the exact sanitizer flags and runtime options (exitcode=86), the suppression policy, std-lib and OpenSSF release hardening, high-entropy-ASLR and container states, debugging recipes (gdb, TSan, valgrind) and the evidence standard. Use for any C++ change, any sanitizer finding or report, suppression or hardening question, or when reviewing for memory safety.
---

# Memory safety, sanitizers and hardening

Memory and lifetime errors are this studio's dominant failure mode. They surface as crashes, silent corruption or UB that only the optimizer exposes, not as compile errors. Three layers catch them:
1. **Static rules and warnings.** See `modern-cpp-conventions`, and the hazard table below.
2. **The sanitized dev preset** (ASan+UBSan on every `check`).
3. **The separately built, hardened `-O2 -Werror` release.**

`memory-safety-reviewer` gates all of it with **evidence**, citing hazard rows by number.

**Sanitizers only see executed paths.** A clean run is evidence about the paths that ran, never proof of memory safety. Never write "memory-safe"; write "no ASan/UBSan finding on the exercised paths (tests …)".

## Option C: the dev preset is sanitized, under eight hard rules
The `dev` preset (`@PROJ_UPPER@_SANITIZE="address;undefined"`, `-O1 -g`) is what `pixi run test`/`check` build. It is sound only with all eight conditions:

1. **Reserved exit code 86, everywhere.** Sanitizers default to exit 1, which is also the app's "failure" code. An E2E case expecting 1 would pass on a heap overflow.
   - So `exitcode=86` goes in **ASAN_OPTIONS, UBSAN_OPTIONS and LSAN_OPTIONS** (and TSAN_OPTIONS). Clang has one shared runtime exit code; GCC has separate runtimes, each governed by its own variable.
   - 86 is `@PROJ@::exit_code::sanitizer_reserved` (dev builds only; the app never returns it).
   - The robust half is the E2E driver: it fails any case whose stderr matches
     `ERROR: (Address|Leak|Memory)Sanitizer|WARNING: ThreadSanitizer|runtime error:|AddressSanitizer:DEADLYSIGNAL`
     **whatever the exit code**. GCC's UBSan prints no `SUMMARY:` line, so the regex keys on `runtime error:`.
2. **GCC + sanitizers + `-Werror` produces false positives** (GCC PR 83382, PR 108306). In **sanitized GCC builds only**, demote exactly these six, and nothing else:
   `-Wno-error=maybe-uninitialized -Wno-error=array-bounds -Wno-error=stringop-overflow -Wno-error=stringop-overread -Wno-error=stringop-truncation -Wno-error=null-dereference`.
   - They are target-scoped warnings, applied in `cmake/ProjectWarnings.cmake` (cpp-quality scaffold).
   - Clang sanitized builds keep full `-Werror`, and so do release builds.
   - This is a starting list for the current GCC major. Add an entry only with a GCC bugzilla link.
3. **A sanitized install is never shipped.**
   - `scripts/package.sh` refuses a build dir whose cache has `@PROJ_UPPER@_SANITIZE` set. As a backstop, `Packaging.cmake` makes `cpack` itself fail with a FATAL at package time (a refusing `CPACK_PROJECT_CONFIG_FILE`); the sanitized dev preset still configures normally.
   - The release preset gets its own install + E2E (`release-test`, in CI and `/harden`).
4. **Explicit `-D_GLIBCXX_ASSERTIONS`.** libstdc++ enables its assertions by itself only at `-O0`; the dev preset is `-O1`.
5. **`-U_FORTIFY_SOURCE` in sanitized builds.** Fortify is incompatible with ASan, and toolchains or distros may predefine it.
6. **The release build is compiled with `-Werror` inside `check`.** UB and warnings that only appear at `-O2` (`-Wmaybe-uninitialized`, `-Warray-bounds`, `-Wstringop-*`, `-Wnull-dereference`, `-Wuse-after-free`, `-Wdangling-pointer`, `-Wfree-nonheap-object`, `-Wrestrict`, Clang `-Wfortify-source`) are invisible to the `-O1` sanitized run.
7. **Parallel caps cover ctest `-j` and the link pool**, not just build jobs. ASan reserves terabytes of virtual space and uses much more real memory. Per-worktree `-local` presets cap both; `CMakeLists.txt` sets a Ninja link pool of 2 when sanitizing (`build-workflow`).
8. **Probe in the real context.** Sanitizer usability is decided by the `platform-testing` probe in the same pixi env, compiler, flags and `*_OPTIONS` as `check` (`nice pixi run cxx-probe …`). A probe from another shell proves nothing.

## Sanitizer flags (single source: `cmake/Sanitizers.cmake`)
Applied **project-wide** (`add_compile_options`/`add_link_options`, before any target), so source-built dependencies are instrumented too. Warnings stay target-scoped.

| Flag | Why |
|---|---|
| `-fsanitize=address,undefined` | the pair; LSan rides on ASan on Linux. Required: configure FATALs if it can't compile **and link** |
| `-fsanitize=float-cast-overflow` | in Clang's `undefined` group but **not GCC's**, so it must be explicit (probed) |
| `-fsanitize=vptr` | Clang's `undefined` no longer includes it; GCC's does. Explicit on both (probed; needs RTTI) |
| `-fno-sanitize-recover=all` | any finding terminates with exit 86 |
| `-fno-omit-frame-pointer -fno-optimize-sibling-calls` | usable stacks |
| `-fno-sanitize-merge` | **Clang only** (GCC rejects it); distinct check sites, accurate report lines. Guarded by compiler ID and probed |
| `-U_FORTIFY_SOURCE` (an *option*), `-D_GLIBCXX_ASSERTIONS`, `-D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_DEBUG` | conditions 4/5; each std library ignores the other's macro |
| `-O1 -g` | owned by the preset (`CMAKE_CXX_FLAGS_DEBUG`). `Sanitizers.cmake` never adds a second `-O` level; the last `-O` wins |

- Every optional flag is probed with `check_cxx_compiler_flag` **with `CMAKE_REQUIRED_LINK_OPTIONS`**. A compile-only probe passes even when the runtime is missing.
- Never hardcode "compiler X supports Y". UBSan group membership drifts across releases.
- **Opt-in extended UB pass** (`/harden`, triaged, never in `check`): `float-divide-by-zero`, `implicit-conversion`, `unsigned-integer-overflow`, `local-bounds` (Clang). Run it with `-fsanitize-recover=` for these non-UB checks, because they are noisy by design (hashing, bit tricks).
- **TSan** has its own optional `tsan` preset (`@PROJ_UPPER@_SANITIZE="thread"`), **only if the tool uses threads**.
  - It is never combined with ASan.
  - Its control is `sanitizer-control.data-race`.
- **MSan** is impractical: it needs a fully instrumented std library (terminal reason T2).
- **HWASan** doesn't apply on x86_64.

## Runtime options (exact; one source, three outputs)
```
ASAN_OPTIONS=halt_on_error=1:abort_on_error=0:exitcode=86:detect_leaks=1:detect_stack_use_after_return=1:check_initialization_order=1:strict_init_order=1:strict_string_checks=1:handle_segv=2:print_summary=1:suppressions=<root>/tools/sanitizers/asan.supp
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1:exitcode=86:suppressions=<root>/tools/sanitizers/ubsan.supp
LSAN_OPTIONS=exitcode=86:print_suppressions=0:suppressions=<root>/tools/sanitizers/lsan.supp
TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1:exitcode=86          (tsan preset only)
```
`<root>` is the absolute source dir. `Sanitizers.cmake` FATALs if that path contains `:` or whitespace, because the runtime option parser splits on them.

`Sanitizers.cmake` emits the strings to:
1. **`@PROJ_UPPER@_SANITIZER_TEST_ENV`** → the CTest `ENVIRONMENT` of every plain `add_test()` (E2E cases, controls). The E2E driver keeps `*SAN_OPTIONS` in its hermetic env.
2. **`<build>/sanitizer-env.txt`** (`KEY=VALUE` lines) → `cxx-probe.sh`, and humans reproducing by hand.
3. **`<build>/generated/sanitizer_options.cpp`** (from `src/app/sanitizer_options.cpp.in`): `__asan_default_options`, `__ubsan_default_options`, `__lsan_default_options` and `__tsan_default_options`. No attributes are needed on either compiler.
   - `@PROJ@_add_sanitizer_options(<target>)` links it into the app, the Catch2 test binaries and the controls.
   - An installed sanitized binary run by hand still exits 86 with stacks.
   - Environment keys override defaults key by key.
   - The `sanitizer-control.*-defaults` controls (heap-overflow, leak, signed-overflow) prove the compiled-in path for each runtime: they run with every `*SAN_OPTIONS` unset and must still exit 86.

Never set any `*SAN_OPTIONS` containing `exitcode=0` or `halt_on_error=0` (the `guard-bash` hook blocks it).

`handle_segv=2` stops Catch2's own SIGSEGV handler from replacing ASan's, which gives the accurate `ERROR: AddressSanitizer: SEGV` report (`sanitizer-control.catch2-segv`).

## Suppressions (`tools/sanitizers/{asan,ubsan,lsan}.supp`)
- **The files must exist**, even if they hold only comments. A missing suppressions file is a fatal runtime error. It is raised lazily, and the message may say `AddressSanitizer:` even for a UBSan/LSan file. Paths are always absolute.
- Every entry carries a reason and an issue link on the line above it. **A new entry is a review finding** for `memory-safety-reviewer`.
- A suppression is a last resort for code you do not control (a prebuilt dependency). A finding in our code is a bug: fix it.
- **UBSan runtime suppressions are inert under `-fno-sanitize-recover=all`** (they apply only to recoverable checks). Fix the code. If the behaviour is intended and defined by contract, put a narrowly scoped `__attribute__((no_sanitize("<check>")))` on that one function, with a comment that gives the reason and an issue link.
- LSan suppressions do work: `leak:<substring of function/file/module>`.

## Standard-library hardening
| | Dev (sanitized) | Release |
|---|---|---|
| libstdc++ | `-D_GLIBCXX_ASSERTIONS` (explicit; `_GLIBCXX_NO_ASSERTIONS` opts out) | `-D_GLIBCXX_ASSERTIONS` |
| libc++ (only if `-stdlib=libc++`) | `_LIBCPP_HARDENING_MODE_DEBUG` | `_LIBCPP_HARDENING_MODE_FAST` |

- **Never `_GLIBCXX_DEBUG`.** It changes container layout (ABI) against the prebuilt Catch2 and other deps.
- `_GLIBCXX_SANITIZE_VECTOR` (container-overflow annotations) is opt-in, and only when **every** dependency is built from source with the same flags. Otherwise mixed instrumentation produces false positives.

## Release hardening (`cmake/Hardening.cmake`, OpenSSF Compiler Options Hardening Guide)
ON in the `release` preset (`@PROJ_UPPER@_HARDENING`). **FATAL if combined with sanitizers.**

Every flag is probed (`check_cxx_compiler_flag`, `check_linker_flag`) and applied project-wide:
- **Fortify:** `-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3` as compile **options**, in that order. CMake emits definitions *before* options, so a `-U` among the options would undo a `-D` from `add_compile_definitions`. Skipped for Debug, because fortify needs optimization.
- `_GLIBCXX_ASSERTIONS`, `_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_FAST`.
- `-fstrict-flex-arrays=3 -fstack-clash-protection -fstack-protector-strong -ftrivial-auto-var-init=zero`.
  - The last one is in OpenSSF's "production code" row; it is the studio default. It does not suppress `-Wuninitialized`, but it hides uninitialized reads from valgrind.
- x86_64: `-fcf-protection=full -fzero-call-used-regs=used-gpr`. AArch64: `-mbranch-protection=standard`. GCC: `-fzero-init-padding-bits=all`.
- Link (`LINKER:` form): `-z,nodlopen -z,noexecstack -z,relro -z,now --as-needed --no-copy-dt-needed-entries`.
- **PIE** via `CMAKE_POSITION_INDEPENDENT_CODE` + `check_pie_supported()`. Never hand-written `-fPIE -pie`.
- **Opt-in `@PROJ_UPPER@_HARDENING_EXTRA`:** `-fno-delete-null-pointer-checks -fno-strict-overflow -fno-strict-aliasing`.
  - This is a genuine trade-off. They make some UB benign and cost some optimization; UBSan in dev is what finds the underlying bugs.
- **`-fhardened` (GCC 14+) is not a substitute.** It lacks strict-flex-arrays, noexecstack and nodlopen, and it has no Clang equivalent. Mention it, don't use it.
- Post-build checks (`release-check.sh`): `Type: DYN`, `GNU_STACK` RW (not RWE), `GNU_RELRO`, `BIND_NOW` / `FLAGS_1 … NOW PIE`.
  - These readelf markers are **binutils conventions, not OpenSSF text**. Confirm them against this project's own known-good build before trusting the gate.
- A confirmed optimizer false positive in release gets a scoped `#pragma GCC diagnostic` with the reason and a bugzilla link, or a compiler-gated `-Wno-error=<flag>` in `ProjectWarnings.cmake`. **Never a blanket `-Wno-error`.**

## High-entropy ASLR, containers and other "present but unusable" states
- `vm.mmap_rnd_bits` is root-only. The probe cannot read it, so report `unreadable (root-only)` and infer the state from symptoms.
- The research reports ASan fixed for 32-bit ASLR from LLVM 17 / GCC 14, and TSan from LLVM 18.1 / GCC 15. **The probe decides, not the version number.**
- Markers of a present-but-unusable runtime (quote the exact text in reports):
  - `Shadow memory range interleaves with an existing memory mapping`
  - repeated `AddressSanitizer:DEADLYSIGNAL` on the clean probe
  - `FATAL: ThreadSanitizer: unexpected memory mapping`
  - `memory layout is incompatible, even though ASLR is disabled`
  - `CHECK failed: … personality(…ADDR_NO_RANDOMIZE)` (seccomp-blocked)
  - `LeakSanitizer has encountered a fatal error` / `does not work under ptrace`
  - `ERROR: AddressSanitizer: out of memory`
  - `ASan runtime does not come first in initial library list` (an `LD_PRELOAD` or a preloaded library interferes with the runtime)
- **Diagnosis line:** `setarch x86_64 -R .platform-runs/cxx-probe/<env>/probe_clean` (the binary `cxx-probe` left behind). If it passes → high-entropy ASLR; if it errors with a personality/permission error → container or seccomp.
- **Workarounds are reported, never applied:** `sudo sysctl -w vm.mmap_rnd_bits=28` (the user runs it), `setarch -R`, a toolchain upgrade, `detect_leaks=0` narrowing, container flags.
- GitHub-hosted ubuntu images already bake `mmap_rnd_bits=28`. CI's `sudo sysctl … || true` step is defensive, for self-hosted runners and containers.

## Debugging recipes
- **Reproduce first:** the exact installed-binary command (`build/default/dev/e2e-prefix/bin/@TOOL@ …`), or `nice pixi run test -R '<name>'` plus the `--rng-seed` from the log. The compiled-in defaults give stacks and exit 86 even outside CTest.
- **gdb:** `ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 gdb --args <bin> …`. LSan does not work under ptrace, and `abort_on_error=1` stops gdb at the report. This command prompts; that's acceptable.
- **Unsymbolized frames:** `llvm-symbolizer` comes from the env's `llvm-tools`. Check `command -v llvm-symbolizer` under `pixi run`; `ASAN_SYMBOLIZER_PATH` is the override. GCC's runtime falls back to its own symbolization.
- **TSan** (threaded tools only): the `tsan` preset and `sanitizer-control.data-race`. Use `std::jthread`, join before exit, and keep TSan and ASan in separate builds.
- **valgrind** (optional; probe `valgrind --version`): only on **unsanitized** builds without `-ftrivial-auto-var-init`. The `cov` build qualifies.
  - Use it mainly for uninitialized reads (hazard #8), which ASan/UBSan miss.
  - Run with `--error-exitcode=<n>` so it gates. It is never part of `check`.

## Hazard table (cite rows by number)
Rows 1–17 come from the sanitizer research, with fact-check resolutions applied. Rows 18–19 are this stack's CLI11/`Command` specifics.

| # | Hazard | Runtime detection | Missed when | Static aids / rule |
|---|---|---|---|---|
| 1 | `string_view`/`span`/reference to a temporary or a destroyed owner | ASan heap-use-after-free; stack-use-after-scope (on by default in Clang and GCC ASan) | storage reused before the read; dangling into a still-valid buffer | Clang `-Wdangling` group (on by default: `-Wdangling-gsl`, `-Wdangling-field`, `-Wdangling-assignment`, `-Wdangling-capture`, `-Wreturn-stack-address`); `[[clang::lifetimebound]]` on accessors returning views/refs into params or `*this`; Clang `-Wlifetime-safety-permissive` (intra-procedural, doesn't track moves; opt-in `@PROJ_UPPER@_LIFETIME_SAFETY`, probed); GCC `-Wdangling-reference` (C++ only; in `-Wextra`), `-Wdangling-pointer`; clang-tidy `bugprone-dangling-handle`, `bugprone-return-const-ref-from-parameter`. Rule: never store a view whose owner's lifetime isn't obviously longer; no `string_view` members without a documented owner |
| 2 | Lambda `[&]` capture escaping (stored callback, thread, deferred task) | ASan stack-use-after-return (`detect_stack_use_after_return` on by default on Linux) | callback never invoked in tests | `-Wdangling-capture`, `[[clang::lifetime_capture_by]]`. Rule: `[&]` only for immediately invoked, non-escaping lambdas |
| 3 | Iterator/reference invalidation (`push_back`/`insert`/`erase` while holding iterators or `&v[i]`) | ASan use-after-free **only if reallocation happens** | capacity was sufficient (no realloc) | `_GLIBCXX_DEBUG` (ABI caveat, not used here); libc++ debug mode; Clang `-Wlifetime-safety-invalidation` (**experimental**, may produce false positives; off). Rule: re-acquire after mutation; erase-remove / `std::erase_if` |
| 4 | Out-of-bounds index | ASan (heap/stack/global); `_GLIBCXX_ASSERTIONS` (`operator[]`, `front()`, …); UBSan `bounds` for statically sized arrays | intra-object overflow (array member inside a struct); `vector[i]` with `size ≤ i < capacity` without container annotations | `.at()` at trust boundaries; `std::span`; `-fstrict-flex-arrays=3` in release |
| 5 | Signed overflow, bad shifts, int divide-by-zero | UBSan (`signed-integer-overflow`, `shift`, `integer-divide-by-zero`) | unexecuted paths | checked arithmetic (`__builtin_*_overflow`) for size math and allocation sizes |
| 6 | Unsigned wrap in size math (`n * sizeof(T)`, `a - b` underflow) | not UB, so default UBSan misses it; opt-in `unsigned-integer-overflow` (noisy) | default | `-Wconversion -Wsign-conversion` (-Werror); checked helpers; review all allocation-size expressions |
| 7 | Narrowing / implicit conversions | — (opt-in Clang `implicit-conversion`) | — | brace-init; `-Wconversion`; `gsl::narrow`-style checked casts |
| 8 | Uninitialized reads | **ASan and UBSan miss these** (UBSan only `bool`/`enum` load checks); MSan impractical | almost always | `-Wuninitialized`/`-Wmaybe-uninitialized` in the **release -O2 -Werror build**; clang-tidy `cppcoreguidelines-init-variables`; valgrind Memcheck (optional). Rule: initialize at declaration; every member has a default member initializer |
| 9 | Static-initialization-order fiasco | ASan `check_initialization_order=1:strict_init_order=1` (Linux only) | init path not exercised | `constinit`; function-local statics; avoid non-trivial globals across TUs |
| 10 | Lifetime across `std::exit` / `quick_exit` | ASan may catch use-after-free in static destructors | — | `std::exit` doesn't unwind (automatic objects never destroyed, buffers maybe not flushed by RAII). Rule (also the CLI contract): return from `main`; catch at `main`; flush and check stdout before returning |
| 11 | Data races / joinable `std::thread` destroyed (→ `std::terminate`) | **TSan only** (separate preset) | single-threaded tests | `std::jthread`; atomics/mutexes; join before exit |
| 12 | Ownership: raw `new`/`delete`, double free, `new[]`/`delete` mismatch, leaks | ASan double-free, `alloc_dealloc_mismatch`, `new_delete_type_mismatch`; LSan at exit | leaks still reachable from globals | Rule of Zero; `unique_ptr` default; no owning raw pointers |
| 13 | Use-after-move | sanitizers miss it (valid-but-unspecified state) | always | clang-tidy `bugprone-use-after-move` |
| 14 | Type punning / strict aliasing | missed (Clang `-fsanitize=type` exists but is out of scope) | — | `std::bit_cast`, `memcpy` |
| 15 | Misaligned access, null deref | UBSan `alignment`, `null`; ASan SEGV | — | — |
| 16 | Non-NUL-terminated buffers to C APIs (`sv.data()` into `open()`/`getenv()`) | ASan + `strict_string_checks=1` | — | Rule: convert views to `std::string` before any C-string API |
| 17 | Format strings | — | — | `std::format`/`std::print` (compile-time checked; never `vformat`/`runtime_format` with user data); for printf-family use `-Wformat=2 -Werror=format-security` |
| 18 | CLI11 bound-variable lifetime: variables bound by `add_option`/`add_flag` must outlive `parse()` and every callback that reads them | ASan stack-use-after-return / use-after-scope, **only if** the dangling path runs | the binding outlives its storage only on an untested subcommand | Rule: bind into members of the `Command` object owned by `run()`'s scope; never bind locals of a `register_<sub>()` function; callbacks only *select* a `Command*`, and work runs in `dispatch` after `parse` (`cli-design`) |
| 19 | `Command`/Invocation object lifetime and escaping callbacks: a `Command*` selected in a callback, or a lambda stored in the app, outliving its owner | ASan heap-use-after-free if executed | ownership moved or the `unique_ptr` dropped before dispatch; callback never invoked in tests | Rule: `run()` owns every `std::unique_ptr<Command>` for the whole parse + dispatch; no `[&]` capture of locals in stored CLI11 callbacks (hazard #2); a test drives each subcommand through `run()` (tier 2) and E2E |

## Evidence standard (what `memory-safety-reviewer` and every fixer cite)
- **Which run:** env/compiler + version, preset (`dev`/`tsan`/`release`), and that the `sanitizer-control` tests **fired** (otherwise "sanitizer-clean" is not licensed).
- **Which tests exercised the changed paths:** test names, or `cov` when enabled. "Changed paths exercised: yes / partial / no".
- An **unexercised risky path is a Must-fix: "add a test that exercises it"**. A clean run that never reached the change proves nothing about it.
- "Not provable here" is stated explicitly: uninitialized reads (→ release `-Werror`, valgrind if present), races (→ `tsan` if threaded), MSan-class bugs (T2).
- A sanitizer finding is a **bug**. Never suppress, `NOLINT`, weaken a test or lower an option to get green.

## When not to
- Don't add sanitizer flags per target, or combine sanitizers with hardening.
- Don't hand-tune `*_OPTIONS` in a test, a script or the shell. Change `Sanitizers.cmake`, the one source.
- Don't chase valgrind or MSan when a sanitizer control or a release `-Werror` warning already covers the hazard.

## Sibling skills
- `modern-cpp-conventions`: ownership and view rules.
- `cpp-testing`: controls, the E2E marker check and goldens.
- `platform-testing`: the usability probe, tiers and reasons.
- `cpp-quality`: warnings policy and `ProjectWarnings.cmake`.
- `build-workflow`: presets, packaging refusal and the link pool.
- `cpp-security`: format strings, size math and untrusted input.
