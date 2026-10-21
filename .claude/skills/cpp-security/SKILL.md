---
name: cpp-security
description: Security rules for this C/C++ CLI - process spawning, file descriptors, temp files, TOCTOU and symlinks, format strings, integer/size math, untrusted input (argv, env, files, JSON), secrets, supply chain - plus the `audit` task (syft + grype over the pixi env, with a coverage statement). Use when touching any of those areas, adding a dependency, or reviewing a change for security.
---

# C++ CLI security

A CLI runs with the user's privileges, but **its inputs aren't the user's intent**. Treat all of these as untrusted:
- argv, environment variables, file contents, file *names*;
- anything another process can change between your check and your use.

`security-reviewer` walks this checklist on every change that touches these areas, and always in `/harden`. Memory-safety bugs (overflows, use-after-free, UB) are the other half of C++ security; see `memory-safety`.

## Quick scan for reviewers
Start with a grep over the diff or the tree, then read every hit in context:
```bash
git grep -nE '\b(system|popen|tmpnam|tempnam|mktemp|gets|strcpy|strcat|sprintf|vsprintf)\s*\(|runtime_format|vformat|getenv|setenv|putenv|reinterpret_cast|O_CREAT|fopen\(|posix_spawnp|execvp|remove_all' -- src include
```
A hit isn't automatically a finding. A hit without one of the safeguards below is.

## 1. Process spawning
- **Never `system()` or `popen()` with any interpolated input.** That is shell injection (CERT ENV33-C). Even a constant string runs through `/bin/sh` and the caller's `PATH`.
- Use **`posix_spawn` or `fork` + `execv`** with an **argv vector**: one element per argument, no quoting, no shell.
  - Give an **absolute path** for anything security-relevant. `posix_spawnp`/`execvp` search the caller-controlled `PATH`.
  - Pass an **explicit, minimal `envp`**. Don't hand the child your whole environment.
  - **Check every return.** `posix_spawn` returns an error number; it doesn't set errno.
  - **Reap every child** with `waitpid` (retrying on `EINTR`), and interpret the status with `WIFEXITED` / `WEXITSTATUS` / `WIFSIGNALED`.
- Arguments that come from users and could look like options get a `--` separator, if the child supports one.
```cpp
// Storage outlives the call; argv/envp are NUL-terminated arrays of char*.
std::vector<std::string> args{"/usr/bin/git", "-C", repo.string(), "rev-parse", "HEAD"};
std::vector<char*> argv;
for (auto& a : args) { argv.push_back(a.data()); }
argv.push_back(nullptr);
std::string path_env = "PATH=/usr/bin:/bin";
std::array<char*, 2> envp{path_env.data(), nullptr};

pid_t pid = 0;  // <spawn.h>
if (const int rc = posix_spawn(&pid, argv[0], nullptr, nullptr, argv.data(), envp.data()); rc != 0) {
    return std::unexpected(Error{.code = Errc::failure, .message = std::format("cannot run git: {}", std::strerror(rc))});
}
```
Prefer an in-process library over spawning a tool whenever one exists.

## 2. File descriptors
- **`O_CLOEXEC` on every descriptor**, so children don't inherit them:
  - `open(..., O_CLOEXEC)`, `openat(..., O_CLOEXEC)`;
  - `mkostemp(tmpl, O_CLOEXEC)`, `pipe2(fds, O_CLOEXEC)`;
  - `socket(..., SOCK_CLOEXEC)`, `accept4(..., SOCK_CLOEXEC)`;
  - `dup3`/`F_DUPFD_CLOEXEC`;
  - `fopen(path, "re")`: the glibc `e` mode flag.
- Own every fd with RAII (a `unique_fd` type or `unique_ptr` with a closer), and **check `close()`** on files you wrote.
- Never `close(1)` or `close(2)` (`cli-design`: flush-and-check, never close stderr).

## 3. Temporary files
- Use **`mkstemp`/`mkostemp`/`mkdtemp`**. **Never `tmpnam`, `tempnam` or `mktemp`**: they give a predictable name and a race.
- `std::filesystem::temp_directory_path()` reads `TMPDIR` and similar variables, so it is **environment-controlled**. That is fine for scratch space. Create files inside it with `mkstemp`, never under a predictable name, and never base a security decision on the path.

## 4. TOCTOU and symlinks
- **Open first, then `fstat` the descriptor.** `stat()`/`exists()` followed by `open()` is a race.
- **`std::filesystem` operations are path-based, not race-safe.**
  - Fine for the user's own files in a normal workflow.
  - Wrong for directories other users can write, or for "is this path inside X" decisions.
  - Never `remove_all` a tree that someone else can write to.
- For security-relevant paths, walk with descriptors:
  - `openat(dirfd, name, O_NOFOLLOW | O_CLOEXEC ...)`. `O_NOFOLLOW` covers only the **last** component.
  - `O_DIRECTORY` when you expect a directory.
