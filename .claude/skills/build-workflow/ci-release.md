---
name: build-workflow-ci-release
description: Supporting file of the build-workflow skill (CI, release and packaging templates read by /setup). Not a standalone skill.
---

# build-workflow: CI, release and packaging templates

`/setup` writes `scripts/release-check.sh` always (the `release-test` task needs it). It writes the CI files only after asking (step 9), and `packaging/aur/PKGBUILD` only if you want it. Each block goes to the **target path in its heading**, with `@TOOL@`, `@PROJ@`, `@PROJ_UPPER@`, `@DESCRIPTION@` and `@SPDX@` substituted. Rules and rationale: the `build-workflow` skill, §9–§11.

## Filling the placeholders (use-time, never from memory)
- **`<sha> # <tag>`** on every `uses:` line becomes `<40-hex commit sha> # <tag>`. Resolve each action's latest release with `gh api` (latest release tag → `git/ref/tags/<tag>` → dereference an annotated tag), as `/setup` step 9 describes. If that is impossible, leave `<sha>` in place with `# TODO: pin SHA` and say so: the workflow fails closed until it is filled. No action major is hardcoded here. `upload-artifact` and `download-artifact` majors are resolved independently, because they can differ.
- **`<pixi-version>`** becomes the exact pixi version in use locally (`pixi --version`, written as `vX.Y.Z`). setup-pixi's inputs can change between minor versions, so the action is pinned exactly too.
- **`main`** in `ci.yml` becomes the repository's default branch. **`<owner>`** in the PKGBUILD becomes the GitHub owner.
- Then have `fact-checker` confirm the inputs these templates use against each resolved tag's `action.yml`:

| action | inputs / permissions used here |
|---|---|
| `actions/checkout` | `persist-credentials`, `fetch-depth` |
| `prefix-dev/setup-pixi` | `pixi-version`, `locked`, `environments`, `cache` |
| `actions/upload-artifact` | `name`, `path`, `if-no-files-found`, `retention-days` |
| `actions/download-artifact` | `name`, `path` |
| `actions/attest` | `subject-checksums`; job permissions `id-token: write`, `attestations: write`, `artifact-metadata: write` (attestations on private repos may need GitHub Enterprise Cloud) |
| `gh` CLI (preinstalled) | `release create --verify-tag --draft --generate-notes --title`, `release edit --draft=false` |

Also verified at use-time: `ubuntu-26.04` is still the right pinned image; `pixi exec --spec …` and the availability of `syft`/`grype` on conda-forge (otherwise use the SHA-pinned Anchore actions or drop the job); `groff` on the runner; the readelf marker spelling in `release-check.sh` against a known-good build of this project.

## `.github/workflows/ci.yml`
Matrix over the two pixi envs: `check`, then `release-test`; a scheduled `audit`; optional macOS/Windows commented out. CI runs at normal priority (never `nice`).

```yaml
# CI: the same pixi tasks as local, in both envs. Every `uses:` is pinned by full commit SHA
# with the tag in a comment; /setup resolves them (gh api) and Dependabot keeps them current.
# `<sha> # <tag>` / `<pixi-version>` are placeholders /setup fills -- the workflow fails
# closed until they are real.
name: ci

on:
  push:
    branches: [main] # your default branch
  pull_request: # never pull_request_target
  schedule:
    - cron: "23 5 * * 1" # weekly dependency audit
  workflow_dispatch:

permissions:
  contents: read

concurrency:
  group: ${{ github.workflow }}-${{ github.ref }}
  cancel-in-progress: ${{ github.event_name == 'pull_request' }}

