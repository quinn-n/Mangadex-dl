# `download` feature: pipeline status

- Design gate: APPROVED (download-spec.md, incl. errata). Plan gate: APPROVED (download-plan.md + download-plan-rev1.md, which wins on conflict). Fact-check: download-factcheck.md.
- **Batch 1 (T0–T3): integrated.** Rounds: T0 1, T1 2, T2 2, T3 2.
- **Batch 2 (T4–T8): integrated 2026-10-18.** Rounds: T4 2, T5 2, T6 2, T7 2, T8 3. Integration edits: unit tests get `src/core` include path + curl headers; model.hpp titles-order comment; zip.hpp add_stored precondition. Gate: `check` 296/296 + lint clean, `-e gcc check` 296/296, `tsan` 288/288.
- **Batch 3 (T9–T13): integrated 2026-10-18.** Rounds: T9 2, T10 1, T11 2 (+security), T12 2, T13 2. build-fixer: release fixture_gen BUILD_RPATH (tests/CMakeLists.txt). Header doc edits: filename.hpp, net/transport.hpp, net/curl_transport.hpp, epub.hpp. Gate: check 446/446, -e gcc check 446/446, release-test 439/439 (both envs), tsan 438/438.
- **Batch 4 (T14–T16): integrated 2026-10-18.** Rounds: T14 2, T15 2, T16 3. Gate: check 471/471 + lint clean, -e gcc check 471/471, release-test 464/464, tsan 463/463.
- **Batch 5 (T17): integrated 2026-10-18.** Round 1 (both approved; quality signed off gate decision 7 conditional on T18b's stderr-color cases). Human approved and the orchestrator ran `pixi run bless '^e2e\.edge\.help$'` and `git rm tests/e2e/golden/hello.default.stdout`. Gate: check 460/460, -e gcc check 460/460, release-test 453/453.
- **Batch 6 (T18a, T18b): integrated 2026-10-18.** Round 1 each (T18b quality-only, CMake). Human approved and the orchestrator blessed `download.dry-run-clean` + `download.json-dry-run`; `download.help` deliberately left unblessed until the cli-ux board settles the help layout. Gate: dev 512/513, gcc 512/513, tsan 504/505 (only e2e.download.help), lint clean, both release builds green.
- **Verification:** cli-tester ✅ (download-clitest.md), platform-tester ✅ (download-platform.md), cli-ux-reviewer APPROVED + fix round (help layout, error format, messages) and core fix round (exit-4 text, dry-run replaced, dedupe labels, D2, D3) — both reviewed APPROVED; tests updated; human blessed `e2e.edge.help` + `e2e.download.help`. `check` green 523/523.
- **Board: ALL APPROVED 2026-10-18** — security (audit no-op: syft/grype absent; 3 Lows), final quality (check + -e gcc check 523/523, release-test 516/516), final memory-safety (dev 523/523, tsan 515/515 both compilers, all controls fired), cli-ux (approved + fixes landed).
- **Feature complete; nothing committed.**
- Next: batch 4 (T14 orchestration, T15 command, T16 README) → batch 5 (T17 registration, hello removal, edge knobs; golden re-bless gate) → batch 6 (T18a CLI tests, T18b E2E + deferred driver self-test; golden bless gate) → cli-tester → platform-tester → board.
- Follow-ups noted: hoist T4's ExactBuffer and T8's mask helpers into tests/support; element-count pre-scan for JSON (security board).
- Nothing committed yet.
