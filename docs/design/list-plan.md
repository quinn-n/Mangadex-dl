# Implementation plan: `mangadex-dl mdlist LIST [DIR]`, every title in a public MangaDex list (MDList)

Status: **APPROVED 2026-10-26** at the ⏸ plan gate, after fact-check ([`list-factcheck.md`](list-factcheck.md): nothing refuted; its "Plan amendments" 1–10 apply on top of this plan and take precedence where they differ).

**Plan-gate decisions (user, 2026-10-26):**
- Spec issues 2, 4 and 6 are accepted as written: an exit-5 item claims no directory; a malformed relationship entry makes the list malformed (exit 3); the AC46 E2E case is a dry run, with the real-run variant at the cli tier in a `TempDir`.
- C1/DEL bytes in `--json` (fact-check V17b): **out of scope here.** A separate change will escape DEL and U+0080–U+009F in every `--json` document; `mdlist` inherits it. In this change, `list.name` follows the existing document rules, and T5's tests assert only C0/ESC escaping (never that C1 or DEL are escaped).
  - Resolved by json-c1 (`docs/design/json-c1-plan.md`): DEL and C1 are now escaped in every stdout `--json` document, `mdlist`'s included.

Design: `/home/user/Mangadex-dl/docs/design/list-spec.md` (rev1, amendment A1). The gate decisions are binding:
- Q1: the name is `mdlist`.
- Q2: reuse exit 4; no new code.
- Q3: flat by default, plus an opt-in `--dir-per-title`.
- QA1: plain `<stem>/` directories, with ` [<8 hex>]` only on a same-name clash within one run.
- QA2: no `--dir-per-title` on `batch` in this change.

Prior plans used for format and depth: `docs/design/batch-plan.md`, `docs/design/download-plan.md` and `docs/design/skip-existing-plan.md`.

What I checked in the tree on 2026-10-26 (orientation only; the fact-checkers re-confirm the [VERIFY] items):
- `pixi list`: cli11 2.7.2, nlohmann_json 3.12.0, catch2 3.16.1, cmake 4.4.4, ninja 1.13.2, clang 23.1.3, gxx 16.2.0, libstdcxx 16.2.0. **No new dependency is needed.**
- CMake globs:
  - `CMakeLists.txt:90` is `GLOB_RECURSE src/core/*.cpp`, so `src/core/download/*.cpp` is picked up.
  - `CMakeLists.txt:108` is `GLOB src/app/cmd_*.cpp`: flat, and **only the `cmd_` prefix**.
  - `tests/unit/CMakeLists.txt:7` is `GLOB_RECURSE`.
  - `tests/cli/CMakeLists.txt:12` is a **flat** `GLOB *.cpp`, so CLI tests must sit directly in `tests/cli/`.
  - `tests/CMakeLists.txt:57` globs `e2e/cases/*.cmake`.
- No task edits a CMake file, a preset, `pixi.toml`, `scripts/*` or a workflow.

---

## Spec issues (read first)

Each item says plainly where the spec is wrong, infeasible against the code, or under-specified, and what this plan does instead. Items 1, 2 and 4 need the gate's attention.

1. **The remedy rule can't be met with the suggested `ApiClient::fetch_custom_list(...) -> list`.**
   - The spec says the over-cap message (and, per V8, the malformed-list messages) must not get the network remedy. But `with_remedy` (`src/app/cmd_download_run.cpp:87`) keys **only on exit code 3**, and every list content error is `Errc::network` (exit 3). Also, `ApiClient::State::get_json` prefixes every parse error with `<url>: ` (`api_client.cpp:521-526`), which the spec's cap message doesn't have.
   - **Plan:** split the two steps.
     - The member `ApiClient::fetch_list_document(const ListId&, stop) -> Result<std::string>` returns the body only.
     - A pure `parse_custom_list(json, expected)` does the parsing.
     - A core orchestrator `fetch_custom_list(...)` returns `std::expected<CustomList, ListFetchError>`, where `ListFetchError{kind = request | content, error}`.
   - The app appends `with_remedy` only to `request` errors.
   - **Content messages are self-contained.** They name the list id, never the URL, never response bytes: `list <id>: malformed MangaDex API response (custom list): …` and `list <id> has more than 10000 titles; refusing to truncate it; split it into smaller lists`.
   - AC5's "200 → the list" holds for the free function; the member returns the body.
2. **Exit 5 with `--dir-per-title`.** The spec's `titles[].dir` rule ("DIR when the name was never learned (exit 3/4 before its plan, duplicate, not_run)") implies an exit-5 item resolves and claims its directory name.
   - **Plan (needs a gate nod):** resolve **only when the plan has at least one file**. An exit-5 item claims no name, prints no `output directory:` line, runs no pre-flight, and reports `dir` = DIR.
   - Why: an empty-plan title should not push a later same-name title onto ` [<8 hex>]`, and DIR is left alone for exit 5 anyway (`execute_phase.cpp:985-987`).
   - The alternative (resolve always) is a two-line change in T6 and T9.
3. **AC12 "a simulated interrupt before item 2" can't be injected at the `[cli]` tier.** `run_mdlist_command` builds its own fixture backend, and there is no hook between items.
   - Batch had the same limit: its AC10 used a FakeRunner at the unit tier, plus "flag set before the call" in-process.
   - **Plan:**
     - unit, in T5: the `run_batch` FakeRunner case with `BatchSource::mdlist`;
     - unit, in T7: an interrupt during the list fetch through FakeTransport's `set_on_fetch` hook;
     - cli, in T10: the flag set before the call, which takes the interrupted list-fetch path.
4. **AC46 runs a real download with DIR inside the source checkout** (`{{INPUTS}}/mdlist`). A regression that let item 1 through would write EPUBs into `tests/e2e/inputs/`.
   - **Plan:** the E2E case adds `--dry-run`. It gives the same exit 7 and the same item-1 line, because the engine's pre-flight also runs in dry runs (spec "Per-title subdirectories"), and nothing can be created.
   - The real-run variant is a `[cli]` case in a `TempDir` (T10).
5. **Placement, not a defect.** The spec puts post-plan resolution in `execute_phase.cpp`. The plan resolves in `run_download` (`src/core/download.cpp`) through a new `src/core/download/dir_per_title.cpp`. Then it calls the **unchanged** `execute_phase` with a copied `RunContext` whose `options` points at an effective copy (`dir = DIR/<name>`).
   - Every path in `execute_phase.cpp` already derives from `options.dir` (`output_path` at :110; `open_if_exists` at :953; `open` at :996), so that 1017-line file is untouched.
6. **Relationship entries that are not objects, or that lack a string `type`.** The spec only says "ignore non-manga types and unknown fields".
   - **Plan:** such an entry makes the document malformed (exit 3). An `id` is required only when `type == "manga"`.
   - This is conservative and consistent with `FieldReader`'s required-field rule.
7. **The `-v` per-item line.** Reusing batch's code with the mdlist prefix `list:<n>: <uuid>: ` would print `list:3: <uuid>: <uuid>`.
   - **Plan:** the line is built from label and position, `"{label}:{line}: {uuid}"`. That is byte-identical for batch and gives the spec's `list:3: <uuid>` for mdlist.
8. **AC4 "over 16 MiB" is reachable only in unit tests.** At run time the transport cap (`HttpRequest::max_body_bytes = max_api_json_bytes`) fires first, as a **request** error `<url>: …` **with** the network remedy. `parse_untrusted`'s own byte cap is the unit-tier guard.
9. **The registry's third candidate can collide.** `<stem> [<full uuid>]` can be held by another manga only if some title's own stem is literally that string. The spec is silent.
   - **Plan:** return it anyway. `OutputRegistry` stays the backstop (nothing is overwritten). Documented as unreachable in practice.
10. **AC41 `clean-twin` and the shared covers (V16).** The fixture map ignores `?manga[]=`, so `api/cover/offset-0.json` is shared (`mangadex_api.cpp:542`, `fixture_transport.cpp:145-155`). The planner doesn't filter covers by manga (`planner.cpp:373`).
    - The twin's cover images under `uploads/covers/7777…/` would be missing, which probably gives cover-download warnings.
    - **Plan:** the twin helper also writes the cover images under the twin's id, so AC41 asserts exact stderr. V16 stays, to confirm what happens without them.
11. **AC15 names `cli_test.cpp` and `download_cli_test.cpp`.** But `tests/cli/batch_parse_test.cpp` also pins the old `download` footer line (:247) and exit row 7 (:224), so T11 owns that file too.
12. **Names that differ from the spec's suggestions:**

    | Spec | Plan | Why |
    |---|---|---|
    | `ApiClient::fetch_custom_list` | `ApiClient::fetch_list_document` | issue 1; the free function takes the name |
    | `plan_list(const CustomList&, const BatchSettings&, bool)` | `plan_list(const CustomList&, const MdlistSettings&)` | the settings carry the flag |
    | `mdlist.hpp` holds `ListId` | `ListId` in its own light `list_id.hpp` | `api_client.hpp` must include it without pulling in `batch.hpp` |
    | `title_dir.hpp` | kept | |
    | `cmd_batch_jobs.{hpp,cpp}` | kept | |
13. **Who prints the directory-clash warning.** The spec has the app's resolver print it "with the item prefix". In the plan, the resolver **returns** the warning text, and the engine emits it through `observer.on_warning`. That already adds `warning: <prefix>`, honours `-q` and clears any progress line. The bytes are identical, and every stderr line keeps a single path.

---

## Approach

A list is a batch without a CSV: core turns `GET /list/{id}` into a validated `CustomList`, `plan_list` turns it into ordinary `BatchJob`s, and the **unchanged** `run_batch` loop runs them. The app's batch phase 2 (the `ConsoleTitleRunner` and the closing, note and JSON steps) is extracted verbatim into `cmd_batch_jobs.cpp` and shared by `batch` and `mdlist`. `--dir-per-title` is an additive seam in `run_download`, guarded by a default-false `DownloadOptions::dir_per_title` and a borrowed nullable `TitleDirResolver*`.

**The key trade-off.** The plan front-loads two behaviour-preserving refactors (the URL walk in `manga_id.cpp`, and batch phase 2) plus additive frozen headers. Each refactor is proven byte-identical by the existing `batch`/`download` suites and goldens before any mdlist behaviour lands. The cost is two extra batches; the gain is that every later regression is attributable to new code. The riskiest edit, in the shipped `download` engine, stays behind a flag that `download` and `batch` never set.

---

## Dependencies & pixi/CMake changes

| Change | Where | Task |
|---|---|---|
| None. No `pixi add`, `find_package` or FetchContent | — | — |
| No preset, pixi task, CMake option, `scripts/*` or CI change | — | — |
| New core sources, picked up by the recursive glob: `src/core/mangadex_url.cpp`, `list_id.cpp`, `title_dir.cpp`, `mdlist.cpp`, `mdlist_fetch.cpp`, `mdlist_plan.cpp`, `download/dir_per_title.cpp` | glob | T2, T4, T5, T6, T7, T8 |
| New app sources, picked up by the `cmd_*.cpp` glob: `cmd_batch_jobs.cpp`, `cmd_mdlist.cpp` | glob | T3, T10 |
| New test sources: `tests/unit/**`, flat `tests/cli/*.cpp`, `tests/e2e/cases/mdlist.cmake` | glob | most tasks |
| Committed input `tests/e2e/inputs/mdlist/Clean Title` (a regular file; any content) | not CMake | T12 |

- **Build files for build-fixer, all tasks:** none beyond each task's Owns. A task that seems to need a shared CMake edit is a coordination event for the orchestrator.
- `tests/CMakeLists.txt`, `tests/e2e/CliE2E.cmake`, `tests/e2e/run_case.cmake` and `tests/e2e/fixtures/fixture_gen.cpp` are **not edited**: list documents ride on `write_fixture_tree` (T7).

---

## Resolved in-repo (evidence; the fact-checker only re-reads these)

- **V8b:** `with_remedy` appends the remedy iff `error.exit_code() == exit_code::network` (`cmd_download_run.cpp:87-92`). The plan therefore routes content errors around it (Spec issue 1).
- **V8c:** `ConsoleTitleRunner::run` calls `check_row_dir(defaults.dir, job.sub_dir)` (`cmd_batch.cpp:181-182`), and `sub_dir` is empty for a row without a `dir` cell (`batch_plan.cpp:187-195`). `plan_list` sets `sub_dir = ""`, so mdlist gets exactly the DIR-only pre-flight.
- **V9:**
  - `map_url` (`fixture_transport.cpp:145-155`) appends `.json` to the last component under `api/` when there is no `offset` query.
  - `endpoints_for(fixture).api` is `file://<root>/api` (`api_client.cpp:308-315`).
  - So `file://<root>/api/list/<id>` reads `<root>/api/list/<id>.json`. T7 pins this with a test.
- **V14:** the only exhaustive `switch (Errc)` blocks are `exit_code_for` (`error.hpp:25-45`) and `is_fatal` (`cmd_batch.cpp:139-154`). `batch_run.cpp` has no Errc switch. T1 updates both.
- **V19 (new):** a missing fixture document is HTTP 404, not a transport error: ENOENT/ENOTDIR/ELOOP count as "not in tree" and give `status_only(404)` (`fixture_transport.cpp:55-58`, `:191`, `:198`). So the unknown list is exit 4 in fixture mode.
- **V20 (new):**
  - `State::perform` returns the last non-2xx response with its body (`api_client.cpp:366-371`).
  - `decide_retry` retries only transport errors, 429 and 5xx (`retry.cpp:115-135`), so a 401 or 403 is answered once.
  - The 401/403 fallback can therefore inspect the body at the cost of one request.
