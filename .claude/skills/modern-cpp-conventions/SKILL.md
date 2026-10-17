---
name: modern-cpp-conventions
description: This project's C++23 house conventions - ownership and RAII, Rule of Zero, value types via aggregates + default member initializers + designated initializers, classes with validating factories, views as borrowers, enum class, header rules, errors via std::expected, strong types, naming. Use whenever writing, reviewing or refactoring any C++ in this repo, including deciding how to shape a new type or function signature.
---

# Modern C++ conventions (C++23)

These rules are the house style for every `.hpp`/`.cpp` in the repo. They are enforced by:
- the compiler warnings and clang-tidy (`cpp-quality`);
- the reviewers: `quality-reviewer` for the rules, `memory-safety-reviewer` for lifetime.

Where clang-tidy can't see a rule, the reviewer enforces it.

This skill is the analog of "use attrs for classes" in a Python studio. The default shape for data is an **aggregate with default member initializers, built with designated initializers**. Only types with an invariant get a real class.

## Baseline
- **C++23, headers only.** No modules and no `import std`: CMake's `import std` is still behind an experimental gate, GCC needs `-fmodules`, and clangd's support is experimental. `CMAKE_CXX_SCAN_FOR_MODULES` is `OFF`.
- **Toolchain floor: GCC 14 / Clang 19 (with libstdc++ ≥ 14) / libc++ 18.** Newer is better; GCC 15+ / Clang 20+ is a judgement call, not a requirement. `/setup` enforces the floor with a **probe compiled with the preset's own flags** (`.claude/skills/platform-testing/probes/probe_features.cpp` via `pixi run cxx-probe`), so never hardcode an assumption about the installed compiler.
  - Clang < 19 with libstdc++ has **no `std::expected`**: libstdc++ gates `<expected>` on a concepts level that Clang reached only in 19.
  - libc++ 18 leaves `__cpp_lib_format` undefined. The probe accepts `__cpp_lib_print` as the proxy there; libc++ 19+ is recommended.
  - Features this code may use unguarded (the probe requires them): `std::expected`, `std::print`, `std::format`, `std::ranges::to`, `std::to_underlying`, `std::unreachable`.
  - Everything else is **report-only**: zip/enumerate/chunk/chunk_by/join_with/stride/cartesian_product, `std::generator`, `flat_map`, `mdspan`, `stacktrace`, `move_only_function`.
    - Check the probe output before using one.
    - libc++ lacks several of them (generator, stacktrace, chunk/slide/cartesian_product, move_only_function), so guard them with feature-test macros if macOS CI is enabled.
    - `std::stacktrace` needs `-lstdc++exp` on GCC, so keep it out of shipped code.
- GCC still labels C++23 support experimental, and its ABI may change between majors. **Don't expose C++23 standard-library types across a shared-library ABI.** A static, single-binary CLI is fine.
- No `{fmt}` and no `tl::expected` fallbacks. Every supported toolchain has the standard versions.

## The ten hard rules
1. **No owning raw pointers and no naked `new`/`delete`** (Core Guidelines R.3/R.11).
   - Wrap C resources **immediately** in `std::unique_ptr<T, Deleter>` or a small RAII type. `std::out_ptr` helps with C out-parameters.
   - ```cpp
     struct FileCloser { void operator()(std::FILE* f) const noexcept { static_cast<void>(std::fclose(f)); } };
     using FilePtr = std::unique_ptr<std::FILE, FileCloser>;
     ```
2. **`std::unique_ptr` is the default owner.** Use `std::shared_ptr` only for genuinely shared lifetime, and say whose. Non-owning parameters are `T&`, `const T&` or `T*` (nullable).
3. **Rule of Zero.**
   - If you declare any special member, handle all five.
   - A polymorphic base has a public virtual destructor or a protected non-virtual one, and deletes copy/move to prevent slicing (see `Command` in the scaffold).
   - Overriders say `override` (or `final`), never a repeated `virtual`.
