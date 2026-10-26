# Fact-check report: `mdlist` spec and plan

Merged verdicts per cluster. Spec: `list-spec.md` (V1–V6); plan: `list-plan.md` (V7–V25). Each section lists the verdicts and any plan amendments. Probe scratch lives under `.platform-runs/` (gitignored). Checked 2026-10-26.

**Summary:**
- **Nothing is refuted.**
- Open: V1 (private lists, inferred 404) and V11 (CDN purge on list edit) need a live check: AC32 and AC35, human.
- One design decision surfaced: C1/DEL bytes in `--json` (cluster B, V17b).

## Spec V1–V6: MangaDex API and website (live, about 45 anonymous requests)

| Item | Verdict | Notes |
|---|---|---|
| V1 private list asked for anonymously | UNRESOLVED | Strong indirect evidence for 404: the spec lists only 200/404, and the site's frontend retries with auth only on 404. A nonexistent id gives 404 `not_found_http_exception`. A ban is a generic 403. |
| V2 `relationships` complete | CONFIRMED up to 121 titles | Comes back in one response. `limit`/`offset` are ignored, and there is no documented cap. |
| V3 relationship types | CONFIRMED | Only `manga` and one `user` (last); entries are `{id,type}`. |
| V4 order | CONFIRMED (site source) | The order is the website's order. The site hides titles outside the viewer's content filters; the API doesn't. |
| V5 URL forms, "MDList" | CONFIRMED | `/list/<id>/<slug>`, and an empty slug gives `/list/<id>/`. |
| V6 query, case, sample | CONFIRMED | An upper-case id is a router 404, so the URL must use the lowercase id. `includes[]` only bloats the response. The CDN sends `s-maxage=2592000` (V11 open). |

## Cluster A: CLI11 2.7.2 (V7a–V7d): all CONFIRMED

The probe was a mini app with root `fallthrough()` and three siblings (`download`, `batch`, `mdlist`), using verbatim copies of the shared binders. Source: `.platform-runs/factcheck-cliA/probe_mdlist.cpp`. It was compiled unsanitized, which is enough for a parse-only check.

| Claim | Verdict | Evidence |
|---|---|---|
| V7a: binders on a third sibling; options block = batch's + one line | CONFIRMED | No ConstructionError. `mdlist`'s block is 70 bytes longer than `batch`'s: exactly the `--dir-per-title` line. The order is `… --fail-fast, --dir-per-title`, then the root globals (as in batch). `--api-base` is hidden. |
| V7b: root flags after both positionals | CONFIRMED | `mdlist L out --json -q -v --color never --dir-per-title` binds every flag, with LIST=L and DIR=out. The `-vv`, `-j 3` and `-n` variants also work. |
| V7c: `--title X` / `--title=X` → ExtrasError, reached `mdlist` | CONFIRMED | `remaining(true)` is `[--title]` or `[--title=X]`; `mdlist --title X L` binds X as LIST. |
| V7d: missing LIST → RequiredError, hint names mdlist, exit 2 | CONFIRMED | Code 106 is mapped to 2 by `cli.cpp:180-181`. The hint reads `Run 'mangadex-dl mdlist --help'`. |

Plan impact:
- T11's help assertion must say "`--dir-per-title` is last of the subcommand's own options", because the root globals follow it.
- Column wrapping under the real `configure_help` formatter is left to the T11 `--help` test.

## Clusters B + C: nlohmann_json 3.12.0 and directory names (V17, V18, V12, V13): all CONFIRMED, one caveat

Method:
- Cluster B was checked with sanitized `cxx-probe` runs mirroring the repo's exact call shapes (`json_util.cpp:147`, `batch_json.cpp:152-172`). Probes: `.platform-runs/factcheck-bc/probe_json.cpp`.
- V12 was checked with an exhaustive sweep of U+0000–U+10FFFF through a verbatim copy of `classify` (`probe_classify.cpp`).
- V13 was checked by code reading only.