- **V21 (new):** `FakeTransport` offers `push_status(url, status, body)`, `set_on_fetch(hook)`, and a `RecordedRequest` with `url` and `max_body_bytes` (`tests/support/fake_transport.hpp:41-80`). That covers AC5, including the body-cap assertion and an interrupt mid-fetch.
- **Stem source:** `make_epubs` computes `stem = sanitize_file_stem(title, manga_id)`, with `title == plan.manga.title` and `manga_id == plan.manga.id` (`planner.cpp:712`, `:792-796`). `Plan::file_stem` (T1/T6) is filled from the same call: one source of truth.
- **Covers aren't keyed by manga** in fixture mode or in the planner (Spec issue 10).
- **No test compares a whole `Plan` or `RowOutcome` positionally:** grep found no `Plan{…}` literals in tests. Additive last members with DMIs keep every designated initializer valid.

---

## Assumptions to verify

V1–V6 are already fact-checked (spec "[VERIFY] items"); V1 is still unresolved. V7–V16 are carried forward, and V17 onward are new.

**Cluster A: CLI11 (installed 2.7.2).** Blocks T10 (binding) and T11 (`cli.cpp`). Probe note from earlier plans: `scripts/cxx-probe.sh` adds no `-I` for the env include dir, so use `$CONDA_PREFIX/include` or `CPATH`, or a scratch tier-2 test.
- **V7a** `bind_selection_options` and `bind_run_options` register on a **third** sibling subcommand without `CLI::ConstructionError`. `mdlist --help` lists its options in add order (positionals, then the shared options, `--fail-fast`, and `--dir-per-title` last), so the options block is batch's byte for byte plus one line.
- **V7b** With root `fallthrough()` set before `add_subcommand`, root flags written after both positionals are parsed by the root, and LIST/DIR still bind. Example: `mdlist L out --json -q -v --color never --dir-per-title`.
- **V7c** `mdlist L --title X` and `mdlist L --title=X` raise an `ExtrasError` at the root. When that happens, `app.get_subcommands().front()->get_name() == "mdlist"` and `app.remaining(true)` contains `--title` (or `--title=X`), as V-A4 showed for `batch`.
- **V7d** `mdlist` with no LIST is a `RequiredError`. `help_hint` then names `mdlist` (`Run 'mangadex-dl mdlist --help' …`), and exit 2 is never 100–114.

**Cluster B: nlohmann_json (installed 3.12.0).** Blocks T5 (JSON) and T7 (parse, name cut).
- **V17** An `ordered_json` built as `doc["source"]`, then `doc["list"] = {id, name, url}`, then `doc["titles"]` keeps that key order. Dumped with `dump(2, ' ', false, error_handler_t::replace)`:
  - a `name` that holds U+001B is emitted as `\u001b` (never a raw ESC byte);
  - invalid UTF-8 in a nested `list.name` becomes U+FFFD with no throw;
  - `nullptr` dumps as `null`.
- **V18** The strict non-throwing `json::parse(…, nullptr, false)` used by `parse_untrusted` rejects invalid UTF-8 inside string values. So `attributes.name` is valid UTF-8 after parsing, and the 1024-byte cut only has to back off continuation bytes.

**Cluster C: repo, directory names and directories.** Blocks T5 (the AC36 property test) and T6.
- **V12** Every output of `sanitize_file_stem` (≤150 bytes; never empty, `.` or `..`) passes `validate_list_dir` as **one** segment and is accepted by `check_row_dir(DIR, name)`. So does `<stem> [<8 hex>]` and `<stem> [<uuid>]` (≤189 bytes).
  - A first reading supports this: `classify` (`filename.cpp:103-149`) replaces C0, DEL, C1 and the separators, and drops every code point in `is_unsafe_code_point`'s set (U+061C, U+200B–U+200F, U+202A–U+202E, U+2028/9, U+2060–U+206F, U+FEFF).
  - Confirm that no code point `is_unsafe_code_point` flags survives `classify`.
- **V13** `OutputDir::open(DIR/<name>, create=true)` creates both DIR and `<name>` when both are missing: mkdir -p per component at 0755, subject to umask (`output_dir.cpp:415-442`). `OutputDir::open_if_exists(DIR/<name>)` returns nullopt, not an error, when DIR itself is missing (the dry-run path). `check_row_dir(DIR, name)` is `ok` when DIR is missing (ENOENT at the root).

**Cluster D: repo, transport, API client, fixtures and remedy.** Blocks T7 (V8b, V9, V19, V20, V21), T9 and T10 (V16), and the README rate sentence in T13 (V10).
- **V8** (a) The phase-2 extraction keeps `batch` byte-identical. This is T3's Done-when, not a fact-check. (b) and (c) are resolved in-repo; re-read them.
- **V9** The list URL maps to `api/list/<id>.json`; resolved in-repo, pinned by a T7 test.
- **V10** The list fetch's `ApiClient` (its own 250 ms limiter, destroyed before item 1), plus one client per item, sends at most one request sooner than 250 ms after another at the list/item-1 boundary. That is consistent with the README's "≤4 req/s" wording, or the wording needs a qualifier.
- **V16** For a second manga whose fixture tree shares `api/cover/offset-0.json` with clean, find out whether the plan phase warns when the cover **images** under `uploads/covers/<twin>/` are missing. The planner does not filter the listing by manga. This decides whether the twin helper must copy the images (the plan says yes, regardless).
- **V19** A missing fixture document → HTTP 404; resolved in-repo.
- **V20** 401/403 are answered once, with their body; resolved in-repo.
- **V21** FakeTransport supports the hooks AC5 needs; resolved in-repo.

**Cluster E: compilers and the build surface (repo).** Blocks T1 (V14), T3 and T6 (V15), T9 and T10 (V24).
- **V14** Adding `Errc::row_dir` is caught by `-Wswitch` under `-Werror` in **both** clang 23 and gcc 16 in exactly the two switches. `exit_code_for(Errc::row_dir) == 1` stays a constant expression (`static_assert`).
- **V15** The additive last members leave `download` and `batch` byte-identical (every golden and every CLI/E2E case unchanged): `DownloadOptions::dir_per_title`, `Plan::file_stem`, `DownloadServices::title_dir`, `BatchSettings::source` and `RowOutcome::dir`. No code destructures these aggregates with structured bindings.
- **V24** `nice pixi run tsan -R '<regex>'` forwards the filter: `scripts/preset.sh`'s whitelist for the `tsan` test kind accepts `-R`.
- **V25** The `CONFIGURE_DEPENDS` globs pick up every new file at its planned path. Resolved in-repo (paths above); listed so the orchestrator knows no CMake edit is planned.

**Cluster F: live MangaDex (human or live probe).** Blocks no code task. It feeds T7's refactor trigger (V1) and T13's caching sentence (V11).
- **V1** An anonymous `GET /list/{id}` for a private list answers 404, or 401/403 with a CustomList error body. Closed by AC32.
- **V11** Whether editing a list purges the CDN copy (`s-maxage=2592000`). Closed by AC35 or a live edit-then-GET probe.