4. **Every variable is initialized, and every data member has a default member initializer (DMI).**
   - Scalars, enums and pointers get a value: `int count = 1;`, `Command* selected = nullptr;`.
   - Class-type members get `{}` or a value: `std::string message{};`.
   - Why: with DMIs, partial designated initialization (`Error{.code = Errc::usage}`) is warning-free on both compilers. Without them, both warn: GCC with `-Wmissing-field-initializers` (in `-Wextra`), Clang with `-Wmissing-designated-field-initializers`. Under `-Werror` that is a build failure.
   - The two flag names differ: the Clang one is **Clang-only**, so warning lists are gated per compiler.
   - clang-tidy's `readability-redundant-member-init` flags the `{}` on class types such as `std::string`, which is exactly what this rule needs. The project `.clang-tidy` must therefore keep that check off, so **don't "fix" these `{}` away**.
   - Reference members can't have DMIs. Avoid them (see "Views are borrowers"). The scaffold's `Io` is the documented exception, with a `NOLINTNEXTLINE`.
5. **`enum class` only.** Convert with `std::to_underlying`. Switch over every enumerator, with no `default:`, so `-Wswitch` catches new ones. Follow the switch with a fallback `return` for out-of-range values.
6. **Headers:**
   - Start each with `#pragma once` (the house choice). Make each self-contained (it compiles on its own) and include what you use.
   - **Never write `using namespace` at namespace scope in a header.** Avoid it at namespace scope in `.cpp` files too. Inside a function it is fine.
   - Where a header only needs a declaration, use the forward-declaring header: `<iosfwd>` or `<nlohmann/json_fwd.hpp>`.
   - `<CLI/CLI.hpp>` is included only from `src/app/*.cpp`.
   - Include order is owned by clang-format (`IncludeBlocks: Regroup`), so don't hand-sort.
   - When `misc-include-cleaner` wants a header removed that the standard says you need, keep it with `// IWYU pragma: keep` plus a reason. Example: `<ostream>` for `std::print(std::ostream&, ...)`.
7. **Views are borrowers.**
   - `std::string_view` and `std::span` are for **parameters**.
   - As members or return values, they need the owner's lifetime documented, and a `<PROJ>_LIFETIMEBOUND` (below) on accessors.
   - Never bind a view to a temporary: `std::string_view v = make_string();` dangles.
   - Before calling a C-string API, convert to `std::string`, or use a `(data, size)` API such as `fwrite`. **Never pass `.data()` of a `string_view` where a NUL-terminated string is expected.**
8. **Lambdas that escape their scope never capture by reference** and never capture `this` implicitly via `[=]`.
   - "Escape" means stored in a callback, a thread or a `std::function`.
   - Capture by value. If you must borrow, capture an explicit pointer with a comment that states why it outlives the lambda: `[slot = &selected, self = command.get()] { *slot = self; }`.
9. **No C-style casts.** Use `static_cast`. Reach for `reinterpret_cast`/`const_cast` only with a comment.
   - Code is `-Wconversion -Wsign-conversion` clean: narrowing is explicit and checked.
   - Compare signed and unsigned with `std::cmp_less` and friends, and range-check with `std::in_range<T>(x)` before a `static_cast`.
10. **Errors and output follow `cli-design`:**
    - `Result<T>` (`std::expected<T, Error>`) for expected failures;
    - only `main`/`run` map errors to exit codes;
    - library code never prints to stdout or calls `std::exit`/`abort`.

    Other parts of this rule:
    - **No macros** for constants or functions. Use `inline constexpr` and functions; attribute shims are the exception.
    - Every `NOLINT` is `// NOLINTNEXTLINE(check-name): reason` on the line before, as `cpp-quality` requires.

## Guidance (the shapes to reach for)

**Value and config types: aggregate + DMIs + designated init + defaulted comparison.**
```cpp
struct ExportOptions {
    std::filesystem::path output{};
    Format format = Format::text;
    bool dry_run = false;
    int limit = 100;

    friend bool operator==(const ExportOptions&, const ExportOptions&) = default;
};

const ExportOptions opts{.output = path, .dry_run = true};  // the rest keep their defaults
```
Designated initializers must follow declaration order. Out-of-order designators are diagnosed (an error on GCC, `-Wreorder-init-list` on Clang, so an error under `-Werror`), never a silent swap. Use `<=>` only when ordering means something.

