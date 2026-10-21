#!/usr/bin/env bash
# audit.sh [--fail-on <negligible|low|medium|high|critical>] -- dependency CVE scan.
#
# pixi has no built-in scanner; this follows pixi's documented two-step: syft catalogs the
# installed env with its conda-meta cataloger, grype matches the SBOM against vulnerability
# data. Conda packages carry CPEs, not PURLs, so matching is heuristic.
# Always prints a coverage statement. Exit: nonzero on findings at/above the threshold (or
# a tool error); 0 with a stated no-op when syft/grype are absent. Never installs anything.
set -uo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P) || exit 1
cd "$root" || exit 1

fail_on=high # conda CPE matching is noisy; tighten with `pixi run audit --fail-on medium`
while [[ $# -gt 0 ]]; do
  case "$1" in
    --fail-on)
      [[ $# -ge 2 ]] || { echo "audit: --fail-on needs a severity" >&2; exit 2; }
      fail_on=$2
      shift 2
      ;;
    *) echo "usage: audit.sh [--fail-on <negligible|low|medium|high|critical>]" >&2; exit 2 ;;
  esac
done
case "$fail_on" in negligible | low | medium | high | critical) ;; *) echo "audit: bad severity '$fail_on'" >&2; exit 2 ;; esac

target=".pixi/envs/default"
config="audit/grype.yaml" # pinned, reasoned ignores; explicit -c also stops ./.grype.yaml / ~/.grype.yaml discovery
out_dir=".platform-runs/audit"
sbom="$out_dir/sbom.json"

coverage() { # $1 = tool line
  echo "audit: coverage statement"
  echo "  tools:        $1"
  echo "  scanned:      every conda package installed in $target (the env the release artifact is built"
  echo "                from). Most are build toolchain only and never ship (cmake, clang, ... and their deps,"
  echo "                e.g. cmake's libcurl -> libssh2/krb5); only a few are compiled or linked in (headers such"
  echo "                as cli11, nlohmann_json and curl's; static libstdc++/libgcc; glibc's crt objects)"
  echo "  ignores:      $config (each pinned to a package version, with a reason)"
  echo "  matching:     CPE-heuristic for conda packages (false positives and misses are possible); the"
  echo "                statically linked toolchain runtime (libstdc++.a, libgcc.a, libc_nonshared.a and the"
  echo "                crt objects, from libstdcxx-devel/libgcc-devel/sysroot) is effectively NOT MATCHED:"
  echo "                syft derives CPEs from the conda package names (e.g. sysroot_linux-64), which name no"
  echo "                NVD product"
  echo "  NOT covered:  host/system libraries the binary uses at run time -- glibc, and the user's own"
  echo "                libcurl.so.4 and its TLS/ssh/krb5 deps (only curl's headers come from the env; the"
  echo "                binary dlopen()s libcurl on the user's machine) -- vendored or FetchContent code,"
  echo "                the gcc env, and pixi.lock itself (Dependabot / the GitHub dependency graph do not read it)"
}

have_syft=0
have_grype=0
command -v syft >/dev/null 2>&1 && have_syft=1
command -v grype >/dev/null 2>&1 && have_grype=1
if [[ $have_syft -eq 0 || $have_grype -eq 0 ]]; then
  missing=()
  [[ $have_syft -eq 1 ]] || missing+=(syft)
  [[ $have_grype -eq 1 ]] || missing+=(grype)
  coverage "none (missing: ${missing[*]})"
  echo "audit: NO-OP -- ${missing[*]} not on PATH; nothing was scanned. Install them yourself (they are not installed by this task) or rely on the scheduled CI audit job."
  exit 0
fi
if [[ ! -f $config ]]; then
  echo "audit: $config is missing" >&2
  exit 1
fi
if [[ ! -d $target ]]; then
  echo "audit: $target does not exist -- run 'pixi install' first" >&2
  exit 1
fi

syft_v=$(syft version 2>/dev/null | sed -n 's/^Version:[[:space:]]*//p' | head -n 1)
grype_v=$(grype version 2>/dev/null | sed -n 's/^Version:[[:space:]]*//p' | head -n 1)
coverage "syft ${syft_v:-?}, grype ${grype_v:-?} (fail on >= $fail_on)"

mkdir -p "$out_dir" || exit 1
syft "$target" --select-catalogers=+conda-meta-cataloger --output "syft-json=$sbom" || {
  echo "audit: syft failed" >&2
  exit 1
}
# --fail-on makes grype exit nonzero on findings; without it grype exits 0 regardless.
# Verify the flag against the installed grype (`grype --help`) -- it is checked here too.
# Capture first: under pipefail, `grype --help | grep -q` can report grype's SIGPIPE when grep exits early.
grype_help=$(grype --help 2>&1 || true)
if grep -q -- '--fail-on' <<<"$grype_help"; then
  grype -c "$config" "$sbom" --fail-on "$fail_on"
  status=$?
else
  echo "audit: WARNING: this grype has no --fail-on; findings are listed but cannot fail the task" >&2
  grype -c "$config" "$sbom"
  status=$?
fi
echo "audit: SBOM kept at $sbom"
exit "$status"