**Cluster G: the E2E driver (repo).** Blocks T12.
- **V22** `EXPECT_FILES`, `EXPECT_EPUB` and `EXPECT_ABSENT` accept paths with spaces, such as `out/Clean Title/Clean Title - Vol. 01.epub` (precedent: batch's `out/Excel, Title - Vol. 01.epub`). `{{INPUTS}}/mdlist` expands in ARGS, and a committed file named `Clean Title` inside it is a plain regular file to `check_row_dir`.
- **V23** A `TTY` case running a two-item fixture-mode list shows `(1/2) [1/2] Vol. 1:` in the merged output, as `e2e.batch.tty-progress` does, and `EXPECT_NO_ANSI` holds with `NO_COLOR=1`.

**Clusters:** Cluster A: V7a, V7b, V7c, V7d · Cluster B: V17, V18 · Cluster C: V12, V13 · Cluster D: V8, V9, V10, V16, V19, V20, V21 · Cluster E: V14, V15, V24, V25 · Cluster F: V1, V11 · Cluster G: V22, V23.

**Which cluster blocks which task:**

| Cluster | Must be confirmed before |
|---|---|
| A | T10, T11 |
| B | T5, T7 |
| C | T5, T6 |
| D | T7, T9, T10 (T13 wording) |
| E | T1 (gate), T3, T6, T9, T10 |
| F | nothing (T13 wording, AC32/35) |
| G | T12 |

Run all clusters before batch 1. Only E is needed for batch 1 itself.

**Language and library:** no libstdc++ C++23 feature is used beyond those already in the tree: `std::expected`, `std::format`/`std::print`, `std::string::contains`, `std::ranges` algorithms and heterogeneous `std::map<…, std::less<>>`. No CMake/CTest/CPack behaviour is relied on beyond what batch already uses.

---

## Task breakdown

Conventions for every task (inherited from batch-plan):
- **Naming and style:**
  - Namespaces: `mangadex_dl` (core), `mangadex_dl::detail` (core-private) and `mangadex_dl::app`.
  - Aggregates have DMIs and are built with designated initializers.
  - Exception: `DownloadServices` and the new `ListFetchServices` are filled **member by member**, because of the clang-analyzer 23 NewDeleteLeaks false positive with `std::function`/`stop_source` members (see `run_title`).
- **`enum class` switches** list every enumerator, have no `default:`, and end with a fallback return.
- **No `<CLI/CLI.hpp>` outside `src/app/`, and no nlohmann in public headers.**
- **Core never prints.** Every stderr line goes through `print_error`, `print_warning`, `print_note` or the observer, which `terminal_safe` the whole line, prefix included.
- **Every "Done when"** is `nice pixi run check` green (long `timeout`) **plus** `quality-reviewer` and `memory-safety-reviewer` both at `VERDICT: APPROVED`.
- **Linking:** all unit tests link into one executable, and all CLI tests into one. A track's tests may call only symbols defined by itself or by an already-integrated batch.
- **Frozen headers:** the public headers from T1 are frozen. Changing one later is a coordination event.
- **Test names** start with the module (`"mdlist: …"`, `"list id: …"`, `"title dir: …"`), so `-R 'cli\.mdlist'` and `-R 'unit\.mdlist'` select them.
- **GCC rule** (skip-existing V19): never `static_cast` a call or arithmetic result to the type it already has (`-Wuseless-cast`).

### T1: Frozen interfaces (public headers, `Errc::row_dir`, doc comments)
- **Goal:** every cross-module declaration lands once, so batches 2 and 3 run core and app tracks in parallel. No behaviour change, no golden change.
- **Files:**
  - edit `include/mangadex_dl/{error.hpp, exit_codes.hpp, options.hpp, model.hpp, download.hpp, batch.hpp, api_client.hpp}`;
  - create `include/mangadex_dl/{list_id.hpp, title_dir.hpp, mdlist.hpp}`;
  - edit `src/app/cmd_batch.cpp` (one `case` only) and `tests/unit/interfaces/headers_test.cpp`.
- **Owns:** exactly those 12 files.
- **Build files for build-fixer:** none.
- **What changes:**
  - **`error.hpp`:** `row_dir, ///< --dir-per-title: this title's own directory below DIR is unusable (exit 1; the run continues)`, after `mixed_failures`. Add `case Errc::row_dir:` to the `failure`/`io` group in `exit_code_for`.
  - **`cmd_batch.cpp` `is_fatal`:** `case Errc::row_dir: return false;` (batch never produces it, so there is no behaviour change; it is the AC39 rule). This is the only app line in T1, there because `-Wswitch` demands it.
  - **`exit_codes.hpp`:** doc comments only. `not_found`: "the title, or for mdlist the list, does not exist on MangaDex (404; a private list counts)". `mixed_failures`: "batch/mdlist: …".
  - **`options.hpp`:** `bool dir_per_title = false; ///< mdlist --dir-per-title: files go to dir/<title directory>; download and batch never set it` as the **last** `DownloadOptions` member. The defaulted `==` still compares member-wise; every job of one run has the same value (V15).
  - **`model.hpp`:** `std::string file_stem{}; ///< sanitize_file_stem(manga.title, manga.id): every epubs[i].file_name starts with it` as the last `Plan` member.
  - **`download.hpp`:**
    - forward-declare `class TitleDirResolver;`;
    - add `TitleDirResolver* title_dir = nullptr; ///< --dir-per-title only; borrowed for the run_download call, nullable (null = the plain stem); called only on the calling thread` as the last `DownloadServices` member;
    - extend `run_download`'s doc with `Errc::row_dir`.
  - **`list_id.hpp`** (light; included by `api_client.hpp`):
    ```cpp
    /// A validated MangaDex custom-list (MDList) id: a lowercase 8-4-4-4-12 hex UUID. Path- and URL-safe.
    class ListId {
    public:
        ListId() = default; ///< the nil UUID: a placeholder; parse() never yields it
        /// A bare UUID (any hex case), or [https://|http://][www.]mangadex.org/list/<uuid> then
        /// nothing, `/`, `/<slug>`, `/<slug>/`, each optionally followed by `?...` or `#...`
        /// (host case-insensitive). Lowercased. Errc::usage otherwise; the message never contains
        /// `text`: a title URL (looks_like_manga_id && !is_uuid) -> "LIST is a MangaDex title, not a
        /// list; use 'mangadex-dl download <title>' for one title"; anything else -> "LIST must be a
        /// MangaDex list UUID or a https://mangadex.org/list/<uuid> URL; for a CSV file of titles use
        /// 'mangadex-dl batch <file>'".
        [[nodiscard]] static Result<ListId> parse(std::string_view text);
        /// Borrows from *this: never bind it to a temporary ListId.
        [[nodiscard]] std::string_view str() const noexcept MANGADEX_DL_LIFETIMEBOUND { return value_; }
        friend bool operator==(const ListId&, const ListId&) = default;
        friend std::strong_ordering operator<=>(const ListId&, const ListId&) = default;
    private:
        explicit ListId(std::string canonical) noexcept : value_(std::move(canonical)) {}
        std::string value_{"00000000-0000-0000-0000-000000000000"};
    };
    /// True when ListId::parse would accept `text` (a bare UUID counts). noexcept, no allocation.
    [[nodiscard]] bool looks_like_list_id(std::string_view text) noexcept;
    ```
  - **`title_dir.hpp`:**
    ```cpp
    struct TitleDirChoice {
        std::string name{};                   ///< one path component below DIR (passes validate_list_dir)
        std::optional<std::string> warning{}; ///< one line for observer.on_warning, without prefix
        friend bool operator==(const TitleDirChoice&, const TitleDirChoice&) = default;
    };
    /// Picks the directory below DIR for one title (--dir-per-title). run_download calls resolve()
    /// at most once per run, on its calling thread, after the plan and before anything is written.
    /// Polymorphic base: neither copyable nor movable.
    class TitleDirResolver {
    public:
        TitleDirResolver(const TitleDirResolver&) = delete; /* + the other three deleted */
        virtual ~TitleDirResolver() = default;
        /// `stem` is Plan::file_stem, borrowed for the call only.
        [[nodiscard]] virtual TitleDirChoice resolve(std::string_view stem, const MangaId& manga) = 0;
    protected:
        TitleDirResolver() = default;
    };
    struct TitleDirClaim {
        std::string name{};
        std::optional<std::size_t> taken_by{}; ///< item that holds the plain stem, when moved aside
        friend bool operator==(const TitleDirClaim&, const TitleDirClaim&) = default;
    };
    /// One run's title directories: first claim wins, in list order. Owns its keys. Not
    /// thread-safe (used only on the run_download thread).
    class TitleDirRegistry {
    public:
        /// The same manga again -> its earlier name, no taken_by. A stem free -> the stem. A stem held
        /// by another manga -> "<stem> [<first 8 hex of manga>]", taken_by = the holder's item; if
        /// that is held too -> "<stem> [<full uuid>]" (returned even if held: unreachable in
        /// practice; OutputRegistry is the backstop).
        [[nodiscard]] TitleDirClaim claim(std::string_view stem, const MangaId& manga, std::size_t item);
    private:
        struct Holder { MangaId manga{}; std::size_t item = 0; };
        std::map<std::string, Holder, std::less<>> by_name_{};
        std::map<MangaId, std::string> by_manga_{};
    };
    ```
  - **`batch.hpp`** (all additive):
    ```cpp
    /// Where a multi-title run's rows come from: the wording of row-scoped messages.
    enum class BatchSource { csv, mdlist };   // "line"/"lines" vs "item"/"items"
    // BatchSettings, last member:  BatchSource source = BatchSource::csv;
    // RowOutcome, last member:     std::string dir{}; ///< effective dir when it differs from job.dir
    //                                                 ///< (--dir-per-title: DIR/<name>); empty = job.dir
    struct ListSummary {             ///< the --json `list` object (mdlist only)
        std::string id{};            ///< canonical list UUID
        std::optional<std::string> name{};
        std::string url{};           ///< https://mangadex.org/list/<id>
        friend bool operator==(const ListSummary&, const ListSummary&) = default;
    };
    [[nodiscard]] std::optional<std::string> batch_closing_message(const BatchResult& result,
                                                                   bool dry_run, BatchSource source);
    [[nodiscard]] std::string batch_to_json(const BatchResult& result, std::string_view source,
                                            bool dry_run, const std::optional<ListSummary>& list);
    ```
    The existing two- and three-argument declarations stay unchanged. T5 turns them into wrappers (`csv`, `nullopt`). The new overloads are declared now and defined in T5; nothing calls them before T5 lands.
  - **`api_client.hpp`:** include `list_id.hpp` and add the member:
    ```cpp
    /// GET <api>/list/<canonical id> (no query), body cap max_api_json_bytes; returns the body of a
    /// 2xx unparsed. 404 -> Errc::not_found "list <id> not found on MangaDex (HTTP 404): it does not
    /// exist or is private; check the id, make the list public on MangaDex, or use 'mangadex-dl
    /// download' if it is a title". 401/403 whose body is_custom_list_error_body -> the same.
    /// Otherwise the usual status mapping (Errc::network "<url>: <reason>"); a stop -> interrupted.
    [[nodiscard]] Result<std::string> fetch_list_document(const ListId& id, std::stop_token stop);
    ```
  - **`mdlist.hpp`.** It includes `api_client.hpp` (TraceSink), `batch.hpp`, `list_id.hpp`, `net/transport.hpp`, `options.hpp`, `<expected>`, `<functional>`, `<optional>`, `<string>`, `<string_view>` and `<vector>`.
    ```cpp
    inline constexpr std::size_t max_list_separators = 200000;   ///< V16 guard: ~20 per title at the cap
    inline constexpr std::size_t max_list_name_bytes = 1024;
    inline constexpr std::size_t max_list_display_name_bytes = 64;
    inline constexpr std::string_view list_row_label = "list";

    struct CustomList {
        ListId id{};
        std::optional<std::string> name{};   ///< ≤ max_list_name_bytes, cut on a UTF-8 boundary; display only
        std::vector<MangaId> titles{};       ///< API order, canonical, duplicates kept
        friend bool operator==(const CustomList&, const CustomList&) = default;
    };
    [[nodiscard]] std::string list_request_url(std::string_view api, const ListId& id);  ///< "<api>/list/<id>"
    [[nodiscard]] std::string list_web_url(const ListId& id);  ///< "https://mangadex.org/list/<id>"
    /// Untrusted. Errc::network, self-contained messages naming the list id, never response bytes.
    [[nodiscard]] Result<CustomList> parse_custom_list(std::string_view json, const ListId& expected);
    /// A 401/403 body that is {"result":"error","errors":[…]} with some errors[i].detail starting
    /// "CustomList " (parsed with parse_untrusted, max 64 KiB, depth 64). False on anything else.
    [[nodiscard]] bool is_custom_list_error_body(std::string_view body);
    /// ≤ max_list_display_name_bytes on a UTF-8 boundary, then "..." when cut (raw; app sanitizes).
    [[nodiscard]] std::string list_name_for_display(std::string_view name);
    [[nodiscard]] ListSummary list_summary(const CustomList& list);

    enum class ListFailureKind { request, content };  ///< request: transport/HTTP/interrupt; content: the document
    struct ListFetchError {
        ListFailureKind kind = ListFailureKind::request;
        Error error{};
        friend bool operator==(const ListFetchError&, const ListFetchError&) = default;
    };
    template <class T> using ListFetchResult = std::expected<T, ListFetchError>;
    /// Borrowed for the call: transport and clock non-null; trace nullable and thread-safe.
    struct ListFetchServices {
        Transport* transport = nullptr;
        Clock* clock = nullptr;
        std::function<std::uint32_t(std::uint32_t)> jitter{};
        TraceSink* trace = nullptr;
    };
    /// One ApiClient (endpoints_for(api_base, report = false): no reporter thread), a stop_source
    /// wired to an InterruptBridge, a pending-interrupt check before the request; then
    /// fetch_list_document -> parse_custom_list. Everything it creates is joined before it returns.
    [[nodiscard]] ListFetchResult<CustomList> fetch_custom_list(const ListId& id, const ApiBase& api_base,
                                                                const ListFetchServices& services);

    struct RawMdlistArgs {              ///< what CLI11 binds for mdlist (owned by the Command)
        std::string list{};             ///< LIST as typed
        RawDownloadArgs defaults{};     ///< .manga and .title stay empty; .dir is DIR
        bool fail_fast = false;
        bool dir_per_title = false;
    };
    struct MdlistSettings {
        BatchSettings batch{};          ///< file = LIST as typed, label = "list", source = mdlist
        ListId list{};
        bool dir_per_title = false;
    };
    /// Errc::usage, in this order: "LIST must not be empty"; ListId::parse's messages; "DIR must
    /// not be empty"; "DIR looks like a MangaDex id; give one list per run" (manga or list id);
    /// then detail::validate_common(defaults, MangaId{}).
    [[nodiscard]] Result<MdlistSettings> validate_mdlist_args(const RawMdlistArgs& raw);
    /// line = 1-based position; raw = defaults with .manga = the id; options = base with .manga and
    /// .dir_per_title; dir = DIR; sub_dir = ""; duplicates marked (batch's mark_duplicates).
    [[nodiscard]] std::vector<BatchJob> plan_list(const CustomList& list, const MdlistSettings& settings);
    ```
  - **`headers_test.cpp`:**
    - `static_assert(md::exit_code_for(md::Errc::row_dir) == 1)`;
    - the new constants;
    - `std::is_abstract_v<md::TitleDirResolver>`;
    - `ListId{}.str()` is the nil UUID;
    - every new aggregate default-constructs and builds with partial designated init;
    - `DownloadOptions{} == DownloadOptions{}`, and `dir_per_title` defaults false;
    - `BatchSettings{}.source == BatchSource::csv`.
- **Dependencies:** none.
- **Done when:**
  - `nice pixi run check` **and** `nice pixi run -e gcc check` are green (a new enumerator is compiler-sensitive through `-Wswitch`);
  - `git diff --stat tests/e2e/golden` is empty;
  - both reviewers approve.
- **Tests:** unit: `headers_test.cpp` as above. No other tier changes; the existing suite proves no regression.

### T2: Pure refactor: shared mangadex.org URL walk
- **Goal:** move `extract_uuid`'s URL walk out of `manga_id.cpp` into a core-private helper, parameterised by the path, with **no behaviour change**.
- **Files:** create `src/core/mangadex_url.hpp`, `src/core/mangadex_url.cpp` and `tests/unit/mangadex_url/mangadex_url_test.cpp`; edit `src/core/manga_id.cpp`.
- **Owns:** exactly those 4 files.
- **Build files for build-fixer:** none.
- **What changes:**
  - **`mangadex_url.hpp`** (`mangadex_dl::detail`):
    - `[[nodiscard]] std::optional<std::string_view> extract_mangadex_uuid(std::string_view text MANGADEX_DL_LIFETIMEBOUND, std::string_view path) noexcept;`, where `path` is `"/title/"` or `"/list/"`;
    - `[[nodiscard]] std::string lowercase_ascii(std::string_view text);`.
  - The body is today's `extract_uuid` (`manga_id.cpp:68-103`) with `title_path` replaced by `path`. `is_valid_url_tail`, `is_url_byte`, `starts_with_icase` and `ascii_lower` move with it.
  - `MangaId::parse` and `looks_like_manga_id` call `extract_mangadex_uuid(text, "/title/")`. `is_uuid` stays in `manga_id.cpp`, the public symbol.
- **Dependencies:** none (runs alongside T1).
- **Done when:**
  - `nice pixi run check` is green;
  - `tests/unit/manga_id/manga_id_test.cpp` and `tests/unit/options/options_test.cpp` pass **unmodified** (AC2);
  - `git diff --stat tests/e2e/golden tests/unit/manga_id tests/unit/options` is empty;
  - both reviewers approve.
- **Tests (unit, `mangadex_url_test.cpp`):** the helper with both paths: a title path doesn't accept `/list/`, and vice versa; a bare UUID is accepted for either. The returned view points into the input (`data()` within `[text.data(), text.data()+size)`).

### T3: Pure refactor: extract batch phase 2 into `cmd_batch_jobs`
- **Goal:** `ConsoleTitleRunner`, the closing, note and JSON tail, and the shared print helpers move verbatim out of `cmd_batch.cpp`, so `mdlist` can reuse them. `batch` stays byte-identical.
- **Files:** create `src/app/cmd_batch_jobs.hpp` and `src/app/cmd_batch_jobs.cpp`; edit `src/app/cmd_batch.cpp`.
- **Owns:** exactly those 3 files. `cmd_batch.cpp` passes from T1 (batch 1) to T3 (batch 2).
- **Build files for build-fixer:** none.
- **What changes:**
  - **`cmd_batch_jobs.hpp`:**
    ```cpp
    /// `mangadex-dl: error: <message>` uncolored, as every exit-2 diagnostic.
    void print_usage_line(Io& io, std::string_view message);
    /// "Run 'mangadex-dl <command> --help' for more information." -> returns exit 2.
    [[nodiscard]] int usage_hint(Io& io, std::string_view command);
    void print_warning(Io& io, const OutputPolicy& policy, std::string_view message);  ///< not with -q
    void print_note(Io& io, const OutputPolicy& policy, std::string_view message);     ///< not with -q, uncolored
    /// Phase 2 of a multi-title run over non-empty `jobs`: the runner, run_batch, then (unless
    /// interrupted) the up-to-date note, the closing line and, with --json, the document. Every
    /// argument is borrowed for the call; `backend` and `agent` are shared by every row.
    [[nodiscard]] int run_batch_jobs(std::span<const BatchJob> jobs, const BatchSettings& settings,
                                     const Backend& backend, const std::string& agent, Io& io,
                                     const OutputPolicy& policy);
    ```
  - `is_fatal` (including T1's `row_dir` case) and `ConsoleTitleRunner` move into `cmd_batch_jobs.cpp`'s anonymous namespace, unchanged.
  - `run_batch_command` keeps phases 1–6 (validate, read, parse, plan, empty list, handlers, agent, backend), then `return run_batch_jobs(*jobs, *settings, *backend, agent, io, policy);`.
  - `usage_hint(io, "batch")` replaces the batch-only version and prints the same bytes.
- **Dependencies:** T1.
- **Done when:**
  - `nice pixi run check` is green;
  - `git diff --stat tests/` is empty: **no test edited, no bless requested**;
  - the real installed `build/default/dev/e2e-prefix/bin/mangadex-dl batch --help` and `download --help`, captured before and after, are `cmp`-identical;
  - every `cli.batch*`, `e2e.batch.*` and `e2e.edge.batch-*` case passes unmodified;
  - `nice pixi run tsan -R 'cli\.batch'` is green (V24);
  - both reviewers approve.
- **Tests:** none new. The byte-identity proof is the existing suite plus the goldens `batch.help`, `batch.dry-run` and `batch.json-dry-run` (V8a).

### T4: `ListId`, `looks_like_list_id`, and the `download <list URL>` hint
- **Goal:** the LIST forms per spec "LIST forms", and the new `download` exit-2 text.
- **Files:** create `src/core/list_id.cpp`, `tests/unit/list_id/list_id_test.cpp` and `tests/cli/list_hint_download_test.cpp`; edit `src/core/options.cpp`.
- **Owns:** exactly those 4 files.
- **Build files for build-fixer:** none.
- **What changes:**
  - **`ListId::parse`:**
    - `extract_mangadex_uuid(text, "/list/")` → `lowercase_ascii` → `ListId`.
    - On failure: if `looks_like_manga_id(text) && !is_uuid(text)`, the "is a title" message; else the generic message (both in T1's doc).
  - **`looks_like_list_id`** = `extract_mangadex_uuid(text, "/list/").has_value()`.
  - **`validate_download_args`:** when `MangaId::parse` fails **and** `looks_like_list_id(raw.manga)`, return Errc::usage `MANGA is a MangaDex list, not a title; use 'mangadex-dl mdlist <list>' to download every title in it`. Every other bad MANGA keeps today's text.
  - A bare UUID parses as a MangaId first, so it never reaches the hint.
- **Dependencies:** T1, T2.
- **Done when:** `nice pixi run check` is green, including the unmodified `unit.manga_id*` and `unit.options*`; both reviewers approve.
- **Tests:**
  - **unit (`list_id_test.cpp`), AC1:**
    - accept: bare UUID upper and mixed case, lowercased; `https`, `http`, no scheme; `www.`; upper-case host; `/list/<uuid>`, `/<slug>`, the trailing `/` (the empty-slug form), `/<slug>/`, `?q`, `#f`;
    - reject: empty, `-`, `titles.csv`, `/list/<uuid>/a/b`, `/list//`, `mangadex.org.evil`, a port, a space, a control byte, 35 and 37 characters;
    - `/title/<uuid>` gets the distinct title message;
    - no message contains the input (checked for every reject row).
  - **unit (AC2):** for every row of the accept and reject tables, `looks_like_list_id(x) == ListId::parse(x).has_value()`.
  - **cli (`list_hint_download_test.cpp`, via `run_cli`), AC15 download part:**
    - `download https://mangadex.org/list/<id>/x out` → exit 2, empty stdout, the list message plus `Run 'mangadex-dl download --help' …`;
    - `download not-a-manga` keeps the old text.

