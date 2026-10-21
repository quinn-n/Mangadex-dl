#!/usr/bin/env bash
# audit.sh [--scope binary|dev|all] [--fail-on <negligible|low|medium|high|critical>] -- dependency CVE scan.
#
# pixi has no built-in scanner; this follows pixi's documented two-step: syft catalogs the
# installed env with its conda-meta cataloger, grype matches the SBOM against vulnerability
# data. Conda packages carry CPEs, not PURLs, so matching is heuristic.
#
# The SBOM is split in two disjoint halves by audit/binary-packages.txt (derived from the release
# build and kept honest by `pixi run binary-packages`, part of `check`), so a finding is reported once:
#   binary  the packages compiled or linked into the shipped binary; grype -c audit/grype-binary.yaml,
#           fails on >= medium by default
#   dev     every other package in the env (the build toolchain and its deps); grype -c
#           audit/grype-dev.yaml (pinned, reasoned ignores), fails on >= high by default
# --scope picks one half (default: all = both); --fail-on overrides both thresholds.
# Before grype runs, audit/cpe-overrides.json gives binary-half packages whose conda names map to no
# NVD product real CPEs (the static libstdc++/libgcc are matched as gnu:gcc).
# Always prints a coverage statement per scope. Exit: nonzero on findings at/above a threshold (or
# a tool error); 0 with a stated no-op when syft/grype/jq are absent. Never installs anything.
set -uo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P) || exit 1
cd "$root" || exit 1

usage() { echo "usage: audit.sh [--scope binary|dev|all] [--fail-on <negligible|low|medium|high|critical>]" >&2; }
scope=all
fail_on="" # empty: the per-scope default below
while [[ $# -gt 0 ]]; do
  case "$1" in
    --scope)
      [[ $# -ge 2 ]] || { echo "audit: --scope needs binary, dev or all" >&2; exit 2; }
      scope=$2
      shift 2
      ;;
    --fail-on)
      [[ $# -ge 2 ]] || { echo "audit: --fail-on needs a severity" >&2; exit 2; }
      fail_on=$2
      shift 2
      ;;
    *) usage; exit 2 ;;
  esac
done
case "$scope" in binary | dev | all) ;; *) echo "audit: bad scope '$scope'" >&2; exit 2 ;; esac
case "$fail_on" in "" | negligible | low | medium | high | critical) ;; *) echo "audit: bad severity '$fail_on'" >&2; exit 2 ;; esac
scopes=(binary dev)
[[ $scope == all ]] || scopes=("$scope")

target=".pixi/envs/default"
list="audit/binary-packages.txt"
cpe_overrides="audit/cpe-overrides.json"
out_dir=".platform-runs/audit"
sbom="$out_dir/sbom.json"

threshold() { # $1 = scope
  if [[ -n $fail_on ]]; then
    echo "$fail_on"
  elif [[ $1 == binary ]]; then
    echo medium # anything here can reach users
  else
    echo high # build toolchain only; conda CPE matching is noisy
  fi
}