jobs:
  check:
    name: check (${{ matrix.env }})
    if: github.event_name != 'schedule'
    runs-on: ubuntu-26.04 # pinned: ubuntu-latest moves between images
    timeout-minutes: 90
    strategy:
      fail-fast: false
      matrix:
        env: [default, gcc] # default = clang (+ lint), gcc = second compiler
    steps:
      - uses: actions/checkout@<sha> # <tag>
        with:
          persist-credentials: false

      # Sanitizers need ASLR entropy they can map around. Hosted images already set this;
      # it matters on self-hosted runners and containers. Defensive, never fatal.
      - name: ASLR entropy for sanitizers
        run: sudo sysctl -w vm.mmap_rnd_bits=28 || true

      - uses: prefix-dev/setup-pixi@<sha> # <tag>
        with:
          pixi-version: <pixi-version> # exact, e.g. the local `pixi --version`; the action's API can change between minors
          locked: true # fail if pixi.lock is stale vs pixi.toml
          environments: ${{ matrix.env }}
          cache: true

      - name: Quality gate (fmt-check, sanitized tests, lint, release build)
        run: pixi run -e ${{ matrix.env }} check

      - name: Release tests (unit + E2E + release-check on the release install)
        run: pixi run -e ${{ matrix.env }} release-test

      - name: Man page lint (groff exits 0 on warnings, so fail on any output)
        if: matrix.env == 'default' && hashFiles('docs/@TOOL@.1') != ''
        run: |
          if ! command -v groff >/dev/null; then
            echo "::warning::groff is not on this runner; man page lint skipped"
            exit 0
          fi
          out=$(groff -ww -z -man docs/@TOOL@.1 2>&1)
          if [ -n "$out" ]; then printf '%s\n' "$out"; exit 1; fi

      - name: Upload test logs
        if: failure()
        uses: actions/upload-artifact@<sha> # <tag>
        with:
          name: ctest-logs-${{ matrix.env }}
          path: build/*/*/Testing/Temporary/LastTest*.log
          if-no-files-found: ignore
          retention-days: 7

  audit:
    name: dependency audit
    if: github.event_name == 'schedule' || github.event_name == 'workflow_dispatch'
    runs-on: ubuntu-26.04
    timeout-minutes: 30
    steps:
      - uses: actions/checkout@<sha> # <tag>
        with:
          persist-credentials: false
      - uses: prefix-dev/setup-pixi@<sha> # <tag>
        with:
          pixi-version: <pixi-version>
          locked: true
          environments: default
          cache: true
      # syft + grype in a throwaway env. Verify at /setup that both are on conda-forge
      # (`pixi search syft`, `pixi search grype`); otherwise use the SHA-pinned Anchore actions.
      - name: syft (conda-meta cataloger) -> grype, with coverage statement
        run: pixi exec --spec syft --spec grype -- bash scripts/audit.sh

  # Optional: macOS / Windows. The only way to license those platform tiers. Needs
  # osx-arm64 / win-64 in pixi.toml `platforms`, matching compiler features, and presets that
  # don't assume ELF (release-check is Linux-only). Uncomment and adapt deliberately.
  # check-macos:
  #   if: github.event_name != 'schedule'
  #   runs-on: macos-<version>
  #   steps:
  #     - uses: actions/checkout@<sha> # <tag>
  #       with:
  #         persist-credentials: false
  #     - uses: prefix-dev/setup-pixi@<sha> # <tag>
  #       with:
  #         pixi-version: <pixi-version>
  #         locked: true
  #     - run: pixi run test
```

## `.github/workflows/release.yml`
Tag-triggered. `permissions: {}` at the top, and each job grants only what it needs. The shipped artifact comes from the default (clang) env only.

```yaml
# Release: tag push only (vX.Y.Z). Agents never tag, push tags or run `gh release`.
# build: read-only token -> gate + release tests + tarball. publish: SHA256SUMS -> build
# provenance attestation -> draft release -> upload -> publish.
# `<sha> # <tag>` / `<pixi-version>` are placeholders /setup fills (see ci.yml).
name: release

on:
  push:
    tags: ["v*.*.*"]

permissions: {}

concurrency:
  group: release-${{ github.ref }}
  cancel-in-progress: false