| Claim | Verdict | Notes |
|---|---|---|
| V17a `ordered_json` key order `source, list, titles` | CONFIRMED | `list` is `{id, name, url}`, in that order. |
| V17b ESC dumps as `\u001b` | CONFIRMED, with a caveat | `dump(ensure_ascii=false)` escapes only < U+0020. **DEL (0x7F) and C1 controls (e.g. U+009B, the 8-bit CSI) come out raw.** This already happens today for every string in `batch --json` and `download --json`. |
| V17c invalid UTF-8 → U+FFFD with `replace` | CONFIRMED | It works at nested depth and never throws. |
| V17d `nullptr` → `null` | CONFIRMED | |
| V18 the non-throwing parse rejects invalid UTF-8 in strings | CONFIRMED | Overlongs, surrogates, > U+10FFFF, truncations and lone `\ud800` escapes are all rejected. So a list document with bad UTF-8 is malformed (exit 3), and the name is never "repaired". |
| V12 every `sanitize_file_stem` output passes `validate_list_dir` as one segment, including the suffixes (≤189 bytes) | CONFIRMED | The sweep found 0 survivors among the 97 code points flagged as unsafe. `.`/`..`/empty fall back; the segment limit is 255. |
| V13a `OutputDir::open(DIR/<name>, create)` creates both levels at 0755 & umask | CONFIRMED (code) | The citation should read `output_dir.cpp:415-454`. |
| V13b `open_if_exists(DIR/<name>)` gives nullopt when DIR is missing | CONFIRMED (code) | `:706-719`. |
| V13c `check_row_dir(DIR, name)` is ok when DIR is missing | CONFIRMED (code) | `row_dir.cpp:111-116`. An unwritable DIR is a global error. |

Plan impact:
- Drop the V18 refactor row and the extra AC3 row. Add an AC3 test that invalid UTF-8 or a lone surrogate anywhere in the document gives exit 3.
- Drop the "re-sanitise" refactor row for V12.
- T6 may call `open_if_exists(DIR/<name>)` directly in dry runs.
- **Open decision (C1/DEL in `--json`):** narrow the spec's "never ANSI" claim to C0/ESC, or escape C1 and DEL. See the plan gate.

## Clusters D + G: repo transport, API client, fixtures, E2E driver (V8b, V8c, V9, V10, V16, V19–V23): all CONFIRMED

Evidence: code citations, the dev build plus the binary run against generated fixture trees (`.platform-runs/factcheck-dg/`), and `e2e.batch.(row-dir|excel|row-dir-is-file|tty-progress|dir-is-file)`, all 8 passing. `script` is present, so the TTY cases run.

| Claim | Verdict | Notes |
|---|---|---|
| V8b `with_remedy` keys on exit 3 only | CONFIRMED | `cmd_download_run.cpp:87-92`. `get_json` prefixes `<url>: ` (`api_client.cpp:523-526`). |
| V8c empty `sub_dir` gives a DIR-only pre-flight | CONFIRMED | `cmd_batch.cpp:181-182`, `row_dir.cpp:110-152`. The citation span is `batch_plan.cpp:186-203`. |
| V9 `file://<root>/api/list/<id>` maps to `api/list/<id>.json` | CONFIRMED | `fixture_transport.cpp:145-155`. |
| V10 rate limit at the list/item-1 boundary | CONFIRMED | A fresh limiter per client means a 0–250 ms gap at the boundary, the same as batch's title boundaries today. The README wording "shared by the whole run" (line 566) is already loose. |
| V16 missing twin cover images warn | CONFIRMED, in a different place | The warning comes from the **execute** phase, once per EPUB, in real runs only (`execute_phase.cpp:455-476`). Dry runs are silent. With the images copied, stderr is empty and the exit is 0. |
| V19 a missing fixture document is HTTP 404 | CONFIRMED | Also run against an empty root: exit 4. |
| V20 401/403 answered once with their body | CONFIRMED | `api_client.cpp:365-371`, `retry.cpp:109-146`. `get_body` drops the body (`:507-509`). |
| V21 FakeTransport hooks | CONFIRMED | `fake_transport.hpp:41-49`, `:61`, `:78`, `:143-156`. |
| V22 spaces in paths; `{{INPUTS}}` expansion; a committed file as DIR | CONFIRMED | Bracket quoting is at `CliE2E.cmake:284-291`. Paths can't contain `[`/`]`, so `Clean Title [77777777]` stays at the cli tier. |
| V23 TTY `(1/2)` progress; `NO_COLOR` | CONFIRMED | By code, and by the passing `e2e.batch.tty-progress`. No `EXPECT_STDERR_*` in TTY cases. |