coverage() { # $1 = scope, $2 = tool line
  echo "audit ($1): coverage statement"
  echo "  tools:        $2"
  if [[ $1 == binary ]]; then
    echo "  scanned:      the conda packages in $target whose files are compiled or linked into the shipped"
    echo "                binary, as listed in $list (derived from the release build's header deps and"
    echo "                linker depfile; config audit/grype-binary.yaml)"
    echo "  matching:     CPE-heuristic for conda packages (false positives and misses are possible). syft's"
    echo "                CPEs come from conda package names, which mostly name no NVD product, so"
    echo "                $cpe_overrides adds real ones: the static libstdc++.a / libgcc.a are matched as"
    echo "                gnu:gcc at their version. Still effectively NOT MATCHED: glibc's crt objects and"
    echo "                libc_nonshared.a (sysroot; libc.so itself is the user's), clang's builtin headers and"
    echo "                the kernel UAPI headers"
    echo "  NOT covered:  host/system libraries the binary uses at run time -- glibc, and the user's own"
    echo "                libcurl.so.4 and its TLS/ssh/krb5 deps (libcurl is only headers here; it is"
    echo "                dlopen()ed on the user's machine) -- vendored or FetchContent code, and pixi.lock"
    echo "                itself (Dependabot / the GitHub dependency graph do not read it)"
  else
    echo "  scanned:      every other conda package in $target: the build toolchain (cmake, clang, ...) and"
    echo "                its transitive deps, none of which ship in the artifact (config audit/grype-dev.yaml,"
    echo "                whose ignores are pinned to package versions)"
    echo "  matching:     CPE-heuristic for conda packages (false positives and misses are possible)"
    echo "  NOT covered:  the gcc env, host tools on the build machine, and pixi.lock itself"
    echo "                (Dependabot / the GitHub dependency graph do not read it)"
  fi
}

missing=()
for tool in syft grype jq; do
  command -v "$tool" >/dev/null 2>&1 || missing+=("$tool")
