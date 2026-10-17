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