**A type with an invariant becomes a class with private members and a validating factory:**
```cpp
class Port {
public:
    [[nodiscard]] static Result<Port> parse(std::string_view text);  // the only way to make one
    [[nodiscard]] std::uint16_t value() const noexcept { return value_; }
    friend bool operator==(Port, Port) = default;

private:
    explicit Port(std::uint16_t v) noexcept : value_(v) {}
    std::uint16_t value_ = 0;
};
```

**Strong types** stop argument mix-ups:
- a one-member struct with an `explicit` constructor;
- or `enum class UserId : std::uint64_t {};` for opaque ids.

**Signatures:**
- Pass cheap types (≤ 2 words, trivially copyable) by value and others by `const&`. **Sink parameters** (stored or moved from) go **by value + `std::move`**.
- **Return structs, not out-parameters.** Use `std::optional<T>` for "maybe" and `Result<T>` for "or why not".
- `[[nodiscard]]` goes on factories, on `Result`-returning functions and on anything whose result is the point. The tidy check is off, so reviewers enforce it.
- Make moves `noexcept`, defaulted where possible, so containers move instead of copying.

**Algorithms:** prefer `std::ranges` algorithms and `std::ranges::to<std::vector>()` over hand-written index loops. Range-for is fine. Index loops mean `std::size_t` (or `std::ssize`) and no sign mixing.

**Lifetime annotation** on view-returning accessors. GCC ignores it, so it goes behind a shim:
```cpp
#if defined(__has_cpp_attribute)
#if __has_cpp_attribute(clang::lifetimebound)
#define <PROJ>_LIFETIMEBOUND [[clang::lifetimebound]]
#endif
#endif
#ifndef <PROJ>_LIFETIMEBOUND
#define <PROJ>_LIFETIMEBOUND
#endif

[[nodiscard]] std::string_view name() const <PROJ>_LIFETIMEBOUND { return name_; }
```

**Also allowed:**
- Deducing `this` is allowed, never required.
- `std::unreachable()` only after a check that makes the path provably impossible; otherwise return an error.
- `constexpr` where it is free.

**C++26** (contracts, reflection, `std::execution`): noted only. Adopting any of it takes an ADR and a probe.

## Naming (the scaffold's defaults; match existing code first)
| Kind | Style | Example |
|---|---|---|
| types, concepts | `PascalCase` | `GreetOptions`, `Command` |
| functions, variables, members, enumerators, namespaces | `snake_case` | `finish_stdout`, `exit_code::usage`, `ColorMode::never` |
| private data members | `snake_case_` (trailing underscore) | `count_`, `value_`. This also keeps `-Wshadow` quiet with constructor parameters |
| constants | `snake_case` `inline constexpr` | `max_name_length` |
| macros (attribute shims only) | `<PROJ>_UPPER_CASE` | `<PROJ>_LIFETIMEBOUND` |

## Compiler friction you will meet
- **`-Wshadow`:** trailing `_` on members avoids most of it. Don't silence it by renaming parameters to `p_x`.
- **`-Wuseless-cast` (GCC):** a `static_cast` to the same type is an error under `-Werror`. Don't add "defensive" casts.
- **`-Wold-style-cast` on libc macros** (signal constants and the like): if one trips, wrap the use in a small function and comment it. Never cast around it.
- **`misc-const-correctness`:** make locals `const` unless they are mutated. Don't fight it with `NOLINT` except where the language fixes a signature (`main`).

## When NOT to force patterns
- A plain free function needs no class, and a short-lived local tuple or `std::pair` is fine when naming fields buys nothing.
- Don't wrap every `int` in a strong type. Strong types are for ids, units and values that are easy to swap.
- Don't build a factory for a type without an invariant; an aggregate is simpler and clearer.
- Don't reach for `shared_ptr`, inheritance or templates to "future-proof". Add them when a second real use appears.
- Generated code and vendored third-party code follow their own style. They're excluded from format/lint.

## Sibling skills
- `cli-design`: errors-to-exit-codes, streams, CLI11 lifetime.
- `memory-safety`: the hazard table, UB and the sanitizers.
- `cpp-security`: untrusted input, checked size math, C APIs.
- `cpp-quality`: the warnings, the clang-tidy config, the NOLINT policy and the definition of done.
- `cpp-testing`: how to test what you write.