Plan edits (none blocking):
- Spec issue 10 and V16: say "execute phase, per EPUB, real runs only". Keep the twin images, and drop the refactor-trigger row about tolerating the warnings.
- V20: fix the citation to `retry.cpp:109-146`. T7's `fetch_list_document` must use `State::perform`, not `get_body`.
- V10/T13: add no mdlist-specific rate claim. Reuse "apply to each title in turn", or qualify README:566.
- V8c: widen the citation.

## Cluster E: compilers and build surface (V14, V15, V24, V25): all CONFIRMED

| Claim | Verdict | Notes |
|---|---|---|
| V14 a new `Errc::row_dir` is caught by `-Wswitch` under `-Werror` in clang 23.1.3 and gcc 16.2.0 | CONFIRMED | Probes fail the build with exit 125 in both envs. `switch` is not among the six gcc `-Wno-error=` demotions. The only two `Errc` switches are `exit_code_for` (`error.hpp:28-51`) and `is_fatal` (`cmd_batch.cpp:139-154`), and neither has a `default:` label. A `constexpr` `static_assert(exit_code_for(Errc::row_dir) == 1)` compiles in both. |
| V15 the additive members are safe | CONFIRMED | There are no structured bindings, positional init or member-iterating serialization on these types. `mark_duplicates` now also compares `dir_per_title`, which is the same for every job in one run. |
| V24 `nice pixi run tsan -R '<regex>'` forwards `-R` | CONFIRMED | Ran 5 tests. **The regex must match the real ctest names, `unit.<TEST_CASE text>` / `cli.<TEST_CASE text>`.** A regex that matches nothing exits 8 with "No tests were found". |
| V25 the globs pick up every planned file | CONFIRMED | Core recursive, `src/app/cmd_*.cpp` flat, `tests/unit` recursive, `tests/cli` flat, `e2e/cases/*.cmake`. The unit target includes `src/core`, so it can reach `mangadex_url.hpp`. |

Plan impact:
- T3 must carry the `row_dir` case into the moved `is_fatal`, keeping it exhaustive with no `default:`.
- Tasks using `tsan -R` must check the reported test count.

## Plan amendments (apply on top of `list-plan.md`)

1. **Spec issue 10 / V16:** the missing-cover warning comes from the execute phase, once per EPUB, in real runs only. Keep the twin cover images, and drop the "tolerate warnings" refactor-trigger row.
2. **T7:** `fetch_list_document` uses `State::perform` directly (not `get_body`, which drops the 401/403 body). Fix the citation to `retry.cpp:109-146`.
3. **T7 / AC3:** drop the V18 "repair the name" refactor row and the extra AC3 row. Add a test that invalid UTF-8 or a lone surrogate anywhere in the list document gives exit 3.
4. **T5 / V12:** drop the "re-sanitise" refactor row. The AC36 property test stays.
5. **T6:** in a dry run, `open_if_exists(DIR/<name>)` may be called directly. It gives nullopt when either level is missing.
6. **T3:** the moved `is_fatal` keeps the `row_dir` case and stays exhaustive with no `default:`.
7. **TSan runs:** `-R` regexes match `unit.<TEST_CASE text>` / `cli.<TEST_CASE text>`. Check the test count in the output.
8. **T11:** assert "`--dir-per-title` is last among the subcommand's own options". The root globals still follow it.
9. **T13 / V10:** add no mdlist-specific rate-limit claim. Reuse "apply to each title in turn", or qualify README line 566.
10. **Citations:** `batch_plan.cpp:186-203` (V8c), `output_dir.cpp:415-454` (V13a).
11. **C1/DEL in `--json` (V17b):** decided at the plan gate: out of scope here; a separate change escapes DEL and U+0080–U+009F in every `--json` document. T5 asserts only C0/ESC escaping.