done
if [[ ${#missing[@]} -gt 0 ]]; then
  for s in "${scopes[@]}"; do coverage "$s" "none (missing: ${missing[*]})"; done
  echo "audit: NO-OP -- ${missing[*]} not on PATH; nothing was scanned. Install them yourself (they are not installed by this task) or rely on the scheduled CI audit job."
  exit 0
fi
if [[ ! -d $target ]]; then
  echo "audit: $target does not exist -- run 'pixi install' first" >&2
  exit 1
fi
if [[ ! -f $cpe_overrides ]]; then
  echo "audit: $cpe_overrides is missing" >&2
  exit 1
fi
if [[ ! -f $list ]]; then
  echo "audit: $list is missing -- create it with 'nice pixi run binary-packages --update'" >&2
  exit 1
fi

syft_v=$(syft version 2>/dev/null | sed -n 's/^Version:[[:space:]]*//p' | head -n 1)
grype_v=$(grype version 2>/dev/null | sed -n 's/^Version:[[:space:]]*//p' | head -n 1)

mkdir -p "$out_dir" || exit 1
names="$out_dir/binary-packages.names"
grep -v -e '^[[:space:]]*#' -e '^[[:space:]]*$' "$list" | LC_ALL=C sort -u >"$names"
if [[ ! -s $names ]]; then
  echo "audit: $list names no packages" >&2
  exit 1
fi

syft "$target" --select-catalogers=+conda-meta-cataloger --output "syft-json=$sbom" || {
  echo "audit: syft failed" >&2
  exit 1
}

# A listed package the SBOM does not contain as a conda package would silently drop out of the
# binary audit.
absent=$(jq -r --rawfile l "$names" '
  ($l | split("\n") | map(select(length > 0))) - [.artifacts[] | select(.type == "conda") | .name] | .[]' "$sbom") || {
  echo "audit: could not read $sbom" >&2
  exit 1
}
if [[ -n $absent ]]; then
  echo "audit: $list names packages syft did not find in $target as conda packages (stale list or env?):" >&2
  sed 's/^/  /' <<<"$absent" >&2
  exit 1
fi
# The split routes by name. syft's other catalogers (e.g. binary-classifier) add artifacts named
# after what they recognised; one whose name matches no conda package could belong to a shipped
# package's files yet land in the dev half, so refuse to guess.
orphans=$(jq -r '
  [.artifacts[] | select(.type == "conda") | .name] as $conda
  | [.artifacts[] | select(.type != "conda") | select(.name as $n | $conda | index($n) == null)
     | "\(.type) \(.name) \(.version) (\([.locations[]?.path] | join(", ")))"] | unique | .[]' "$sbom") || {
  echo "audit: could not read $sbom" >&2
  exit 1
}
if [[ -n $orphans ]]; then
  echo "audit: syft found non-conda artifacts whose names match no conda package, so the binary/dev split cannot route them; assign them deliberately (scripts/audit.sh):" >&2
  sed 's/^/  /' <<<"$orphans" >&2
  exit 1
fi

# Split by package name. Relationships that touch an artifact of the other half are dropped too
# (grype warns about every dangling one); those between files and kept artifacts stay.
for s in binary dev; do
  [[ $s == binary ]] && keep=true || keep=false
  jq --rawfile l "$names" --argjson keep "$keep" '
    ($l | split("\n") | map(select(length > 0))) as $bin
    | def kept: (.name as $n | $bin | index($n) != null) == $keep;
      ([.artifacts[] | select(kept | not) | .id]) as $drop
    | .artifacts |= map(select(kept))
    | .artifactRelationships |= map(select(
        (.parent as $p | $drop | index($p)) == null and (.child as $c | $drop | index($c)) == null))' \
    "$sbom" >"$out_dir/sbom-$s.json" || {
    echo "audit: could not split $sbom" >&2
    exit 1
  }
done

# Give the binary half real CPEs where the conda name maps to none (audit/cpe-overrides.json says
# why each one is, or is not, mapped). Every override must name a conda package in the binary scope, so
# a renamed or dropped package cannot silently fall back to the unmatchable syft-generated CPEs.
unhit=$(jq -r --slurpfile o "$cpe_overrides" '
  ($o[0].overrides | keys) - [.artifacts[] | select(.type == "conda") | .name] | .[]' "$out_dir/sbom-binary.json") || {
  echo "audit: could not read $cpe_overrides" >&2
  exit 1
}
if [[ -n $unhit ]]; then
  echo "audit: $cpe_overrides maps packages that are not conda packages in the binary scope ($list):" >&2
  sed 's/^/  /' <<<"$unhit" >&2
  exit 1
fi
jq --slurpfile o "$cpe_overrides" '
  $o[0].overrides as $map
  | .artifacts |= map(. as $a
      | if $a.type == "conda" and $map[$a.name] != null
        then .cpes = ((.cpes // []) + [{cpe: ($map[$a.name] | gsub("\\{version\\}"; $a.version)), source: "declared"}])
        else . end)' \
  "$out_dir/sbom-binary.json" >"$out_dir/sbom-binary.json.tmp" &&
  mv -f "$out_dir/sbom-binary.json.tmp" "$out_dir/sbom-binary.json" || {
  echo "audit: could not apply $cpe_overrides" >&2
  exit 1
}

# --fail-on makes grype exit nonzero on findings; without it grype exits 0 regardless.
# Verify the flag against the installed grype (`grype --help`) -- it is checked here too.
# Capture first: under pipefail, `grype --help | grep -q` can report grype's SIGPIPE when grep exits early.
grype_help=$(grype --help 2>&1 || true)
have_fail_on=0
grep -q -- '--fail-on' <<<"$grype_help" && have_fail_on=1

overall=0
for s in "${scopes[@]}"; do
  sev=$(threshold "$s")
  n=$(jq '.artifacts | length' "$out_dir/sbom-$s.json")
  echo
  coverage "$s" "syft ${syft_v:-?}, grype ${grype_v:-?} (fail on >= $sev; $n packages)"
  if [[ $have_fail_on -eq 1 ]]; then
    grype -c "audit/grype-$s.yaml" "$out_dir/sbom-$s.json" --fail-on "$sev"
    status=$?
  else
    echo "audit: WARNING: this grype has no --fail-on; findings are listed but cannot fail the task" >&2
    grype -c "audit/grype-$s.yaml" "$out_dir/sbom-$s.json"
    status=$?
  fi
  echo "audit ($s): grype exit $status"
  [[ $status -eq 0 || $overall -ne 0 ]] || overall=$status
done
echo "audit: SBOMs kept at $sbom and $out_dir/sbom-{binary,dev}.json"
exit "$overall"