- **Optional, Linux ≥ 5.6: `openat2` with `RESOLVE_BENEATH`** (or `RESOLVE_NO_SYMLINKS` / `RESOLVE_IN_ROOT`) confines the *whole* resolution to a directory.
  - glibc has **no wrapper**. Call it via `syscall(SYS_openat2, dirfd, path, &how, sizeof how)` with `<linux/openat2.h>` and `<sys/syscall.h>`. `syscall` is variadic, so add a `NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)` with a reason.
  - **Fall back on `ENOSYS`** (old kernel) to `openat` + `O_NOFOLLOW` plus a containment check. This is an option, not a rule. If your headers lack `SYS_openat2`, use the fallback.
  - If you target seccomp-filtered sandboxes, have the fact-checker verify what errno they return for unknown syscalls.
- **Containment check for user-supplied paths:**
  1. Canonicalize the base with `std::filesystem::canonical`.
  2. Canonicalize the candidate with `std::filesystem::weakly_canonical` (or the `error_code` overloads).
  3. Compare **path components** (`std::ranges::mismatch(base, candidate)`), never a string prefix: `/srv/data2` starts with `/srv/data`.
  4. Still open with the flags above, because the check alone races.
- **Atomic write** (also the interruption story in `cli-design`):
  1. `mkostemp` **in the destination directory** (`.<name>.XXXXXX`), with `O_CLOEXEC`.
  2. An explicit **`fchmod`**: **0600 for anything secret**, otherwise the intended mode. Don't rely on umask.
  3. Write everything, looping on short writes and `EINTR`.
  4. `fsync(fd)`, then check `close(fd)`.
  5. `rename(tmp, final)`, which is atomic on one filesystem. Optionally `fsync` the directory.
  6. On any failure, `unlink(tmp)`.

## 5. Format strings
- `std::format`/`std::print` only with **literal format strings**, which are checked at compile time. User data always goes in as an argument: `std::print(io.out, "{}\n", text)`.
- **Never `std::vformat` or `std::runtime_format` with untrusted strings. Never `printf(user)`.**
- The warning set includes `-Wformat=2 -Werror=format-security` (`cpp-quality`). Don't suppress them.

## 6. Integer and size math
- **Checked arithmetic on every size computed from input:**
  - `__builtin_add_overflow` / `__builtin_mul_overflow` (GCC and Clang) before allocating or indexing;
  - `std::in_range<T>(x)` before every narrowing `static_cast`;
  - `std::cmp_less` and friends for signed/unsigned comparisons.
- ```cpp
  std::size_t bytes = 0;
  if (__builtin_mul_overflow(count, sizeof(Record), &bytes) || bytes > max_input_bytes) {
      return std::unexpected(Error{.code = Errc::failure, .message = "input too large"});
  }
  ```
- `-Wconversion -Wsign-conversion` are on, so every cast you add is a claim to justify.

## 7. Untrusted input
- **Bound every read.** Never slurp an unbounded stream or file into memory: no unbounded `std::getline`, no `istreambuf_iterator` over stdin.
  - Read with a size cap, and reject lines or records over a documented limit with a clear error.
  - Check file sizes with `fstat` on the open fd before reading.
- **argv and paths are bytes**, not guaranteed UTF-8.
  - Never assume they are valid text.
  - JSON output uses `error_handler_t::replace` (`cli-design`).
  - Error messages that echo input should quote it and strip or escape control characters, so a filename can't inject terminal escape sequences.
- **JSON input:**
  - Cap the size first.
  - Parse with `nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false)` and check `is_discarded()`, or catch `nlohmann::json::exception` at the boundary.
  - Validate types before `get<T>()`.
- **Validators are UX, not security** (`cli-design`). `CLI::ExistingFile` doesn't make the later `open` safe.

## 8. Environment
- `getenv` results are **untrusted**. They are read **once at startup** into the `EnvSnapshot` (`src/app/io.cpp`), already interpreted. No other code calls `getenv`.
- **Never `setenv`/`putenv`** in the running program; they aren't thread-safe. Build a child's environment explicitly (§1).
- `PATH`, `HOME`, `TMPDIR`, `LD_*`, `XDG_*` and the locale variables are all caller-controlled. The tool is never setuid. If it ever has to be, that needs a separate design (`secure_getenv`, sanitized env), not this checklist.

## 9. Secrets
- **Never in argv.** `/proc/<pid>/cmdline` and `ps` expose it to other processes. The environment is readable by the same user (`/proc/<pid>/environ`) and leaks to children.
- Prefer, in order:
  1. a file descriptor (`--password-fd N`);
  2. a 0600 file (`--password-file`), which you verify after opening via `fstat`;
  3. `--password-stdin`.

  An env var is acceptable only when documented as the less-safe option.
