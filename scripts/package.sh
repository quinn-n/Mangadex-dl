#!/usr/bin/env bash
# package.sh -- build the release tarball with `cpack --preset release`.
#
# The `package` task depends on release-test, so the release build is configured, built
# and tested first. Refuses: any env other than `default` (the gcc wrapper always adds an
# rpath into the env, so only the clang env's release binary is shippable) and any
# sanitized or coverage-instrumented build dir. Output: build/default/release/
# mangadex-dl-<version>-linux-<arch>.tar.gz plus its .sha256 (CPACK_PACKAGE_CHECKSUM).
# Publishing is CI's job (tag-triggered release workflow), never an agent's.
set -uo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P) || exit 1
cd "$root" || exit 1

env_name=${PIXI_ENVIRONMENT_NAME:-}
if [[ $env_name != default ]]; then
  echo "package: refused in env '${env_name:-<none>}' -- release artifacts are built in the default (clang) env only: 'pixi run package'" >&2
  exit 1
fi
cache="build/$env_name/release/CMakeCache.txt"
if [[ ! -f $cache ]]; then
  echo "package: $cache is missing -- 'pixi run package' configures and tests the release build first" >&2
  exit 1
fi
if grep -Eq '^MANGADEX_DL_SANITIZE:[A-Z]*=.+' "$cache"; then
  echo "package: refused -- $cache has MANGADEX_DL_SANITIZE set; a sanitized build is never shipped" >&2
  exit 1
fi
if grep -Eq '^CMAKE_CXX_FLAGS:[A-Z]*=.*-f(profile-instr-generate|coverage-mapping)' "$cache"; then
  echo "package: refused -- $cache is coverage-instrumented" >&2
  exit 1
fi
if ! grep -Eq '^MANGADEX_DL_HARDENING:[A-Z]*=(ON|1|TRUE|YES)$' "$cache"; then
  echo "package: refused -- $cache was configured without MANGADEX_DL_HARDENING=ON (not the release preset?)" >&2
  exit 1
fi

# Drop archives from earlier versions/runs so only this run's output is reported/uploaded.
rm -f build/"$env_name"/release/mangadex-dl-*.tar.gz build/"$env_name"/release/mangadex-dl-*.tar.gz.sha256
bash scripts/preset.sh package release || exit $?

shopt -s nullglob
made=(build/"$env_name"/release/mangadex-dl-*.tar.gz)
if [[ ${#made[@]} -eq 0 ]]; then
  echo "package: cpack reported success but no .tar.gz is in build/$env_name/release/" >&2
  exit 1
fi
echo "package: artifacts:"
for f in "${made[@]}" build/"$env_name"/release/mangadex-dl-*.tar.gz.sha256; do
  echo "  $f"
done