jobs:
  build:
    name: build, test, package (default env)
    runs-on: ubuntu-26.04
    timeout-minutes: 90
    permissions:
      contents: read
    steps:
      - uses: actions/checkout@<sha> # <tag>
        with:
          persist-credentials: false
          fetch-depth: 0 # SOURCE_DATE_EPOCH from the tagged commit

      - name: ASLR entropy for sanitizers
        run: sudo sysctl -w vm.mmap_rnd_bits=28 || true

      - uses: prefix-dev/setup-pixi@<sha> # <tag>
        with:
          pixi-version: <pixi-version>
          locked: true
          environments: default
          cache: true

      - name: Reproducibility input (best effort, no bit-for-bit promise)
        run: echo "SOURCE_DATE_EPOCH=$(git log -1 --format=%ct)" >> "$GITHUB_ENV"

      - name: Quality gate
        run: pixi run check

      - name: Release tests + tarball (package depends on release-test)
        run: pixi run package

      - name: Tag matches --version
        env:
          TAG: ${{ github.ref_name }}
        run: |
          got=$(build/default/release/e2e-prefix/bin/@TOOL@ --version | head -n 1)
          want="@TOOL@ ${TAG#v}"
          if [ "$got" != "$want" ]; then
            echo "::error::tag $TAG but --version prints '$got' (expected '$want'); bump project(VERSION)"
            exit 1
          fi

      - uses: actions/upload-artifact@<sha> # <tag>
        with:
          name: dist
          path: build/default/release/@TOOL@-*.tar.gz
          if-no-files-found: error

  publish:
    needs: build
    runs-on: ubuntu-26.04
    timeout-minutes: 15
    permissions:
      contents: write # create the release
      id-token: write # attestation signing
      attestations: write
      artifact-metadata: write # required by the attest action (verify against its action.yml at /setup)
    steps:
      - uses: actions/download-artifact@<sha> # <tag> (its major may differ from upload-artifact's)
        with:
          name: dist
          path: dist

      - name: SHA256SUMS
        working-directory: dist
        run: |
          sha256sum -- *.tar.gz > SHA256SUMS
          cat SHA256SUMS

      - name: Attest build provenance
        uses: actions/attest@<sha> # <tag>
        with:
          subject-checksums: dist/SHA256SUMS

      - name: Draft release, upload assets, publish
        env:
          GH_TOKEN: ${{ github.token }}
          GH_REPO: ${{ github.repository }}
          TAG: ${{ github.ref_name }}
        run: |
          gh release create "$TAG" --verify-tag --draft --generate-notes --title "$TAG" dist/*.tar.gz dist/SHA256SUMS
          gh release edit "$TAG" --draft=false
```

## `.github/dependabot.yml`
Keeps the SHA-pinned actions current. There is no pixi.lock support.

```yaml
# Dependabot keeps the SHA-pinned actions current (it rewrites the SHA and the `# <tag>`
# comment). It cannot update pixi.lock (no pixi support): refresh that deliberately with
# `pixi update` (asked) and review the diff. The `vcpkg` ecosystem only applies to the
# documented vcpkg fallback; `conda` updates manifests but not lockfiles.
version: 2
updates:
  - package-ecosystem: github-actions
    directory: /
    schedule:
      interval: weekly
    groups:
      github-actions:
        patterns: ["*"]
    commit-message:
      prefix: ci
```

## `scripts/release-check.sh`
The portability and hardening gate on the **installed release binary**. The `release-check.binary` CTest test (`cpp-testing`) runs it as `bash <repo>/scripts/release-check.sh <prefix>/bin/@TOOL@ ${@PROJ_UPPER@_GLIBC_FLOOR}`. Non-mutating, so `readelf`/`objdump` are fine to allowlist.

```bash
#!/usr/bin/env bash
# release-check.sh <installed-binary> [<glibc-floor>]
#
# Portability + hardening gate for the RELEASE install (registered as the CTest test
# `release-check.binary`; also run by the release workflow). Non-mutating: readelf/objdump.
#   - PIE (ELF type DYN), non-executable GNU_STACK, GNU_RELRO, BIND_NOW
#   - no RPATH/RUNPATH (env `default`); in env `gcc` only a warning (its compiler wrapper
#     always adds one -- gcc-env release binaries are a check, never shipped)
#   - NEEDED is a subset of glibc's own libraries (libstdc++/libgcc are linked statically)
#   - highest GLIBC_x.y symbol version <= floor (default 2.28 = the conda sysroot)
# The readelf markers are binutils conventions, not from the OpenSSF guide: /setup compares
# them against a known-good build of this project before trusting this gate.
# Exit: 0 all pass; 1 a check failed; 2 usage / tools missing.
set -uo pipefail

bin=${1:-}
floor=${2:-2.28}
if [[ -z $bin || ! -f $bin ]]; then
  echo "usage: release-check.sh <installed-binary> [<glibc-floor>]" >&2
  exit 2
fi
[[ $floor =~ ^[0-9]+\.[0-9]+$ ]] || { echo "release-check: bad glibc floor '$floor'" >&2; exit 2; }

resolve() { # $1 = tool -> host tool, else the conda-prefixed one from the env
  local t=$1 c
  if command -v "$t" >/dev/null 2>&1; then
    command -v "$t"
    return 0
  fi
  for c in "${CONDA_PREFIX:-/nonexistent}"/bin/*-"$t"; do
    if [[ -x $c ]]; then
      echo "$c"
      return 0
    fi
  done
  return 1
}
readelf=$(resolve readelf) || { echo "release-check: readelf not found (binutils)" >&2; exit 2; }
objdump=$(resolve objdump) || { echo "release-check: objdump not found (binutils)" >&2; exit 2; }

env_name=${PIXI_ENVIRONMENT_NAME:-default}
fail=0
pass() { echo "release-check: PASS  $*"; }
bad() { echo "release-check: FAIL  $*"; fail=1; }
warn() { echo "release-check: WARN  $*"; }

hdr=$("$readelf" -hW "$bin") || { echo "release-check: readelf failed on $bin" >&2; exit 2; }
seg=$("$readelf" -lW "$bin")
dyn=$("$readelf" -dW "$bin")

if grep -Eq '^[[:space:]]*Type:[[:space:]]+DYN' <<<"$hdr"; then pass "PIE (Type: DYN)"; else bad "not PIE (ELF type is not DYN)"; fi

stack=$(grep -E '^[[:space:]]*GNU_STACK' <<<"$seg" || true)
if [[ -z $stack ]]; then
  bad "no GNU_STACK segment (stack executability unknown)"
elif grep -Eq 'RWE[[:space:]]' <<<"$stack"; then
  bad "executable stack (GNU_STACK is RWE)"
else
  pass "non-executable stack (GNU_STACK RW)"
fi

if grep -Eq '^[[:space:]]*GNU_RELRO' <<<"$seg"; then pass "GNU_RELRO present"; else bad "no GNU_RELRO segment"; fi

if grep -Eq '\(BIND_NOW\)|\(FLAGS_1\).*NOW' <<<"$dyn" || grep -Eq '\(FLAGS\).*BIND_NOW' <<<"$dyn"; then
  pass "BIND_NOW (full RELRO)"
else
  bad "no BIND_NOW / FLAGS_1 NOW (link with -z now)"
fi

rpath=$(grep -E '\((RPATH|RUNPATH)\)' <<<"$dyn" || true)
if [[ -z $rpath ]]; then
  pass "no RPATH/RUNPATH"
elif [[ $env_name == gcc ]]; then
  warn "RPATH/RUNPATH present in the gcc env (expected: its wrapper adds one; never ship this binary): $rpath"
else
  bad "RPATH/RUNPATH present: $rpath"
fi

allowed='^(libc\.so\.6|libm\.so\.6|ld-linux-x86-64\.so\.2|ld-linux-aarch64\.so\.1|libpthread\.so\.0|libdl\.so\.2|librt\.so\.1)$'
needed=$(sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' <<<"$dyn")
extra=""
while IFS= read -r lib; do
  [[ -z $lib ]] && continue
  [[ $lib =~ $allowed ]] || extra+=" $lib"
done <<<"$needed"
if [[ -z $extra ]]; then
  pass "NEEDED within glibc: $(tr '\n' ' ' <<<"$needed")"
else
  bad "NEEDED outside the allowlist:$extra (libstdc++/libgcc_s must be linked statically)"
fi

max=$("$objdump" -T "$bin" | grep -o 'GLIBC_[0-9][0-9.]*' | sed 's/^GLIBC_//' | sort -Vu | tail -n 1)
if [[ -z $max ]]; then
  pass "no versioned glibc symbols"
elif [[ "$(printf '%s\n%s\n' "$max" "$floor" | sort -V | tail -n 1)" == "$floor" ]]; then
  pass "glibc floor: needs GLIBC_$max <= $floor"
else
  bad "glibc floor: needs GLIBC_$max > allowed $floor"
fi

# Capture first: under pipefail, `readelf | grep -q` reports readelf's SIGPIPE when grep exits early.
dsyms=$("$readelf" -W --dyn-syms "$bin" 2>/dev/null || true)
if grep -Eq '__stack_chk_fail|__[a-z0-9_]+_chk' <<<"$dsyms"; then
  pass "stack-protector / fortify symbols present (informational)"
else
  warn "no __stack_chk_fail / __*_chk imports (informational; small programs may legitimately have none)"
fi

exit "$fail"
```

## `packaging/aur/PKGBUILD`
Optional. Uses the system `cli11`, `nlohmann-json` and `catch2` (all in Arch `extra`; verify at use-time). `.SRCINFO` is generated, never hand-written.

```bash
# Maintainer: <name> <email>
# Optional AUR template (Arch's CMake package guidelines). Distro builds use the system
# libraries and makepkg's own CFLAGS/LDFLAGS (CMAKE_BUILD_TYPE=None), not pixi; -Werror off.
# Before publishing: fill url/sha256sums (updpkgsums), check with namcap (advisory), and
# regenerate .SRCINFO with `makepkg --printsrcinfo > .SRCINFO`. Verify the -git/-bin
# naming conventions at use-time. Agents never run `makepkg -i`.
# The source dir "$pkgname-$pkgver" assumes the GitHub repo is named like the tool (GitHub
# archives unpack to <repo>-<ver>); adjust build()'s -S path otherwise.
pkgname=@TOOL@
pkgver=0.1.0
pkgrel=1
pkgdesc="@DESCRIPTION@"
arch=('x86_64')
url="https://github.com/<owner>/@TOOL@"
license=('@SPDX@') # SPDX identifier; LicenseRef-<name> for a custom license
depends=('gcc-libs' 'glibc')
makedepends=('cmake' 'ninja' 'cli11' 'nlohmann-json' 'catch2') # catch2: tests are configured in build()
source=("$pkgname-$pkgver.tar.gz::$url/archive/refs/tags/v$pkgver.tar.gz")
sha256sums=('SKIP') # replace via updpkgsums before publishing

build() {
  cmake -B build -S "$pkgname-$pkgver" -G Ninja \
    -W no-dev \
    -D CMAKE_BUILD_TYPE=None \
    -D CMAKE_INSTALL_PREFIX=/usr \
    -D @PROJ_UPPER@_WARNINGS_AS_ERRORS=OFF
  cmake --build build
}

check() {
  # release-check.* gates the hardened, static release artifact; distro builds are neither.
  ctest --test-dir build --output-on-failure -E '^release-check\.'
}

package() {
  DESTDIR="$pkgdir" cmake --install build
  install -Dm644 "$pkgname-$pkgver/LICENSE" "$pkgdir/usr/share/licenses/$pkgname/LICENSE"
}
```