### T5: Batch core for lists: wording, effective dir, the `list` JSON object, `TitleDirRegistry`
- **Goal:** the pure core pieces the shared runner needs; batch output stays byte-identical.
- **Files:**
  - edit `src/core/batch_run.cpp` and `src/core/batch_json.cpp`;
  - create `src/core/title_dir.cpp`, `tests/unit/batch/batch_source_test.cpp`, `tests/unit/batch/batch_json_list_test.cpp` and `tests/unit/title_dir/title_dir_test.cpp`.
- **Owns:** exactly those 6 files.
- **Build files for build-fixer:** none.
- **What changes:**
  - **`batch_closing_message(result, dry_run, source)`:**
    - the noun comes from `source`: `line`/`lines` for csv, `item`/`items` for mdlist;
    - every variant: `stopped at <noun> <k>: <reason>[; <r> titles not run]`, `<n> of <m> titles did not complete (<nouns> …); …` and the dry-run `would fail`;
    - the two-argument overload forwards with `csv`.
  - **`run_batch`:** `run_result` uses `outcome.dir` when it is non-empty, else `job.dir`. Nothing else in the loop changes.
  - **`batch_to_json(…, list)`:** when `list` has a value, insert `"list": {"id", "name" (null when absent), "url"}` right after `"source"`. The three-argument overload forwards with `nullopt`, so a batch document is byte-identical and never has `list`.
  - **`TitleDirRegistry::claim`:** exactly T1's contract.
    - The suffix uses `manga.str().substr(0, 8)` and the full `manga.str()`.
    - Keys are owned `std::string`s; nothing borrows `stem`.
- **Dependencies:** T1.
- **Done when:**
  - `nice pixi run check` is green;
  - every existing `unit.batch*` test passes unmodified (AC7 csv half);
  - both reviewers approve.
- **Tests (unit):**
  - **AC7 (`batch_source_test.cpp`):**
    - with `BatchSource::mdlist`: `2 of 15 titles did not complete (items 4, 9); …`, `stopped at item 9: --fail-fast; 6 titles not run`, singular `item 4`, the 10-item cap, dry-run `would fail (items …)`;
    - with `csv`, the same results give today's strings byte for byte.
  - **AC12 (unit half):** a FakeRunner that calls `simulate_interrupt_for_testing(SIGINT)` during item 1 of 2 under an mdlist-shaped job list → item 2 never runs, `stop == interrupted`, no closing message. Use the RAII flag reset.
  - **AC39 (core half):**
    - a non-fatal `exit_code 1` outcome with `dir = "out/X"` → `failed`, the next item runs, `titles[0].dir == "out/X"`;
    - {failed, 4} → 7.
  - **AC8 (`batch_json_list_test.cpp`):**
    - keys exactly batch's plus `list{id,name,url}` after `source`; `line` = position;
    - `name` null when absent;
    - a name with ESC and invalid UTF-8 → valid JSON, `\u001b`, U+FFFD, no `0x1B` byte (V17);
    - an empty result with a list → `"titles": []` plus the `list` object;
    - a batch document from the three-argument overload equals the one built today for the same input, byte for byte, and has no `list` key.
  - **AC37 (`title_dir_test.cpp`):**
    - the same manga twice → the same name, no `taken_by`;
    - another manga with the same stem → `<stem> [77777777]`, `taken_by = 1`;
    - a third manga sharing the stem and the 8-hex prefix → `<stem> [<full uuid>]`;
    - a title whose own stem equals an earlier suffixed name → suffixed itself;
    - a 150-byte stem plus the full suffix ≤ 189 bytes.
  - **AC36 (property half, V12):**
    - every `sanitize_file_stem(s, uuid)` over the filename test corpus plus ≥10 000 seeded random byte strings (record the seed), and each with ` [<8 hex>]`/` [<uuid>]` appended, passes `validate_list_dir` and contains no `/`;
    - the AC36 examples: `A/B: C?` → `A_B_ C_`; `...`, all spaces and only U+202E → the UUID; a 300-byte CJK title → ≤150 bytes on a boundary; leading `-`/`.` and trailing `.`/space trimmed.

### T6: Engine: `--dir-per-title` inside `run_download`
- **Goal:** after a non-empty plan, resolve the title's directory, print the `-v` line, run the read-only pre-flight, and run the unchanged execute phase under `DIR/<name>`. With the flag off, nothing changes.
- **Files:**
  - edit `src/core/download.cpp`, `src/core/download/internal.hpp` and `src/core/planner.cpp`;
  - create `src/core/download/dir_per_title.cpp`, `tests/unit/download/download_dir_per_title_test.cpp` and `tests/unit/planner/planner_file_stem_test.cpp`.
- **Owns:** exactly those 6 files.
- **Build files for build-fixer:** none.
- **What changes:**
  - **`planner.cpp`:** `build_plan` computes the stem once, passes it to `make_epubs` (whose local becomes a parameter), and sets `plan.file_stem`. The file names are unchanged.
  - **`internal.hpp`:**
    - `RunContext` gains `TitleDirResolver* title_dir = nullptr;` as its last member;
    - declare `[[nodiscard]] Result<std::filesystem::path> resolve_title_dir(RunContext& ctx, const Plan& plan);`, documented as "dir_per_title and a non-empty plan only; returns options.dir / name".
  - **`dir_per_title.cpp`, `resolve_title_dir`:**
    1. `stop_now` → interrupted.
    2. `choice = ctx.title_dir ? ctx.title_dir->resolve(plan.file_stem, options.manga) : TitleDirChoice{.name = plan.file_stem}`.
    3. Defence in depth: `validate_list_dir(choice.name)` must succeed and the name must contain no `/`; otherwise `internal_error("title directory name")`.
    4. `choice.warning` → `observer->on_warning`.
    5. `observer->on_verbose(1, "output directory: " + (options.dir / name).string())`.
    6. `check_row_dir(options.dir, name)`:
       - `ok` → return `options.dir / name`;
       - `row_error` → `Errc::row_dir` with the verdict's message;
       - `global_error` → `Errc::failure` with the verdict's message.
  - **`download.cpp`:**
    - `ctx.title_dir = services.title_dir`;
    - after `plan_phase`: if `options.dir_per_title && !planned->plan.epubs.empty()`:
      - resolve;
      - on error, return it;
      - else declare `DownloadOptions effective = options; effective.dir = *dir;` then `RunContext title_ctx = ctx; title_ctx.options = &effective;` (the copied `stop_source` shares state), and `return execute_phase(title_ctx, *planned)`;
    - otherwise unchanged. Both locals live in `run_download`'s frame until `execute_phase` returns.
  - `execute_phase.cpp` is **not edited** (Spec issue 5).
- **Dependencies:** T1.
- **Done when:**
  - `nice pixi run check` **and** `nice pixi run -e gcc check` are green;
  - every existing `unit.download*`, `unit.planner*`, `cli.download*` and `e2e.download.*` test passes unmodified (AC38 first bullet, V15);
  - `git diff --stat tests/e2e/golden` is empty;
  - both reviewers approve.
- **Tests (unit):**
  - **`planner_file_stem_test.cpp`:**
    - `plan.file_stem == sanitize_file_stem(plan.manga.title, plan.manga.id)` and every `file_name` starts with it;
    - AC36 lang half: with no `en` title, the stem follows `--lang` (an altTitle in that language), and otherwise the first title.
  - **`download_dir_per_title_test.cpp`** (clean fixture tree in a `TempDir`, fixture transport plus instant clock as `download_test.cpp` does; a test `FakeResolver` records calls and returns a scripted choice), AC38 and V13:
    - flag off → paths and report equal to a run without the field (compare `report_to_json` bytes), and the resolver is never called;
    - flag on, null resolver → files in `DIR/Clean Title/`, report paths include it, one `output directory: …/Clean Title` verbose line;
    - flag on, resolver returns `X [77777777]` plus a warning → files under that name, the warning reaches `on_warning` once, before any `on_epub_end`;
    - `DIR/Clean Title` a regular file → `Errc::row_dir` with `cannot open directory '…/Clean Title': Not a directory`; the recursive listing is unchanged;
    - DIR a regular file → `Errc::failure`;
    - dry run → neither DIR nor `DIR/Clean Title` created, every file `planned` under `DIR/Clean Title/` (V13);
    - DIR and the title dir both missing, real run → both created;
    - the exit-5 tree (`nochapters`) with the flag → no resolver call, no verbose line, no directory (Spec issue 2);
    - unknown manga (404) → no resolver call;
    - an interrupt simulated before the call → interrupted, resolver not called;
    - a resolver that returns `../x` or `a/b` → internal error, nothing created.

### T7: List document: parse, URLs, error-body check, fetch, and fixture lists
- **Goal:** AC3–AC5 and V9 in core, plus generated list documents in every fixture tree.
- **Files:**
  - create `src/core/mdlist.cpp`, `src/core/mdlist_fetch.cpp`, `tests/unit/mdlist/mdlist_parse_test.cpp`, `tests/unit/mdlist/mdlist_fetch_test.cpp` and `tests/unit/support/mdx_fixture_list_test.cpp`;
  - edit `src/core/api_client.cpp` and `tests/support/mdx_fixture.hpp`.