- **Never logged.** Redact secrets in `--verbose` output, in `--json` error objects and in echoed URLs (`https://user:***@host`).
- **Never `capture_default_str()` on a secret option.** Keep secrets out of config files that get committed.
- Write secret files only through the atomic-write path with `fchmod(0600)`. Clear secret buffers after use where practical (`explicit_bzero` on glibc). This is best-effort, not a guarantee.

## 10. Supply chain
- **`pixi.lock` is committed and reviewed in PRs.** Never edit it by hand. CI installs with `pixi install --locked`.
- Pin every `FetchContent` dependency by **`URL` + `URL_HASH SHA256=...`** or by **`GIT_TAG <full commit SHA>`**, never by a branch or a movable tag.
- **GitHub Actions are pinned by full commit SHA** with a `# vN` comment and updated by Dependabot. Resolve SHAs at use-time with `gh`; never hardcode them from memory (`build-workflow`).
- **No `curl | sh`** and no unpinned downloads in scripts, CI or the docs.
- Vendored code records its upstream URL, commit SHA and license next to it.
- A new dependency needs a reason. Prefer what conda-forge already ships in the lock.

## 11. The `audit` task (dependency CVE scan)
`nice pixi run audit` runs `scripts/audit.sh`, which comes from the `build-workflow` scaffold.

When it runs:
- in `/harden` and in the security review of a feature that changes dependencies;
- on a schedule in CI;
- never inside `check`.

**pixi has no built-in scanner.** The task follows pixi's documented two-step:
```bash
syft .pixi/envs/default --select-catalogers=+conda-meta-cataloger --output syft-json=.platform-runs/audit/sbom.json
grype .platform-runs/audit/sbom.json
```
**Ignores** live in `audit/grype.yaml`, which `audit.sh` passes with an explicit `grype -c` (that also stops grype from reading `./.grype.yaml` or `~/.grype.yaml`). Each entry pins `vulnerability` + `package { name, version, type }` and carries a dated `reason` that says why the artifact is not affected, so an upgraded package or a new CVE alerts again. A package-name-only rule would silence every future CVE for that package: never write one.

The contract `audit.sh` must keep:
1. **Probe first.** If `syft` or `grype` is missing, print what is missing, print the coverage statement saying nothing was scanned, and **exit 0 as a stated no-op**. **Never install anything.**
2. **Always print a coverage statement**, whatever the result, and quote it in reviews verbatim. It must say:
   - the tool names and versions (read at run time);
   - what was scanned: the conda packages installed in the `default` env, which is the one the release artifact is built from;
   - that most scanned packages are build toolchain only and never ship, and which kinds are compiled or linked into the binary;
   - that matching is **CPE-heuristic**: Syft emits CPEs, not PURLs, for conda packages, so expect false positives and misses. Say plainly that the statically linked toolchain runtime (libstdc++/libgcc/sysroot packages) is effectively not matched, because their conda names map to no NVD product;
   - what is **NOT covered**: host/system libraries such as the target machine's glibc and the user's own libcurl (dlopen()ed at run time), vendored or FetchContent code, and `pixi.lock` itself. **Dependabot doesn't update `pixi.lock`, and GitHub's dependency graph doesn't read it.**
3. **Exit nonzero on findings** at or above the configured threshold. A tool error is nonzero too.
4. Keep the SBOM under `.platform-runs/audit/`, which is never committed.

Other managers, if the project ever switches (see `build-workflow`):
- **vcpkg:** Dependabot's `vcpkg` ecosystem does version updates (it bumps `builtin-baseline`). Scan the installed tree with syft + grype.
- **Conan:** OSV-Scanner reads `conan.lock`, the only C/C++ lockfile it supports.
- **Vendored or CPM:** an OSV-Scanner recursive scan with submodules, or a manual review of the pinned SHAs.

Verify OSV-Scanner's CLI flags against the installed version (`--help`); they changed across majors.

An SBOM is optional (syft). CMake's `install(SBOM)` is experimental; don't bake it in.

## When NOT to apply
- A dev-only script under `scripts/` that never ships and never sees user input needs the shell rules, but not fd-level TOCTOU hardening.
- Don't wrap the user's *own* files in `openat2` ceremony when the threat model has no other writer. Note the decision in the design instead.
- Don't add secret-handling flags to a tool that handles no secrets.

## Sibling skills
- `memory-safety`: sanitizers, release hardening (`_FORTIFY_SOURCE=3`, stack protector, RELRO/NOW, PIE), UB.
- `cli-design`: streams, exit codes, atomic writes for interruption, JSON output.
- `modern-cpp-conventions`: RAII wrappers for C resources, views and C-string APIs.
- `build-workflow`: lockfile, CI pinning, release attestation.
- `low-priority-execution`: `nice pixi run audit`.