- **Owns:** exactly those 7 files.
- **Build files for build-fixer:** none.
- **What changes:**
  - **`parse_custom_list`:**
    - `json_util::parse_untrusted(json, "custom list", {.max_separators = max_list_separators})` (API defaults otherwise: 16 MiB, depth 64, BOM and NUL rejected);
    - `FieldReader` checks:
      - `data` object;
      - `data.type == "custom_list"`;
      - `data.id`: a string, `is_uuid`, and case-insensitively equal to `expected`;
      - `data.relationships`: a required array;
      - each entry: an object with a string `type` (Spec issue 6);
      - for `type == "manga"`: a string `id`, `is_uuid`, then `MangaId::parse`;
      - other types and unknown fields ignored;
    - more than `max_list_titles` manga entries → the cap message;
    - `attributes.name` optional: absent or null → nullopt, else cut to `max_list_name_bytes` backing off UTF-8 continuation bytes (V18);
    - every error is prefixed `list <expected>: ` (Spec issue 1), apart from the cap message, which is already self-contained;
    - every stored string is an owned copy.
  - **`list_request_url` and `list_web_url`:** fixed formats, with the canonical id.
  - **`is_custom_list_error_body`:** T1's contract, with a `parse_untrusted` limit of 64 KiB.
  - **`list_name_for_display` and `list_summary`:** as declared.
  - **`ApiClient::fetch_list_document`** (in `api_client.cpp`):
    - `s.perform(GetSpec{.url = list_request_url(api, id), .max_body_bytes = max_api_json_bytes}, stop)`;
    - 404 → not_found with T1's message;
    - 401/403 with `is_custom_list_error_body(body)` → the same not_found;
    - any other non-2xx → `s.status_error`;
    - 2xx → `std::move(body)`.
  - **`fetch_custom_list`** (in `mdlist_fetch.cpp`):
    - declaration order: `std::stop_source stop{}` → `ApiClient client{…, ClientConfig{.endpoints = endpoints_for(api_base, /*report=*/false)}, services.jitter, services.trace}` → `const InterruptBridge bridge{stop}`, so the bridge is destroyed first;
    - `interrupt_requested()` → `{request, interrupted}`;
    - document error → `{request, error}`;
    - parse error → `{content, error}`.
  - **`mdx_fixture.hpp`:**
    - `struct FixtureList { std::string id{}; std::optional<std::string> name{}; std::vector<std::string> manga_ids{}; std::optional<std::string> data_id{}; std::string visibility = "public"; };` (raw strings, so L5's `../x` is expressible);
    - `FixtureManga` gains `std::vector<FixtureList> lists{}` (last member);
    - `list_json(const FixtureList&)` follows the spec's live shape: `{"result":"ok","response":"entity","data":{"id","type":"custom_list","attributes":{"name","visibility","version":1},"relationships":[{"id","type":"manga"}…,{"id","type":"user"}]}}`, with exactly one `user`, last;
    - `write_fixture_tree` writes `api/list/<id>.json`, after a `safe_component` check on `id`;
    - `mdx::list_id(n)` gives `a0000000-0000-4000-8000-<n as 12 hex>`;
    - the lists from the spec's table are added to the `clean`, `nochapters`, `incomplete` and `bulk` trees: L1–L6 on clean; L8, L9, L10; no file for `…ff`.
    - **Constraint:** if attaching lists inside `clean_fixture()` etc. changes any existing test's expectations (a tree file listing, for example), attach them through `all_fixtures()`-side wrappers instead, so that existing tests stay unmodified and the tree count stays twelve. That decision is the implementer's; a forced existing-test edit is a coordination event.
- **Dependencies:** T1, T4.
- **Done when:**
  - `nice pixi run check` is green, including the unmodified `unit.api_client*`, `unit.support*` and `unit.mangadex_api*`;
  - `nice pixi run tsan -R 'unit\.mdlist'` is green (a new InterruptBridge use; V24);
  - both reviewers approve.
- **Tests (unit):**
  - **AC3:**
    - the live-sample shape (`user` last) → manga in order;
    - `user` and an unknown type ignored; extra fields ignored;
    - name absent → nullopt, `null` → nullopt;
    - a 2000-byte name (multi-byte at the boundary) → ≤1024 bytes and valid UTF-8;
    - an upper-case manga id → canonical;
    - only `user` → empty `titles`.
  - **AC4:** each of these → `Errc::network`, the message starts `list <id>`, and contains no response bytes (plant a marker string in each hostile document and assert it is absent):
    - not JSON; BOM; NUL; depth 65; 200 001 separators; 16 MiB + 1;
    - missing `data`, `id`, `type`, `relationships`; `type` `"manga"`; `data.id` = L1 while L6 was requested;
    - manga ids `../x`, 35 characters, `42`;
    - a non-object entry; an entry without `type`;
    - 10 001 manga entries → the exact cap message naming `10000`.
  - **AC5 (`mdlist_fetch_test.cpp`, FakeTransport):**
    - `ListId::parse(upper)` → the recorded URL is exactly `<api>/list/<lowercase>`, with `max_body_bytes == max_api_json_bytes`;
    - 200 → the list;
    - 404 → `{request, not_found}` with the exact message;
    - 503 ×N then 200 → the list; 503 exhausted → `{request, network}`;
    - 429 → per policy;
    - 401 and 403 with a `CustomList` error body → not_found;
    - a bare 403 → network with the 403 hint;
    - malformed 200 → `{content, network}`;
    - an `on_fetch` hook that calls `simulate_interrupt_for_testing` and cancels → `{request, interrupted}`;
    - the flag set before the call → interrupted, and no request recorded;
    - a trace sink receives exactly one `GET … -> 200` line.
  - **V9 (`mdx_fixture_list_test.cpp`):**
    - `write_fixture_tree` with L1 → the file exists;
    - `fetch_custom_list` over `make_fixture_transport(root)` and `ApiBase::parse("file://<root>")` → L1;
    - the unknown id → `not_found` (V19);
    - L6 → content error.

### T8: `validate_mdlist_args`, `plan_list`, and the `batch <list URL>` hint
- **Goal:** CLI-level validation and the pure list-to-jobs step, reusing batch's dedupe.
- **Files:** create `src/core/mdlist_plan.cpp`, `src/core/batch_internal.hpp`, `tests/unit/mdlist/mdlist_plan_test.cpp` and `tests/cli/list_hint_batch_test.cpp`; edit `src/core/batch_plan.cpp`.
- **Owns:** exactly those 5 files.
- **Build files for build-fixer:** none.
- **What changes:**
  - **`batch_internal.hpp`** (core-private): `namespace mangadex_dl::detail { void mark_duplicates(std::vector<BatchJob>& jobs); }`. It moves out of `batch_plan.cpp`'s anonymous namespace unchanged.
  - **`validate_batch_args`:** after the existing "FILE looks like a MangaDex title" check, if `looks_like_list_id(raw.file)` → `FILE looks like a MangaDex list; use 'mangadex-dl mdlist <list>'`. The order matters: a bare UUID keeps the title message.
  - **`validate_mdlist_args` and `plan_list`:** exactly T1's contracts. The label is `list_row_label`, the source `mdlist`, and `file` = LIST as typed.
- **Dependencies:** T1, T4.
- **Done when:**
  - `nice pixi run check` is green, including every existing `unit.batch*` and `cli.batch*` test unmodified;
  - both reviewers approve.
  - Note: `batch <list URL>` changes from exit 1 to exit 2. No existing test covers it (grep found no `/list/` in tests).
- **Tests:**
  - **unit, AC6:**
    - positions 1..n in API order; `dir` = DIR and `sub_dir` empty for all;
    - `options` = base plus the manga (plus `dir_per_title` when set); `raw.manga` = the id;
    - a repeated id → `duplicate_of` = the earlier position;
    - empty → no jobs.
  - **unit, AC10:** every `validate_mdlist_args` message in order: LIST empty, a title URL, `-`, a CSV path, junk, DIR empty, DIR a manga id, DIR a list URL, `--lang bogus`; `settings.batch.source == mdlist`.
  - **cli (`list_hint_batch_test.cpp`), AC15 batch part:**
    - `batch https://mangadex.org/list/<id> out` → exit 2, the list message plus `Run 'mangadex-dl batch --help' …`, empty stdout, `out` absent;
    - `batch <bare uuid>` → the title message.

### T9: App runner for lists (source-aware wording, directory resolver, effective dir)
- **Goal:** `run_batch_jobs` serves both `batch` and `mdlist`, and `run_title` passes the resolver through.
- **Files:** edit `src/app/cmd_batch_jobs.hpp`, `src/app/cmd_batch_jobs.cpp`, `src/app/cmd_download_run.hpp` and `src/app/cmd_download_run.cpp`; create `tests/cli/batch_jobs_list_test.cpp` and `tests/support/mdx_twin.hpp`.
- **Owns:** exactly those 6 files.
- **Build files for build-fixer:** none.
- **What changes:**
  - **`run_title`** gains a trailing `TitleDirResolver* title_dir = nullptr` → `services.title_dir = title_dir;` (member by member). `cmd_download.cpp` needs no edit.
  - **`run_batch_jobs`** gains a trailing `const std::optional<ListSummary>& list = std::nullopt`.
    - Declaration order: `OutputRegistry registry` → `TitleDirRegistry dirs` → `ConsoleTitleRunner runner{…, registry, dirs}` → `run_batch`.
    - It calls `batch_closing_message(result, dry_run, settings.source)` and `batch_to_json(result, settings.file, dry_run, list)`.
  - **`ConsoleTitleRunner`:**
    - **Prefix:** csv `"{label}:{line}: "` (unchanged); mdlist `"list:{line}: {uuid}: "`.
    - **`-v` start line:** `"{label}:{line}: {uuid}"` for both (Spec issue 7).
    - **Duplicate:** `"{prefix}same as {line|item} {k}; skipped"`.
    - **Collision advice:** csv unchanged; mdlist `"{prefix}{path} was already written by item {k}; download one of them on its own with 'mangadex-dl download <title> --title ...'"`.
    - **Resolver member:** a `final` `ConsoleTitleDirResolver : TitleDirResolver` holding `TitleDirRegistry*`.
      - `begin(item)` clears the last claim.
      - `resolve()` claims and records the name. When `taken_by` is set, it returns the warning `directory '<stem>' already holds item <k> (a title with the same name); using '<name>'`.
    - **Per row:** when `job.options.dir_per_title`, call `begin(job.line)` and pass `&resolver_` to `run_title`; afterwards, if a name was claimed, `outcome.dir = join_list_dir(settings.defaults.dir, name)`.
    - `is_fatal(Errc::row_dir) == false` already holds (T1).
  - **`mdx_twin.hpp`:** `write_clean_twin_tree(root)` writes:
    - the clean tree;
    - the twin's `api/manga/7777….json` and feed pages (via `mdx::manga_json` and `mdx::feed_pages_json` of a copy of `clean_fixture()` with `id = 77777777-7777-4777-8777-777777777777`);
    - the cover images under `uploads/covers/7777…/` (Spec issue 10, V16).

    It is not in `all_fixtures()`.
- **Dependencies:** T3, T5, T6.
- **Done when:**
  - `nice pixi run check` is green, including every `cli.batch*` and `e2e.batch.*` test unmodified and the batch goldens unchanged;
  - `nice pixi run tsan -R 'cli\.(batch|mdlist)'` is green;
  - both reviewers approve.
- **Tests (cli, `batch_jobs_list_test.cpp`).** These call `run_batch_jobs` directly with hand-built `BatchJob`s (no list fetch), `BatchSettings{.label = "list", .source = mdlist}`, and a fixture `Backend` from `make_backend`. Test names start `mdlist: `.
  - **Wording:**
    - a nochapters/unknown pair → `error: list:1: 5555…: no downloadable chapters …` and `error: list:2: 6666…: title 6666… not found …`;
    - the closing line `2 of 2 titles did not complete (items 1, 2); …`;
    - a duplicate → `list:2: 1111…: same as item 1; skipped`;
    - with `-v`, `mangadex-dl: list:1: 1111…`.
  - **AC39:**
    - `dir_per_title` with `out/Clean Title` a regular file plus a second item that is unknown → item 1 `failed`/1 with `cannot open directory`, item 2 runs (404), exit 7, no `stopped at`;
    - `--json` `titles[0].dir == "out/Clean Title"`.
  - **AC41 (runner level, `clean-twin`):**
    - jobs 1111 then 7777 with `dir_per_title` → item 2 in `out/Clean Title [77777777]/`;
    - the exact warning `warning: list:2: 7777…: directory 'Clean Title' already holds item 1 (a title with the same name); using 'Clean Title [77777777]'`;
    - both file sets exist;
    - `-q` hides the warning;
    - JSON `titles[1].dir`.
  - **Flat collision under mdlist:** the mdlist advice text.
  - **csv unchanged:** a two-row csv run's stderr and JSON equal the strings `batch_cli_test.cpp` pins.

### T10: `cmd_mdlist` (not yet registered)
- **Goal:** the whole `mdlist` command as a free function testable without CLI11, plus `register_mdlist` for T11.
- **Files:** create `src/app/cmd_mdlist.hpp`, `src/app/cmd_mdlist.cpp` and `tests/cli/mdlist_cli_test.cpp`.
- **Owns:** exactly those 3 files.
- **Build files for build-fixer:** none.
- **What changes:**
  - **`cmd_mdlist.hpp`**, with the same lifetime docs as `cmd_batch.hpp`:
    - `[[nodiscard]] int run_mdlist_command(const RawMdlistArgs&, const GlobalOptions&, Io&);`
    - `[[nodiscard]] std::string mdlist_help_footer();`
    - `[[nodiscard]] std::unique_ptr<Command> register_mdlist(CLI::App& parent, Command*& selected);`
  - **`MdlistCommand final : Command`** owns `RawMdlistArgs args_{}`. `bind()` adds, in order:
    - `LIST` (required, `type_name("")`, "Public MDList: its UUID or list URL");
    - `DIR` → `args_.defaults.dir` ("Output directory (created if missing)", `capture_default_str`);
    - `bind_selection_options(sub, args_.defaults)` and `bind_run_options(sub, args_.defaults)`;
    - `--fail-fast` → `args_.fail_fast` ("Stop at the first title that fails");
    - `--dir-per-title` → `args_.dir_per_title` ("Put each title in a subdirectory of DIR");
    - `sub.footer(mdlist_help_footer())`: the spec's LIST/Examples/re-run/symlink text, pre-aligned at ≤80 columns, plus `exit_status_footer()`.
  - The subcommand description is "Download every title in a public MangaDex list (MDList)".
  - The selecting callback captures `[slot = &selected, self = command.get()]`.
  - **`run_mdlist_command`, phases:**
    1. `policy`; `validate_mdlist_args` → `print_usage_line` plus `usage_hint(io, "mdlist")` → 2. No request, no fd, no directory.
    2. `ScopedInterruptHandlers::install()` → `user_agent()` → `make_backend(base.api_base, agent)`. A failure → `print_error(with_remedy)` → its code.
    3. `StderrTrace trace{io, policy}`: a `final` `TraceSink`, mutex-guarded. When `verbose >= 2 && !quiet` it prints `mangadex-dl: <line>` through `terminal_safe`.
       - `ListFetchServices` is filled member by member: transport and clock from the backend, `make_jitter()`, `&trace`.
       - `fetch_custom_list(settings.list, base.api_base, services)`.
       - On error: `interrupted` → return its code, printing nothing; `request` → `print_error(with_remedy(e))`; `content` → `print_error(e.message)`; return `e.exit_code()`. No JSON.
    4. `-v`: `list <id> "<display name>": <n> title(s)`, or without a name `list <id>: <n> title(s)`, via `print_note` (verbose only).
    5. `jobs = plan_list(list, settings)`. When empty: the warning `list <id> has no titles` (not with `-q`); with `--json`, `batch_to_json(BatchResult{}, file, dry_run, list_summary(list))`; return 0. DIR is not touched.
    6. `return run_batch_jobs(jobs, settings.batch, *backend, agent, io, policy, list_summary(list));`
  - **Declaration order:** `handlers` → `agent` → `backend` → `trace` → (the fetch: its client and bridge are created and joined inside `fetch_custom_list`) → `list` → `jobs` → `run_batch_jobs`.
- **Dependencies:** T7, T8, T9.
- **Done when:**
  - `nice pixi run check` and `nice pixi run -e gcc check` are green;
  - `nice pixi run tsan -R 'cli\.mdlist'` is green;
  - both reviewers approve. `memory-safety-reviewer` needs evidence that the list-fetch bridge and the per-item runs were exercised under ASan+UBSan and TSan.
- **Tests (cli, `mdlist_cli_test.cpp`).** These call `run_mdlist_command` with a filled `RawMdlistArgs`, a fixture tree in a `TempDir`, and an `ostringstream` Io.
  - **AC9:** L1 on clean → 0; stdout = download's two paths; stderr empty; the files exist; `--json` `list.name == "Clean List"`, `source` as typed, `titles[0].line == 1`, `list.url` the website URL.
  - **AC10:** every usage error → 2, empty stdout, `Run 'mangadex-dl mdlist --help' …`, and no request. Prove "no request" with `api_base = file:///nonexistent`: still 2, and DIR is not created.
  - **AC11:** the unknown list → 4 with the exact message; no JSON, also with `--json`; DIR absent.
  - **AC12 (cli half):** the flag set before the call → nothing written, empty stdout, no JSON, no closing line, rc 1. A second call afterwards succeeds.
  - **AC13:**
    - `-v` prints the list line. Use a hand-written list doc whose name holds ESC; it shows `?`.
    - `-v` prints `list:1: 1111…`.
    - `-vv` traces `GET …/api/list/<id> -> 200`.
    - `-q` hides the empty-list warning (L3) and the duplicate warning (L4), but keeps the closing error (L2).
  - **AC14:** L2 → 4; item 2's line is `error: list:2: 6666…: title 6666… not found …`; the closing line is `1 of 2 titles did not complete (item 2); see the errors above`.
  - **List-level 3:** L5 and L6 → 3; the message starts `list a0000000-…-000000000005: malformed` with **no** network remedy; no JSON, DIR absent.
  - **AC30 (cli half):** `stderr_tty` → `(1/2) [1/2] Vol. 1:`; none with `json`/`quiet`.
  - **AC40:** L1 `--dir-per-title`:
    - stdout `out/Clean Title/Clean Title - Vol. 01.epub` then `… Vol. 02.epub`; stderr empty;
    - JSON `titles[0].dir == "out/Clean Title"`, report paths under it;
    - a re-run → 0, empty stdout, only the "already up to date" note.
  - **AC41:** a hand-written list doc {1111, 7777} over `write_clean_twin_tree` → exactly as T9's runner test, through the whole command.
  - **AC42:** `-v --dir-per-title` → `mangadex-dl: output directory: out/Clean Title` after `list:1: 1111…`.
  - **AC43:** a flat run, then a `--dir-per-title` run into the same DIR → the flat files are byte-identical and their mtime unchanged; the new files are under `out/Clean Title/`.
  - **AC46 (real-run variant, Spec issue 4):** `TempDir` DIR holding a regular file `Clean Title`, L2, real run → 7, item 1's message, no `stopped at`, the listing unchanged.
  - **AC24 (cli mirror):** DIR a regular file → 1, `stopped at item 1: the error above affects every title; 1 title not run`.
  - **`io.out` = `/dev/full`** with a two-item list → the run stops after item 1, `stopped at item 1: cannot write to stdout; 1 title not run`.
  - **Color:** `--color=always` colors `error:` on stderr; stdout and JSON never contain ESC.

### T11: Registration, root text, `--title`, exit rows 4/7, footers, help goldens
- **Goal:** make `mdlist` reachable and finish the help surface, with every help golden blessed in one round.
- **Files:**
  - edit `src/app/cli.cpp`, `src/app/exit_help.hpp`, `src/app/cmd_download.cpp`, `src/app/cmd_batch.cpp`, `tests/cli/cli_test.cpp`, `tests/cli/download_cli_test.cpp` and `tests/cli/batch_parse_test.cpp`;
  - create `tests/cli/mdlist_parse_test.cpp` and `tests/e2e/cases/mdlist.cmake` (the `help` case only).
- **Owns:** exactly those 9 files.
- **Build files for build-fixer:** none.
- **What changes:**
  - **`cli.cpp`:**
    - `commands.push_back(register_mdlist(app, inv.selected));` after `batch`;
    - `failure_text`: `unknown command '<x>' (commands: download, batch, mdlist)`;
    - `batch_got_title` becomes `title_option_owner(app, error) -> std::optional<std::string_view>`, returning `"batch"`/`"mdlist"`:
      - for `batch`, the message is unchanged, byte for byte;
      - for `mdlist`: `mdlist has no --title option; to name a title yourself, download it on its own with 'mangadex-dl download <title> --title ...'` plus the hint (V7c).
  - **`exit_help.hpp`:**
    - row 4: `title or list not found on MangaDex (a private list counts)`;
    - row 7: `batch/mdlist: titles failed in more than one way (see per-title results)`;
    - both ≤80 columns; the drafts go to the cli-ux board.
  - **`cmd_download.cpp`:** the footer line becomes `For many titles at once, see 'mangadex-dl batch --help' (a CSV file)` / `or 'mangadex-dl mdlist --help' (a MangaDex list).`
  - **`cmd_batch.cpp`:** `batch_footer` gains `For a MangaDex list (MDList) instead of a file, see 'mangadex-dl mdlist --help'.` plus a blank line before `Examples:`.
  - **`mdlist.cmake` (T11 part):** `cli_e2e_case(NAME help ARGS mdlist --help EXPECT_EXIT 0 EXPECT_STDOUT_REGEX "\nmangadex-dl mdlist \\[OPTIONS\\] LIST \\[DIR\\]\n" EXPECT_STDERR_EMPTY EXPECT_NO_ANSI GOLDEN)`.
- **Dependencies:** T10.
- **Done when:**
  - the human has blessed `edge.help`, `download.help`, `batch.help` and `mdlist.help`;
  - `nice pixi run check` is green;
  - both reviewers approve.
- **Tests:**
  - **cli (`mdlist_parse_test.cpp`, through `run_cli`):**
    - `mdlist --help` contains the LIST footer, the `--dir-per-title` line after `--fail-fast`, and rows 4/7, and has no `--api-base` (V7a);
    - `mdlist L out --json -q --dir-per-title` → the globals apply, with the clean fixture (V7b);
    - `mdlist L --title X` and `--title=X` → 2 with the mdlist message and hint (V7c);
    - `mdlist` with no LIST → 2 with `Run 'mangadex-dl mdlist --help'` (V7d);
    - `-j 9` → 2;
    - a `download` smoke and a `batch` smoke still work.
  - **cli (edits):**
    - the root unknown-command text (`cli_test.cpp:80`, `download_cli_test.cpp:446`);
    - row 7 (`cli_test.cpp:62`, `batch_parse_test.cpp:224`);
    - the download footer (`batch_parse_test.cpp:247`) (AC15).
  - **e2e:** `mdlist.help` (GOLDEN, AC16). The `edge.help`, `download.help` and `batch.help` cases get re-blessed.

### T12: E2E cases, edge-suite additions and committed inputs
- **Goal:** the spec's `[e2e]`, `[edge]` and `[tty-pty]` criteria against the installed binary.
- **Files:**
  - edit `tests/e2e/cases/mdlist.cmake` (append), `tests/e2e/cases/_edge.cmake` (append an `mdlist-*` block; the `EDGE_*` knobs are unchanged) and `tests/e2e/inputs/README` (document `mdlist/` and the generated list documents per tree);
  - create `tests/e2e/inputs/mdlist/Clean Title` (a regular file).
- **Owns:** exactly those 4 files.
- **Build files for build-fixer:** none.
- **Dependencies:** T11.
- **Done when:**
  - the human has blessed `mdlist.dry-run` and `mdlist.json-dry-run`;
  - `nice pixi run check` is green;
  - `nice pixi run release-test` is green: every new case on the hardened install;
  - both reviewers approve.
- **Tests:** see the Test plan's E2E and edge tables.

### T13: User documentation (README)
- **Goal:** the spec's "README and docs" section.
- **Files:** edit `README.md`.
- **Owns:** `README.md`.
- **Build files for build-fixer:** none.
- **What changes:** every bullet of spec "README and docs":
  - the intro line and the contents link;
  - the section "A MangaDex list: `mdlist`", placed after "Many titles: `batch`": the LIST forms including the trailing slash, no login, order, dedupe, the 10 000 cap, the content-filter caveat, caching, flat vs `--dir-per-title`, the rename / `--lang` / mode-switch notes, the stderr shape, the JSON (`list` object, `line` = position, `dir`), list-level 3/4 vs aggregate, re-runs, examples;
  - exit rows 1, 3, 4 and 7;
  - the Output, Interruption, Limits and Network notes.
  - **The caching sentence** depends on V11. Until AC35 settles it, write the cautious "may take a while to appear". Rate wording per V10.
- **Dependencies:** T11 (final help text and messages).
- **Done when:** `nice pixi run check` is green (format/lint only touch C++, but the gate must still pass) and `quality-reviewer` approves. `memory-safety-reviewer` has no C++ to review here; it confirms "no C++ changed".
- **Tests:** none (docs only).

---

## Parallelization plan

**Batch 1 (parallel): T1, T2 · Batch 2 (parallel): T3, T4, T5, T6 · Batch 3 (parallel): T7, T8, T9 · Batch 4: T10 · Batch 5: T11 (cli.cpp registration) · Batch 6 (parallel): T12, T13**

- **Batch 1:** additive headers (T1) and a pure core refactor (T2). The Owns sets are disjoint, and T2 uses no T1 symbol.
  - T1 is the **only** track touching `include/mangadex_dl/*`.
  - T1's single app line (`is_fatal` in `cmd_batch.cpp`) is there because `-Wswitch` demands it; T3 takes the file over in batch 2.
- **Batch 2:** four tracks with disjoint Owns.
  - T3 is the second pure refactor, app only.
  - T4, T5 and T6 are core, each in its own files. Two different `title_dir`-ish names were chosen to avoid confusion: `src/core/title_dir.cpp` (T5) and `src/core/download/dir_per_title.cpp` (T6).
  - Tests call only their own or batch-1 symbols:
    - T6 uses a test-local `FakeResolver`, not T5's registry;
    - T5's AC36 property test uses only existing `sanitize_file_stem` and `validate_list_dir`.
- **Batch 3:** three tracks with disjoint Owns.
  - T7 is core plus `tests/support/mdx_fixture.hpp`; T8 is core; T9 is app plus `tests/support/mdx_twin.hpp`.
  - **Shared-header caution:** T9's tests include `mdx_fixture.hpp` while T7 extends it (additively, last members). A half-written edit can break T9's build: that is a coordination event, never a fix in T9.
  - T9 must not depend on T7's list documents: its jobs are hand-built.
- **Batch 4:** T10 alone. It needs T7, T8 and T9 at link and test time.
- **Batch 5:** T11, the late registration task and the sole owner of `src/app/cli.cpp` and `exit_help.hpp`. It also owns the second edits of `cmd_download.cpp` (untouched since the batch feature) and `cmd_batch.cpp` (T1 in batch 1, T3 in batch 2). It runs bless round 1.
- **Batch 6:** T12 (E2E, `_edge.cmake`) and T13 (README). Disjoint, and both need the registered binary and the final text. T12 runs bless round 2.
- **Parallelism stays modest:** at most four tracks, all sharing `build/default/dev` under the lock.

**Serialization points and who touches them:**

| Serialization point | Task |
|---|---|
| shared public headers `include/mangadex_dl/*` (`error.hpp`, `exit_codes.hpp`, `options.hpp`, `model.hpp`, `download.hpp`, `batch.hpp`, `api_client.hpp`, new `list_id.hpp`, `title_dir.hpp`, `mdlist.hpp`) | T1 only |
| `src/app/cli.cpp` | T11 only |
| `tests/e2e/cases/_edge.cmake` (a shared file) | T12 only |
| top-level `CMakeLists.txt`, `CMakePresets.json`, `pixi.toml`/`pixi.lock`, `tests/CMakeLists.txt`, `tests/e2e/CliE2E.cmake`, `tests/e2e/run_case.cmake`, `scripts/*`, `.github/workflows/*` | none |

Also shared, but not on the list; each has exactly one owner per batch:

| File | Owners, by batch |
|---|---|
| `src/app/cmd_batch.cpp` | T1 (B1), T3 (B2), T11 (B5) |
| `src/app/cmd_batch_jobs.*` | T3 (B2), T9 (B3) |
| `src/app/cmd_download_run.*` | T9 |
| `src/app/exit_help.hpp` | T11 |
| `src/core/batch_plan.cpp` | T8 |
| `src/core/options.cpp` | T4 |
| `src/core/api_client.cpp` | T7 |
| `src/core/download/internal.hpp` | T6 |
| `tests/support/mdx_fixture.hpp` | T7 |
| `tests/support/mdx_twin.hpp` | T9 |
| `tests/unit/interfaces/headers_test.cpp` | T1 |
| `tests/e2e/inputs/README` | T12 |
| `README.md` | T13 |

---

## Test plan (per tier)

### AC → task → tier

| AC | Task | Tier | AC | Task | Tier |
|---|---|---|---|---|---|
| 1 | T4 | unit | 25 | T12 | edge |
| 2 | T2, T4 | unit | 26 | T12 | edge |
| 3, 4 | T7 | unit | 27 | T12 | edge |
| 5 | T7 | unit | 28 | T12 | edge |
| 6 | T8 | unit | 29 | T12 | edge |
| 7 | T5 | unit | 30 | T10 (cli), T12 (tty-pty) | cli, e2e |
| 8 | T5 | unit | 31–35 | — | human (User Test Plan) |
| 9 | T10 | cli | 36 | T5 (property), T6 (lang) | unit |
| 10 | T8 (unit), T10 (cli), T11 (`--title` via `run()`) | unit, cli | 37 | T5 | unit |
| 11 | T10 | cli | 38 | T6 | unit |
| 12 | T5, T7 (unit), T10 (cli); see Spec issue 3 | unit, cli | 39 | T5 (core), T9 (runner) | unit, cli |
| 13 | T10 | cli | 40 | T10 | cli |
| 14 | T10 | cli | 41 | T9, T10 | cli |
| 15 | T4, T8, T11 | cli | 42 | T10 | cli |
| 16 | T11 | e2e golden | 43 | T10 | cli |
| 17–24 | T12 | e2e | 44, 45 | T12 | e2e |
| | | | 46 | T10 (real run), T12 (`--dry-run`) | cli, e2e |
| | | | 47 | T12 | edge |
| | | | 48 | — | human |

### Unit (Catch2 against `mangadex_dl::core`)

| File | Task | Covers |
|---|---|---|
| `tests/unit/interfaces/headers_test.cpp` | T1 | `row_dir` → 1, constants, abstract resolver, designated-init smoke |
| `tests/unit/mangadex_url/mangadex_url_test.cpp` | T2 | the helper with both paths; the view borrows the input |
| `tests/unit/list_id/list_id_test.cpp` | T4 | AC1, AC2 |
| `tests/unit/batch/batch_source_test.cpp` | T5 | AC7, AC12 (unit), AC39 (core) |
| `tests/unit/batch/batch_json_list_test.cpp` | T5 | AC8, V17 |
| `tests/unit/title_dir/title_dir_test.cpp` | T5 | AC36 (property), AC37, V12 |
| `tests/unit/planner/planner_file_stem_test.cpp` | T6 | `file_stem`; AC36 (lang) |
| `tests/unit/download/download_dir_per_title_test.cpp` | T6 | AC38, V13, Spec issue 2 |
| `tests/unit/mdlist/mdlist_parse_test.cpp` | T7 | AC3, AC4, V18 |
| `tests/unit/mdlist/mdlist_fetch_test.cpp` | T7 | AC5, AC12 (fetch), V20, V21 |
| `tests/unit/support/mdx_fixture_list_test.cpp` | T7 | V9, V19 |
| `tests/unit/mdlist/mdlist_plan_test.cpp` | T8 | AC6, AC10 (unit) |

Existing `unit.manga_id*`, `unit.options*`, `unit.batch*`, `unit.download*`, `unit.planner*`, `unit.api_client*`, `unit.report*` and `unit.support*` stay green **unmodified**. That is the proof that T2, T5, T6 and T7 preserve behaviour.

### In-process CLI (`tests/cli/`, flat)
- **`list_hint_download_test.cpp` (T4) and `list_hint_batch_test.cpp` (T8):** AC15, the two new exit-2 hints.
- **`batch_jobs_list_test.cpp` (T9):** item wording, AC39, AC41 (runner level), the flat-collision advice, csv unchanged.
- **`mdlist_cli_test.cpp` (T10):** AC9–AC14, AC24 mirror, AC30 (cli), AC40–AC43, AC46 (real run), `/dev/full`, color.
- **`mdlist_parse_test.cpp` (T11):** V7a–V7d through `run()`, help content, smokes.
- **Edited (T11):** `cli_test.cpp`, `download_cli_test.cpp`, `batch_parse_test.cpp`.
- **TSan:** T7 runs `nice pixi run tsan -R 'unit\.mdlist'`; T9 runs `-R 'cli\.(batch|mdlist)'`; T10 runs `-R 'cli\.mdlist'`. The board runs `nice pixi run tsan -R '(cli|unit)\.(batch|mdlist|download)'`.

### E2E cases: `tests/e2e/cases/mdlist.cmake` (tests `e2e.mdlist.<case>`)
Fixture mode (`--api-base file://{{FIXTURES}}/<tree>`) unless noted; `out` as DIR; exact `EXPECT_EXIT`. The list ids are `a0000000-0000-4000-8000-0000000000NN`.

| Case | Task | Asserts |
|---|---|---|
| `help` | T11 | 0, stderr empty, no ANSI, GOLDEN (AC16) |
| `clean` | T12 | L1 → 0, stdout = the two clean paths, stderr empty, `EXPECT_FILES` + `EXPECT_EPUB`, `EXPECT_NO_GLOB "out/.*.part-*"` (AC17) |
| `form-upper`, `form-url-slug`, `form-url-empty-slug`, `form-www`, `form-no-scheme` | T12 | the same stdout regex as `clean` (AC17) |
| `mixed-exit-7` / `mixed-exit-7-json` | T12 | nochapters, L8 → 7; stderr has `list:1: 5555…: ` and `list:2: 6666…: ` lines and the closing line; JSON: `EXPECT_JSON`, `"no_chapters"`, `"not_found"`, `"list": {` (AC18) |
| `fail-fast` | T12 | L8 `--fail-fast` → 5, `stopped at item 1: --fail-fast; 1 title not run` (AC18) |
| `incomplete` | T12 | L9 → 6, failed file `EXPECT_ABSENT`, no `.part-` (AC19) |
| `uniform-not-found` | T12 | L2 → 4, the clean files written (AC19) |
| `unknown-list` / `unknown-list-json` | T12 | `…ff` → 4, the exact list message, stdout empty (also with `--json`), `EXPECT_ABSENT out` (AC20) |
| `bad-id` / `bad-id-json`, `id-mismatch` | T12 | L5 / L6 → 3, `list a0000000-…: malformed`, **no** `check your network`, stdout empty, `out` absent (AC20) |
| `empty` / `empty-json` | T12 | L3 → 0, `warning: list a0000000-0000-4000-8000-000000000003 has no titles`; JSON `"titles": \[\]` plus `"list"`; `out` absent (AC21) |
| `twice` | T12 | L4 → 0, `list:2: 1111…: same as item 1; skipped`, one set of files (AC21) |
| `dry-run`, `json-dry-run` | T12 | L1 `--dry-run` → 0, `EXPECT_ABSENT out`, GOLDEN (AC22) |
| `network` | T12 | `--api-base http://127.0.0.1:1` → 3, the network remedy, stdout empty, `out` absent, `LABELS needs-libcurl`, `TIMEOUT 300` (AC23) |
| `dir-is-file` | T12 | L2, DIR `{{INPUTS}}/README` → 1, `stopped at item 1: the error above affects every title; 1 title not run` (AC24) |
| `usage-title-url`, `usage-csv-path`, `usage-title-option` | T12 | → 2, stdout empty, the exact messages plus the `mdlist --help` hint (AC10 at e2e) |
| `tty-progress` | T12 | `TTY`, L2 → 4, merged output matches `\(1/2\) \[1/2\] Vol\. 1:` (AC30, V23) |
| `tty-json-no-progress`, `tty-quiet-no-progress`, `tty-term-dumb` | T12 | no `\(1/2\)`, `EXPECT_NO_ANSI` (AC30) |
| `tty-no-color-error` | T12 | `TTY ENV NO_COLOR=1`, L8 → 7, `EXPECT_NO_ANSI` (AC30) |
| `dir-per-title` | T12 | L1 `--dir-per-title` → 0, stdout exactly `out/Clean Title/Clean Title - Vol\. 01\.epub\nout/Clean Title/Clean Title - Vol\. 02\.epub\n`, `EXPECT_FILES` + `EXPECT_EPUB` under it, no `.part-` (AC44, V22) |
| `dir-per-title-dry-run` | T12 | the same paths, stderr empty, `EXPECT_ABSENT out` (AC45) |
| `dir-per-title-row-dir-file` | T12 | L2 `--dir-per-title --dry-run`, DIR `{{INPUTS}}/mdlist` → 7; stderr `list:1: 11111111-…: cannot open directory '[^']*/mdlist/Clean Title': Not a directory` and `list:2: 6666…`; no `stopped at` (AC46, Spec issue 4) |

### Edge-suite additions (`tests/e2e/cases/_edge.cmake`, T12 only; tests `e2e.edge.mdlist-*`)

| Case | Asserts |
|---|---|
| `mdlist-stdin-null`, `mdlist-stdin-closed` | L1 `--dry-run` with `STDIN NULL` / `CLOSED` → 0, the same stdout, never 70 (AC25) |
| `mdlist-dev-full` | L1 `--dry-run`, `STDOUT_FILE /dev/full` → 1, `^mangadex-dl: write error` (AC26) |
| `mdlist-sigpipe`, `mdlist-sigpipe-parent-ignores` | L10 on bulk, `--dry-run --layout chapters`, `PIPE_HEAD 1` (± `PARENT_IGNORES_SIGPIPE`) → SIGPIPE, stderr empty (AC27) |
| `mdlist-invalid-utf8-list-lc-c` | LIST `{{INVALID_UTF8}}`, `ENV LC_ALL=C LANG=C` → 2, the fixed message, input not echoed, not 70 (AC28) |
| `mdlist-invalid-utf8-dir-json` | L1 `--json --dry-run`, DIR `{{INVALID_UTF8}}` → 0, `EXPECT_JSON` (AC28) |
| `mdlist-utf8-dir`, `mdlist-utf8-dir-lc-c` | DIR `Zoë`, `--dry-run` → stdout contains `Zoë/` under C.UTF-8 and C (AC28) |
| `mdlist-json-clicolor-force` | `--json --dry-run`, `CLICOLOR_FORCE=1` → `EXPECT_JSON EXPECT_NO_ANSI` (AC29) |
| `mdlist-pipe-stderr-no-ansi` | L8, piped → 7, `EXPECT_NO_ANSI` (AC29) |
| `mdlist-json-dir-per-title-clicolor-force` | `--json --dry-run --dir-per-title`, `CLICOLOR_FORCE=1` → `EXPECT_JSON EXPECT_NO_ANSI`, stdout matches `"dir": "out/Clean Title"` (AC47) |

The `EDGE_*` knobs stay on `download`.

### Sanitizer-control additions
None. The feature adds no new risk class:
- the new thread is one `InterruptBridge` poller, a pattern `run_download` already uses;
- there is no C-string interop and no raw buffers;
- the list-JSON views are covered by the `dangling_view_heap` and `dangling_view_scope` controls (both in `tests/sanitizer-controls/`).

Evidence for `memory-safety-reviewer`: those controls (plus `heap_overflow`, `signed_overflow`, `leak`, and `data_race` under tsan) fired in the T7/T10 gate runs, and the multi-item, list-fetch and `--dir-per-title` cases exercised the new paths.

### Release-test
- There is no release-surface change: no deps, no link change; NEEDED and the glibc floor are untouched.
- T12's Done-when and the board run `nice pixi run release-test`, so every `e2e.mdlist.*` and `e2e.edge.mdlist-*` case runs on the hardened static-libstdc++ install.
- `e2e.mdlist.network` needs libcurl. Exclude it with `-E '^e2e\.mdlist\.network$'` on a host without it.
- The board runs `nice pixi run -e gcc check` once more over the final tree. T1, T6 and T10 already do so per track.

### Goldens that will change (human-blessed: `pixi run bless '<regex>'`)

| Golden | Why | Triggered by |
|---|---|---|
| `edge.help` | root `SUBCOMMANDS:` gains `mdlist`; exit rows 4 and 7 reworded | T11 |
| `download.help` | rows 4 and 7; the footer pointer becomes two lines (batch + mdlist) | T11 |
| `batch.help` | rows 4 and 7; the new footer line before `Examples:` | T11 |
| `mdlist.help` (new) | the new `mdlist --help` | T11 |
| `mdlist.dry-run` (new) | planned paths for L1 | T12 |
| `mdlist.json-dry-run` (new) | batch's document plus `list` for an L1 dry run | T12 |

Bless requests:
- T11: `'^e2e\.(edge|download|batch|mdlist)\.help$'` (reason: new subcommand plus the exit-row rewording);
- T12: `'^e2e\.mdlist\.(json-)?dry-run$'` (new GOLDEN cases).

**Must not change:** `edge.version`, `download.dry-run-clean`, `download.json-dry-run`, `batch.dry-run` and `batch.json-dry-run`. Any diff in these, and **any** golden diff in T1–T10, is a regression, never a bless.

---

## Memory-safety watch-list (for `memory-safety-reviewer`)

**Borrowed resolver and engine context**
1. **`DownloadServices::title_dir` (`TitleDirResolver*`).** It is borrowed and nullable, and set per item inside `run_title` to `&resolver_`, a member of `ConsoleTitleRunner`, which outlives every `run_batch` call.
   - It is copied into `RunContext::title_dir` and called at most once, on the `run_download` thread, between `plan_phase` and `execute_phase`.
   - It is never stored past `run_download`, and never reached by a FetchPool worker or the reporter.
   - Confirm that nothing captures it in a lambda.
2. **`resolve(std::string_view stem, const MangaId&)`.** `stem` borrows `Plan::file_stem` for the call only. `TitleDirRegistry` copies it into owned `std::string` keys, and `TitleDirChoice` returns owned strings.
   - No `string_view` key points into a `Plan`, a `RunReport` or a temporary.
   - The suffix is built from `manga.str().substr(...)` into a new string. `manga.str()` is never bound from a temporary `MangaId`.
3. **The effective options and context copy in `run_download`.**
   - `DownloadOptions effective` and `RunContext title_ctx` are locals declared in that order, after `ctx`, and live until `execute_phase` returns.
   - `title_ctx.options = &effective`; the copied `std::stop_source` shares state, so the bridge still stops it.
   - Nothing keeps `&effective` after return. The original `ctx.options` still points at the caller's options.
4. **Resolver per-item state.** `ConsoleTitleDirResolver::begin(item)` resets the last claim before every `dir_per_title` row, so a row whose run ended before the plan (exit 3/4) never inherits the previous row's name for `outcome.dir`.

**String views over list JSON**

5. **`parse_custom_list`:**
   - `FieldReader` returns owned `std::optional<std::string>`; every manga id becomes a `MangaId` (an owned string) before the parser returns;
   - the name cut works on an owned copy;
   - no `string_view` into `json` (or into the `Result<std::string>` body held by `fetch_custom_list`) escapes;
   - `CustomList` has no view members.
6. **`ListId::str()` and `MangaId::str()`** are LIFETIMEBOUND views. Never `ListId::parse(x)->str()`. `list_request_url` and `list_web_url` copy into the formatted string.
7. **`extract_mangadex_uuid`** (T2) returns a view into its argument. `MangaId::parse` and `ListId::parse` copy it (`lowercase_ascii`) before the argument dies, and `looks_like_*` keep nothing.
8. **The `is_custom_list_error_body` parse** runs on the error response's body, owned by `perform`'s result for the call, with its own 64 KiB limit. Nothing is retained.

**Registries keyed by names**

9. **`TitleDirRegistry`:**
   - `std::map<std::string, Holder, std::less<>>` plus `std::map<MangaId, std::string>`: owned keys and values;
   - heterogeneous lookup with `string_view` only for `find`, never stored;
   - not thread-safe by contract; used on one thread.
10. **`OutputRegistry`** is unchanged (keys owned, `lexically_normal`). In `--dir-per-title` mode its keys include the title directory.

**Extraction of `ConsoleTitleRunner` (T3) and its extension (T9)**

11. **Pointers in the moved runner.** It holds pointers to `settings`, `backend`, `agent`, `io`, `policy`, `registry`, plus (T9) `dirs`.
    - In `run_batch_jobs` the order is `registry` → `dirs` → `runner`, all after the caller's `handlers` → `agent` → `backend`, and all live until `run_batch` returns.
    - Check that T3 moved the code verbatim (no copy of `settings` where a pointer was kept, no temporary `BatchSettings`) and that `run_batch_command`'s `settings`/`jobs` (Results) outlive the call.
12. **`RowContext` is moved by value into `ConsoleObserver`** (unchanged). The mdlist prefix `"list:<n>: <uuid>: "` is a local `std::string` copied into it, never borrowed.
13. **`std::optional<ListSummary>`** is passed by `const&` from `run_mdlist_command`'s frame into `run_batch_jobs` and `batch_to_json`; nothing stores it.
14. **`std::span<const BatchJob>`** over `plan_list`'s vector: the vector is not mutated while the span is live. `mark_duplicates` (T8) works on indices, never pointers or iterators into the growing vector.

**CLI11 bindings and callbacks**

15. **`MdlistCommand` owns `RawMdlistArgs args_`.** LIST → `args_.list`; DIR and the shared options → `args_.defaults` via `bind_*` (the **member**); `--fail-fast` and `--dir-per-title` → `args_` members.
    - The Command lives in `run()`'s `commands` vector, declared before the `CLI::App`.
    - The selecting callback captures `[slot = &selected, self = command.get()]` by value.
    - `run_mdlist_command` takes `const RawMdlistArgs&` and copies what it keeps into `MdlistSettings`.

**Threads and process-wide state**

16. **The list fetch (`fetch_custom_list`).**
    - Order: `stop_source` → `ApiClient` (report = false, so **no reporter thread**; assert `endpoints_for(…, false)`) → `InterruptBridge` (its poller is the one new thread).
    - Reverse destruction joins the bridge first, then the client, all before `fetch_custom_list` returns, so before item 1.
    - The `TraceSink` (app `StderrTrace`, mutex-guarded) and the Backend's transport and clock outlive the call.
    - TSan: `unit.mdlist*` and `cli.mdlist*`.
17. **Interrupt state.** One `ScopedInterruptHandlers` covers the fetch and every item, plus a per-item `InterruptBridge` inside `run_download`. The fetch's bridge is gone before the first per-item bridge starts, so two pollers never overlap. Tests reset the flag with RAII; the second-call test proves the handlers were released.
18. **`ListFetchServices` and `DownloadServices` are filled member by member** (the analyzer false positive). `jitter` is a fresh `make_jitter()` per fetch, captured by value.

**Index and size math**

19. **The name cut and the display cut.** `max_bytes` is a `size_t`; back off while `(byte & 0xC0) == 0x80`; never cut below 0; `substr` bounds are checked.
20. **The suffix length:** ≤150 + 39 bytes, checked by T5's test; `validate_list_dir` re-checks the 255-byte segment limit in the engine (T6).
21. **`max_list_titles`:** counted as manga entries are accepted, compared with `>` before `push_back`, never allocating past the cap + 1.

---

## Rollout & risk

**CLI contract**
- `download` and `batch` keep every flag, exit code, stdout byte and `--json` byte.
  - A `batch` JSON document never gets `list`, and its `format_version` stays 1.
  - The unchanged goldens (`download.dry-run-clean`, `download.json-dry-run`, `batch.dry-run`, `batch.json-dry-run`) and the unmodified `cli.download*`/`cli.batch*` suites guard this.
- **Contract changes, all on stderr or help:**
  - root `unknown command '<x>' (commands: download, batch, mdlist)`;
  - `download <list URL>`: still exit 2, with a new message (only for list URLs);
  - **`batch <list URL>`: exit 1 → 2**, with the mdlist hint. This is a nonsensical input, given the same treatment `batch` already gives a title URL. Call it out in the PR body.
  - Help: exit rows 4 and 7 reworded in every `--help`, plus the footer pointers.
- **No new exit code** (Q2). Exit 4 and 3 gain list-level meanings, documented in the README with how to tell them from the aggregates (no JSON, no `list:<n>:` prefix).
- **New surface:** `mdlist LIST [DIR]` with batch's options, `--fail-fast` and `--dir-per-title`. The JSON is batch's plus the additive `list`.
- No migration and no state file.

**Riskiest task: T6** (the `run_download` engine of the shipped `download`). De-risked by:
- the flag-gated branch (`dir_per_title` false → the exact old path), with the resolution code in a new file;
- `execute_phase.cpp` left untouched;
- byte-identical report JSON with the flag off in a unit test;
- the unmodified download and planner suites, and both env gates;
- no golden diff allowed.

**Second risk: T3** (moving batch's phase 2). Mitigated by:
- a verbatim move with no test edits;
- `cmp` of the installed help;
- TSan over `cli.batch`.

**Third risk: T7** (an untrusted JSON parser plus a new API call). Mitigated by:
- `parse_untrusted` with the separator cap;
- table-driven hostile tests;
- self-contained messages that never echo response bytes;
- `security-reviewer` on the board (untrusted JSON, server-derived directory names);
- at `/harden`, a libFuzzer target for `parse_custom_list` can follow (fuzz is off in Project specifics).

**Behaviour choices for the gate:**
- Spec issue 2 (exit 5 claims no directory);
- Spec issue 6 (malformed entries → exit 3);
- Spec issue 4 (AC46 as a dry run at E2E).

**Smaller risks:**
- **Rate limit at the list/item-1 boundary (V10):** one request may come up to 250 ms early. AC31 (human) watches for 429s.
- **CDN staleness (V11):** README wording only. AC35 settles it.
- **Private lists (V1):** if refuted (say 403 without a CustomList body), only T7's mapping changes.
- **Draft wording for the cli-ux board:**
  - rows 4 and 7;
  - the footer pointers;
  - `mdlist --help`;
  - the item wording;
  - the directory-clash warning;
  - the `--title` message;
  - the list-level messages.

---

## Refactor triggers

| [VERIFY] | If refuted, these tasks change |
|---|---|
| V1 (private list → 404 or 401/403 with a CustomList body) | T7: `fetch_list_document`'s status mapping (for example a 403 shape to recognise); T13 and spec wording "does not exist or is private"; AC32's expectation. No other task |
| V7a (shared binders on a third subcommand; help order) | T10: duplicate the `add_option` calls in `MdlistCommand::bind`, with the help strings hoisted into shared `constexpr std::string_view`s, so they can't drift; T11's help assertions follow |
| V7b (globals after LIST/DIR) | T11 tests, the README (T13) and the help examples put the globals before `mdlist`; spec wording back to `cli-designer`. No core change |
| V7c (leftover `--title` visible via `remaining(true)` once mdlist is reached) | T10: add a hidden `--title` option on `mdlist` that fails with exit 2 and the mdlist message; T11 drops the `cli.cpp` generalisation for mdlist and tests the hidden option |
| V7d (RequiredError hint names mdlist) | T11: `help_hint` special-cases the reached subcommand differently; tests follow |
| V8a (extraction byte-identical) | T3 is redone (a move only) before batch 3. Any intended difference is a plan change |
| V8b, V8c (remedy keyed on exit code; empty sub-dir pre-flight) | T7/T10 already route around V8b; if `with_remedy` turns out to inspect more, T10's content-error path re-checks. V8c refuted: T8's `plan_list` sets a sub-dir, or T9 calls a DIR-only check |
| V9, V19 (fixture mapping, 404 on a missing doc) | T7: adjust `write_fixture_tree`'s path (for example `api/list/<id>/…`) or `list_request_url`; T12's unknown-list expectation |
| V10 (rate at the boundary) | T13 wording only; or a follow-up that shares the limiters in `Backend` (a public-header change, out of this plan) |
| V12 (stems pass `validate_list_dir`) | T5 or T6: a dedicated `title_dir_name(stem, manga)` that re-sanitises with `is_unsafe_code_point` (replace with `_`) before claiming; T5's property test targets it; the engine's defence-in-depth check stays |
| V13 (`OutputDir::open` creates both levels; `open_if_exists` with DIR missing) | T6: create DIR first (or `open_if_exists(DIR)` first in dry runs) in `dir_per_title.cpp`; tests follow |
| V14 (only two Errc switches; `-Wswitch` in both compilers) | T1 adds the missed cases (whoever owns that file joins T1's batch, or T1 widens its Owns) |
| V15 (additive members byte-identical) | The task whose golden or test moved stops; the orchestrator re-plans that member (for example `Plan::file_stem` → recompute `sanitize_file_stem` in T6 instead) |
| V16 (twin covers) | T9: `write_clean_twin_tree` changes (for example writing a per-manga cover document is impossible with the shared map, so tolerate the warnings: AC41 asserts with regex and `-q`) |
| V17 (ordered_json order, ESC escape, replace) | T5: build `list` from strings with explicit escaping checks, or assert the semantics instead of bytes; AC8 wording back to the designer |
| V18 (strict parse rejects invalid UTF-8) | T7: validate the name with `decode_utf8_at` before cutting, and replace invalid bytes with U+FFFD; AC3 gains a row |
| V20, V21 (perform keeps the body; FakeTransport hooks) | T7: read the 401/403 body by a dedicated non-retrying `get`, or extend the test seam in `tests/support/fake_transport.hpp` (then T7 also owns it) |
| V22, V23 (driver paths with spaces; TTY progress) | T12: use regex-only stdout checks instead of `EXPECT_FILES` for spaced paths, or move AC44 to the cli tier; a TTY miss moves AC30 to cli plus human |
| V24 (tsan `-R` forwarding) | T7/T9/T10 run the full `nice pixi run tsan` instead (longer) |
| V11 (CDN purge) | T13's caching sentence and AC35's expectation only |

---

## Relevant paths

- Spec: `/home/user/Mangadex-dl/docs/design/list-spec.md`
- Prior plans: `/home/user/Mangadex-dl/docs/design/batch-plan.md`, `/home/user/Mangadex-dl/docs/design/download-plan.md`, `/home/user/Mangadex-dl/docs/design/skip-existing-plan.md`
- Frozen headers (T1):
  - `/home/user/Mangadex-dl/include/mangadex_dl/{error,exit_codes,options,model,download,batch,api_client}.hpp`
  - new: `/home/user/Mangadex-dl/include/mangadex_dl/{list_id,title_dir,mdlist}.hpp`
- Code read for this plan:
  - core: `/home/user/Mangadex-dl/src/core/manga_id.cpp`, `/home/user/Mangadex-dl/src/core/api_client.cpp`, `/home/user/Mangadex-dl/src/core/batch_plan.cpp`, `/home/user/Mangadex-dl/src/core/batch_run.cpp`, `/home/user/Mangadex-dl/src/core/batch_json.cpp`, `/home/user/Mangadex-dl/src/core/download.cpp`, `/home/user/Mangadex-dl/src/core/download/internal.hpp`, `/home/user/Mangadex-dl/src/core/download/plan_phase.cpp`, `/home/user/Mangadex-dl/src/core/download/execute_phase.cpp`, `/home/user/Mangadex-dl/src/core/planner.cpp`, `/home/user/Mangadex-dl/src/core/filename.cpp`, `/home/user/Mangadex-dl/src/core/json_util.hpp`, `/home/user/Mangadex-dl/src/core/net/fixture_transport.cpp`, `/home/user/Mangadex-dl/src/core/fs/row_dir.cpp`;
  - app: `/home/user/Mangadex-dl/src/app/cmd_batch.cpp`, `/home/user/Mangadex-dl/src/app/cmd_download_run.{hpp,cpp}`, `/home/user/Mangadex-dl/src/app/cmd_download_options.cpp`, `/home/user/Mangadex-dl/src/app/cmd_download_output.hpp`, `/home/user/Mangadex-dl/src/app/cli.cpp`, `/home/user/Mangadex-dl/src/app/exit_help.hpp`;
  - tests: `/home/user/Mangadex-dl/tests/support/mdx_fixture.hpp`, `/home/user/Mangadex-dl/tests/support/fake_transport.hpp`, `/home/user/Mangadex-dl/tests/e2e/fixtures/fixture_gen.cpp`, `/home/user/Mangadex-dl/tests/e2e/cases/_edge.cmake`, `/home/user/Mangadex-dl/tests/e2e/cases/batch.cmake`.
- Tests pinning text that T11 changes: `/home/user/Mangadex-dl/tests/cli/cli_test.cpp` (:62, :80), `/home/user/Mangadex-dl/tests/cli/download_cli_test.cpp` (:446), `/home/user/Mangadex-dl/tests/cli/batch_parse_test.cpp` (:224, :247).
- Goldens: `/home/user/Mangadex-dl/tests/e2e/golden/{edge,download,batch}.help.stdout` change; `mdlist.{help,dry-run,json-dry-run}.stdout` are new.
