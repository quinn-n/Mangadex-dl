# Implementation plan: `download` skips up-to-date EPUBs and rebuilds incomplete ones with page reuse

Design: `/home/user/Mangadex-dl/docs/design/skip-existing-spec.md` (rev 2, APPROVED). Gate decision 5 (re-pick on `--group`) wins over the body's "keep the held release" rule wherever they conflict. Prior plan format: `/home/user/Mangadex-dl/docs/design/download-plan.md`.

What I checked on 2026-10-19 (orientation only; the fact-checker re-confirms):
- `pixi list`: CLI11 2.7.2, nlohmann_json 3.12.0, Catch2 3.16.1, cmake 4.4.4, clang 23.1.3.
- `pixi search epubcheck` and `pixi search '*epubcheck*'` both return "No packages found".

Code read: `epub.{hpp,cpp}`, `fs/output_dir.{hpp,cpp}`, `fs/output_dir_testing.hpp`, `report.{hpp,cpp}`, `model.hpp`, `zip.hpp`, `json_util.hpp`, `image.hpp`, `mangadex_api.hpp`, `planner.{hpp,cpp}` (`preference_rank`), `download.{hpp,cpp}`, `download/{internal.hpp,plan_phase.cpp,execute_phase.cpp}`, `cmd_download_output.{hpp,cpp}`, `tests/support/{mdx_fixture,zip_reader}.hpp`, `tests/e2e/{CliE2E,run_case}.cmake`, `cases/{download,_edge}.cmake`, the goldens, and `tests/e2e/inputs/README`.

---

## Approach

The logic lands as four pure or near-pure core modules behind interfaces frozen first in T1:
- the manifest model and its JSON;
- a bounded central-directory (CD) reader over an abstract `ReadSource` (pread on an fd, or a fake in tests);
- inspection of an existing file plus held-page reads;
- the matching and final-list decision.

The download orchestration then integrates them late, in `execute_phase.cpp` plus one new private file. Everything is unit-tested with fake sources and hostile tables before any CLI behaviour changes.

`report_to_json`, the console note and the help text change in one late task, so the two goldens are blessed in a single round. The trade-off is six batches and modest parallelism (at most four tracks), in exchange for no cross-track symbol or golden dependency inside a batch. Reused pages are read twice (a validation pass, then the copy) to keep memory at one image.

---

## Dependencies & pixi/CMake changes

**None.**
- No `pixi add`, so `pixi.toml` and `pixi.lock` are untouched.
- No change to `CMakeLists.txt`, `CMakePresets.json`, `tests/CMakeLists.txt`, `cmake/*`, `scripts/*` or CI.
- New sources go under `src/core/**` (GLOB_RECURSE with CONFIGURE_DEPENDS, confirmed at `CMakeLists.txt:89`). New tests go under `tests/unit/**` (`tests/unit/CMakeLists.txt:7`, recursive) and `tests/cli/*.cpp`. New test-support headers are header-only.
- epubcheck is not added. It is not on conda-forge (V9), and it would be a Java runtime dependency. It stays human-only (V1).
- The E2E driver (`CliE2E.cmake`, `run_case.cmake`) gains two keywords, `PRE_RUN` and `EXPECT_UNCHANGED`. That is a serialization point, owned by T3.
- No new pixi tasks, presets or CMake options.

---

## Assumptions to verify

The spec's V2 was dropped in rev 2. V1 and V3–V8 are carried over with their original numbers.

**Cluster A: EPUB / epubcheck / conda-forge (docs + `pixi search`)**
- **V1** `[VERIFY][human-only here]` epubcheck (current 5.x) reports no error and no warning for an extra STORED `META-INF/mangadex-dl.json` that is not listed in the OPF. Otherwise switch to an OPF `<meta>`. Not checkable locally if V9 holds, so it goes to the User Test Plan (`[human: epubcheck not installable via pixi · retryable]`).
- **V9** `[VERIFY]` epubcheck is not on conda-forge. The planner saw "No packages found" for `epubcheck` and `*epubcheck*`. Confirm, and say whether any conda-forge package bundles it, without proposing an add.
- **V10** `[VERIFY]` EPUB 3.3 OCF permits extra files in `META-INF/` beyond the reserved names. Such files "MUST NOT" be listed in the package manifest (confirm the exact wording), and reading systems ignore unknown META-INF files.

**Cluster B: Linux / glibc syscalls (probe with `cxx-probe` on the build dir's filesystem; state the fs type)**
- **V3** `[VERIFY]` `openat(dirfd, fifo, O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC)` returns at once when the FIFO has no writer, and `fstat` reports `S_ISFIFO`.
- **V5** `[VERIFY]` `renameat` over a file that a reader holds open is safe on Linux. The reader keeps the old inode.
- **V6** `[VERIFY]` After the name is renamed over, `pread` on our fd still reads the inspected inode, on both publish paths: O_TMPFILE, linked to `.part-` and then renamed, and the named fallback. Note any overlayfs or tmpfs caveat.
- **V11** `[VERIFY]` `openat(…O_NOFOLLOW)` fails with `ELOOP` on a symlink, dangling ones included, and with `ENXIO` on a UNIX socket. `fstatat(AT_SYMLINK_NOFOLLOW)` classifies a symlink, FIFO, socket, directory or char device without opening it.
- **V12** `[VERIFY]` `st_mtim.tv_nsec` is available under the toolchain's feature macros. An in-place `pwrite` to the inode changes `st_mtim` at nanosecond granularity on ext4 and tmpfs, so `(st_dev, st_ino, st_size, st_mtim)` detects a same-size rewrite.
- **V13** `[VERIFY]` On a regular file, `pread` returns 0 at or past EOF and short counts only at EOF. `O_NONBLOCK` has no effect on regular-file `pread`, and an EINTR retry loop is sufficient.

**Cluster C: nlohmann_json 3.12.0**
- **V4** `[VERIFY]` nlohmann parses and destroys iteratively (download-factcheck V-E3 says yes, but dump, copy and `==` recurse). `depth_ok` plus a non-throwing parse with a 16 MiB cap therefore covers the manifest, provided the parsed manifest document is never dumped, copied or compared.
- **V14** `[VERIFY]` `nlohmann::ordered_json::dump(-1, ' ', false, error_handler_t::replace)` emits compact output (no spaces, no newlines) in insertion order, byte-identical across runs.
- **V15** `[VERIFY]` A JSON `1` parses as `number_unsigned` and is read by `FieldReader::unsigned_integer`. `1.0` (float), `-1` (integer), `"1"` and `true` are rejected as the wrong type.
- **V16** `[VERIFY][report-only]` Peak RSS when parsing a 16 MiB adversarial manifest under the cap (for example a flat array of small numbers at depth 3), measured with a probe. It decides whether T4 needs an element-count pre-scan on top of the byte cap.

**Cluster D: image sniffing**
- **V7** `[VERIFY]` `sniff_image` returns the same `ImageInfo` on the copy path as on the download path for the fixture formats: real PNG and GIF, header-only JPEG (SOF0) and WebP (VP8X). The bytes are identical, so this should hold by construction. Confirm with a unit probe over `mdx::image_bytes`.

**Cluster E: CMake 4.4.4 (floor 3.31), E2E driver, test support**
- **V8** `[VERIFY]` Can `tests/support/zip_reader.hpp` read and modify entries in tests? The planner's reading: it reads, CRC-verifies and exposes `data_offset` per entry, but has no writer. After a byte patch, `read_archive` fails on the CRC, so tests must locate offsets before patching. Rewriting (re-zipping) goes through core `ZipWriter` in the new `tests/support/zip_forge.hpp`.
- **V17** `[VERIFY]` `file(SHA256 <file> <var>)` works in `-P` script mode at the 3.31 floor (for EXPECT_UNCHANGED). Two sequential `execute_process` calls in `run_case.cmake` both see the hermetic env set with `set(ENV{})`.
- **V18** `[VERIFY]` In `execute_process(COMMAND tool COMMAND head -n 1)`, a tool that writes fewer than 4 KiB of fully buffered stdout in one write at exit ends with tool exit 0 and head exit 0, deterministically. This justifies `EXPECT_PIPE_RESULT 0` for the two-update pipe edge case; see Rollout decision D7.

**Cluster F: compiler warnings and clang-tidy (both envs, project flags)**
- **V19** `[VERIFY]` On x86-64 glibc, `dev_t`/`ino_t` are `unsigned long` (the same type as `std::uint64_t`) and `off_t`/`time_t` are `long`. So with GCC 16, `static_cast<std::uint64_t>(st.st_dev)` trips `-Wuseless-cast`, which is an error under `-Werror`. That is why `FileIdentity` stores the native types.
- **V20** `[VERIFY]` `misc-include-cleaner` wants `<sys/types.h>` (and `<ctime>` or `<time.h>` for `time_t`) in `output_dir.hpp` for those types.
- **V21** `[VERIFY]` clang-tidy 23 raises no `cppcoreguidelines-pro-type-vararg` on `::pread`. The bounds-checked `std::span::subspan` pattern raises no `cppcoreguidelines-pro-bounds-*` finding.

**Clusters:** Cluster A: V1, V9, V10 · Cluster B: V3, V5, V6, V11, V12, V13 · Cluster C: V4, V14, V15, V16 · Cluster D: V7 · Cluster E: V8, V17, V18 · Cluster F: V19, V20, V21.

The riskiest are cluster B (fd and rename semantics; probes V5, V6 and V11 should run before approval) and cluster F/V19, because a wrong guess breaks the `-e gcc` build at T1.

---

## Task breakdown

**Conventions for every task:**
- Namespace `mangadex_dl`. Aggregates take DMIs and designated initializers. Classes with an invariant get `[[nodiscard]]` factories.
- `#pragma once`. No nlohmann in public headers.
- Every "Done when" includes `nice pixi run check` green (long timeout) and **both** track reviewers APPROVED.
- Within a batch, a track's code and tests may only call symbols defined by that track itself or by an already-integrated batch. All unit tests link into one executable.
- The T1 headers are **frozen** from batch 2 on. A needed signature change is a coordination event (the orchestrator runs a mini-amendment between batches).

### T1: Frozen interfaces
- **Goal:** every cross-module declaration lands once, so batches 2–4 compile against stable signatures. Existing behaviour stays unchanged.
- **Files / Owns (exclusive):**
  - new public headers: `include/mangadex_dl/book_manifest.hpp`, `include/mangadex_dl/zip_index.hpp`, `include/mangadex_dl/existing_epub.hpp`, `include/mangadex_dl/update_plan.hpp`, `include/mangadex_dl/fs/read_source.hpp`
  - edited public headers: `include/mangadex_dl/epub.hpp`, `include/mangadex_dl/report.hpp`, `include/mangadex_dl/planner.hpp`, `include/mangadex_dl/fs/output_dir.hpp`
  - `src/core/planner.cpp` (extraction only)
  - `tests/unit/interfaces/headers_test.cpp`
- **Build files for build-fixer:** none.
- **What changes:** signatures are binding, and doc comments state ownership and lifetime.
  - **`book_manifest.hpp`:**
    - Constants: `manifest_entry_name = "META-INF/mangadex-dl.json"`, `manifest_generator = "mangadex-dl"`, `manifest_format = 1` (`std::uint64_t`), `max_manifest_bytes = 16 MiB`, `max_manifest_string_bytes = 4096`, `max_manifest_depth = 8`.
    - `struct ManifestCover{std::string file_name{}; std::string entry{};}`
    - `struct ManifestChapter{std::string id{}; std::optional<std::string> volume{}, chapter{}; std::string title{}; std::vector<ScanGroup> groups{}; std::string publish_at{}, updated_at{}; ImageQuality quality = ImageQuality::data; std::vector<std::string> images{};}`
    - `struct BookManifest{std::string identifier{}; std::uint32_t opf_crc32 = 0; std::optional<ManifestCover> cover{}; std::vector<ManifestChapter> chapters{};}`. All three structs get a defaulted `==`.
    - `[[nodiscard]] std::string manifest_to_json(const BookManifest&);`
    - `enum class ManifestFault{malformed, newer_format};`
    - `[[nodiscard]] std::expected<BookManifest, ManifestFault> parse_manifest(std::string_view json);`
    - `[[nodiscard]] bool is_held_image_entry_name(std::string_view) noexcept;` matches `^OEBPS/images/(p[0-9]{5}|cover)\.(jpg|png|gif|webp)$`.
    - `[[nodiscard]] ManifestChapter manifest_chapter_from(const Chapter&, ImageQuality);` (images left empty)
    - `[[nodiscard]] Chapter chapter_from(const ManifestChapter&);` (`pages` = image count; language empty)
  - **`fs/read_source.hpp`:**
    - `enum class ReadFault{io, short_read};`
    - `struct ReadError{ReadFault fault = ReadFault::io; int error = 0;};`
    - Abstract `class ReadSource`: protected constructor; copy and move deleted; virtual destructor; `[[nodiscard]] virtual std::uint64_t size() const noexcept = 0;`; `[[nodiscard]] virtual std::expected<void, ReadError> read_exact(std::uint64_t offset, std::span<std::byte> out) = 0;`.
    - `class FdReadSource final : public ReadSource{ FdReadSource(const UniqueFd& fd MANGADEX_DL_LIFETIMEBOUND, std::uint64_t size) noexcept; … }`. It borrows the fd, and the size is fixed at inspection.
  - **`zip_index.hpp`:**
    - `max_central_directory_bytes = 8 MiB`.
    - `struct ZipEntryRecord{std::string name{}; std::uint16_t flags = 0, method = 0; std::uint32_t crc32 = 0, compressed_size = 0, uncompressed_size = 0, local_header_offset = 0;}`
    - `enum class ZipFault{read_error, short_read, size_out_of_range, no_end_record, unsupported, bad_directory, duplicate_name, bad_local_header, data_out_of_bounds};` (`unsupported` covers a comment, multiple disks and zip64)
    - `struct ZipError{ZipFault fault = ZipFault::bad_directory; int error = 0;}`
    - `class ZipIndex{ static std::expected<ZipIndex, ZipError> read(ReadSource&); const ZipEntryRecord* find(std::string_view) const noexcept LIFETIMEBOUND; const std::vector<ZipEntryRecord>& entries() const noexcept LIFETIMEBOUND; std::uint64_t central_directory_offset() const noexcept; }` Move-only value. Entries are kept in CD order; the index is immutable after `read`.
    - `[[nodiscard]] std::expected<std::uint64_t, ZipError> locate_entry_data(ReadSource&, const ZipIndex&, const ZipEntryRecord&);` returns the first data byte. It guarantees the local header agrees with the CD and that `[data, data + csize)` lies within `[0, cd_offset)`.
  - **`existing_epub.hpp`:**
    - `enum class ForeignReason{not_regular, unreadable, not_ours, other_book, changed_by_other_program, newer_format, changed_during_update};`
    - `[[nodiscard]] std::string_view foreign_reason_text(ForeignReason) noexcept;` and `foreign_remedy(ForeignReason) noexcept` (static strings)
    - `struct InspectFailure{ForeignReason reason = ForeignReason::not_ours; int error = 0;}`
    - `struct HeldEntry{std::string name{}; MediaType type = MediaType::jpeg; std::uint32_t crc32 = 0; std::uint32_t size = 0; std::uint64_t data_offset = 0;}`
    - `struct HeldBook{BookManifest manifest{}; std::vector<std::vector<HeldEntry>> pages{}; std::optional<HeldEntry> cover{};}` (`pages` is parallel to `manifest.chapters`)
    - `[[nodiscard]] std::expected<HeldBook, InspectFailure> inspect_existing(ReadSource&, std::string_view expected_identifier);`
    - `enum class PageFault{content, read};`
    - `struct HeldPage{std::vector<std::byte> bytes{}; ImageInfo info{};}`
    - `[[nodiscard]] std::expected<HeldPage, PageFault> read_held_page(ReadSource&, const HeldEntry&);`
  - **`update_plan.hpp`:**
    - `enum class UpdateState{complete, incomplete, diverged};`
    - `enum class ChapterSource{reuse, download};`
    - `struct FinalChapter{Chapter chapter{}; ChapterSource source = ChapterSource::download; ImageQuality quality = ImageQuality::data; std::optional<std::size_t> held{}; std::size_t planned = 0;}` These are indices, never iterators.
    - `struct KeptRelease{std::string held_id{}, planned_id{}, key_label{};}`
    - `struct Repick{std::string held_id{}, planned_id{}, held_group{}, planned_group{}, key_label{};}`
    - `struct UpdateDecision{UpdateState state = UpdateState::complete; std::vector<FinalChapter> chapters{}; std::vector<std::size_t> missing{}, extra{}; std::vector<KeptRelease> kept{}; std::vector<Repick> repicked{};}`
    - `[[nodiscard]] UpdateDecision decide_update(const PlannedEpub&, const BookManifest&, std::span<const std::string> preferred_groups, ImageQuality quality);`
    - `[[nodiscard]] UpdateDecision fall_back_to_download(UpdateDecision, std::size_t final_index, const PlannedEpub&, ImageQuality);`
    - `[[nodiscard]] PlannedEpub final_epub(const PlannedEpub&, const UpdateDecision&);` Chapters are the final list; `planned_pages` = held images + planned pages.
    - `[[nodiscard]] std::vector<std::string> new_chapter_ids(const UpdateDecision&);` and `reused_chapter_ids(const UpdateDecision&);`
    - `[[nodiscard]] std::vector<SkippedChapter> reconcile_skipped(std::vector<SkippedChapter>, std::span<const KeptRelease>, const PlannedEpub&);`
    - `[[nodiscard]] std::string chapter_labels(std::span<const Chapter>, std::size_t max_shown = 5);` gives `Ch. 4, Ch. 5 and K more`.
  - **`epub.hpp` additions:**
    - `inline constexpr std::string_view opf_entry_name = "OEBPS/content.opf";`
    - `[[nodiscard]] std::string page_image_entry(std::size_t index, MediaType);` and `cover_image_entry(MediaType);`
    - `struct EpubBookChapter{ManifestChapter record{}; std::size_t first_page = 0; std::size_t page_count = 0;}`
    - `[[nodiscard]] Result<void> EpubWriter::finish(std::optional<std::string> cover_file_name, std::vector<EpubBookChapter> chapters);` It does what `finish()` does, then writes the manifest last. The existing `finish()` stays: no manifest, used by tests.
  - **`report.hpp`:**
    - `enum class ExistingKind{none, complete, incomplete, diverged, foreign, unchecked};`
    - `EpubOutcome` gains, after `error` (designated-init order is safe), `ExistingKind existing = ExistingKind::none; std::vector<std::string> new_chapters{}; std::vector<std::string> reused_chapters{};`.
    - Doc comments: `exists` now means "left alone"; `replaced` also covers updates.
  - **`fs/output_dir.hpp` additions:**
    - `struct FileIdentity{dev_t device{}; ino_t inode{}; off_t size{}; time_t mtime_sec{}; long mtime_nsec{};}` with `==` (native types, V19).
    - `enum class ExistingOpen{absent, opened, not_regular, unreadable};`
    - `struct ExistingHandle{ExistingOpen status = ExistingOpen::absent; UniqueFd fd{}; FileIdentity identity{}; int error = 0;}`
    - `[[nodiscard]] Result<ExistingHandle> OutputDir::open_existing(std::string_view name) const;`
    - `enum class ReplaceOutcome{replaced, written, changed};`
    - `[[nodiscard]] Result<ReplaceOutcome> OutputDir::publish_replacing(TempFile&&, std::string_view final_name, const FileIdentity& expected);`
  - **`planner.hpp` / `planner.cpp`:** `[[nodiscard]] std::size_t group_preference_rank(std::span<const ScanGroup> groups, std::span<const std::string> preferred);` is the anonymous `preference_rank` made public: ASCII case-insensitive name or id, and `preferred.size()` when nothing matches. `pick_release` calls it, with behaviour unchanged.
- **Dependencies:** none.
- **Done when:** `nice pixi run check` **and** `nice pixi run -e gcc check` are green (V19 and V20 bite here), and `headers_test.cpp` includes every new or edited header in isolation.
- **Tests (unit):**
  - `headers_test.cpp` includes the headers.
  - `static_assert`s on `manifest_entry_name` and `max_manifest_bytes == max_api_json_bytes`.
  - `EpubOutcome{}` defaults (`existing == none`, empty lists).
  - The existing planner dedupe tests prove the extraction preserved behaviour.

### T2: Fixture variants and the in-process CLI harness
- **Goal:** the seven hermetic variants of `clean` (including a B-served regrouped tree for decision 5), and one shared harness for the `[cli]` download tests.
- **Owns:** `tests/support/mdx_fixture.hpp`, `tests/support/download_cli_harness.hpp` (new), `tests/cli/download_cli_test.cpp` (behaviour-preserving refactor only), `tests/e2e/fixtures/fixture_gen.cpp` (header comment only), `tests/e2e/inputs/README`, `tests/unit/support/mdx_fixture_test.cpp`.
- **Build files for build-fixer:** none.
- **What changes:**
  - **Fields:** `FixtureChapter` and `FixtureCover` each gain `bool served = true;`, appended last with a DMI. `write_chapter_files` skips the at-home document **and** the images of an unserved chapter, so fetching it is a 404 and the file fails with exit 6. Unserved cover images are not written.
  - **Groups:** A is the existing "Clean Scans" (`11111111-0002-4000-8000-000000000001`). B is `fixture_uuid('1', group, 2)`, named **"Bravo Scans"**. The name "Charlie Scans" is used only as a `--group` value that matches nothing.
  - **New chapters:**
    - ch 4: `numbered('1', 4)`, vol 2, A, PNG + GIF.
    - B's ch 1–3: `numbered('1', 101..103)`, same labels as A's.
    - B's ch 2.5: `numbered('1', 104)`, vol 1.
    - Every shared chapter keeps byte-identical metadata and images with `clean`, which byte identity relies on.
  - **Variants**, all with manga `11111111-…`, all built from `clean_fixture()`, all added to `all_fixtures()`:

    | Tree | Feed | Served |
    |---|---|---|
    | `clean-grown` | clean + ch 4 | all |
    | `clean-grown-reuse-only` | as clean-grown | **only ch 4**; covers not served |
    | `clean-regrouped` | B ch 1–3 (A gone) | none; covers not served |
    | `clean-regrouped-served` | B ch 1–3 | **all** (decision 5) |
    | `clean-regrouped-grown` | B ch 1–3 + B ch 2.5 | **only ch 2.5**; covers not served |
    | `clean-mixed-reference` | A ch 1–3 + B ch 2.5 | all |
    | `clean-diverged` | vol 1 as clean; vol 2 = ch 4 only (ch 3 gone) | all |

  - **Harness:** `download_cli_harness.hpp` takes the `Tree` / `run_args` / `snapshot` / `path_lines` / `concat` / `parse_document` helpers out of `download_cli_test.cpp`, with no new behaviour. `download_cli_test.cpp` then includes it.
  - **README:** the trees table and the `served` rule.
- **Dependencies:** none.
- **Done when:** `nice pixi run check` is green; `e2e.fixtures` writes 12 trees; `download_cli_test.cpp` has the same test count and passes unchanged.
- **Tests (unit, `mdx_fixture_test.cpp`):**
  - Each variant's feed lists exactly the documented chapter ids.
  - Unserved chapters have no `api/at-home/server/<id>.json` and no images; unserved covers are absent.
  - Shared chapters are equal field by field across variants (`==` on the produced JSON and image bytes).
  - `all_fixtures()` names are unique and safe path components.

### T3: E2E driver keywords `PRE_RUN` and `EXPECT_UNCHANGED`
- **Goal:** installed-binary cases can prepare a DIR with one earlier run, and can prove that a file was left byte-unchanged.
- **Owns:** `tests/e2e/CliE2E.cmake`, `tests/e2e/run_case.cmake`, `tests/e2e/cases/download.cmake` (one self-test case and the header comment).
- **Build files for build-fixer:** `tests/e2e/CliE2E.cmake`, `tests/e2e/run_case.cmake`.
- **What changes:**
  - **`PRE_RUN <arg>...`** (multi-value):
    - Runs `<tool> <args>` once in `{{WORK}}`, after the hermetic env and ENV overrides and before the main command, with stdin `/dev/null`. Placeholders are expanded as in ARGS.
    - It must exit 0, and its stderr must not match the sanitizer-marker regex. Otherwise the case fails with "pre-run failed (exit N …)", and the `_explain_exit` text is reused.
    - Its output is kept in variables, never written as files into `{{WORK}}`.
    - It combines with every main mode (plain, TTY, PIPE_HEAD, STDOUT_FILE).
    - With PRE_RUN, the CTest TIMEOUT becomes `2*TIMEOUT + 30`.
  - **`EXPECT_UNCHANGED <rel>...`:**
    - It requires PRE_RUN, or registration fails with FATAL_ERROR. It also goes through the existing relative-path validation.
    - The driver takes `file(SHA256)` of each file after the pre-run, where a missing file fails the case, and again after the main run. They must match.
  - Header comment updated. The spec file carries `CASE_PRE_RUN` and `CASE_EXPECT_UNCHANGED`.
  - **Self-test case `rerun-unchanged`** in `download.cmake`: PRE_RUN `download --api-base file://{{FIXTURES}}/clean <id> out`, then the same command. Asserts EXPECT_EXIT 0, EXPECT_STDOUT_EMPTY, EXPECT_UNCHANGED for both volumes, and EXPECT_NO_GLOB `out/.*.part-*`. Stderr is deliberately not asserted yet; T14 tightens it.
- **Dependencies:** none.
- **Done when:** `nice pixi run check` is green; `nice pixi run test -R '^e2e\.'` is green; the hand-off shows a local (uncommitted) negative check, where a PRE_RUN with a bad id reports "pre-run failed".
- **Tests:** the self-test case. The existing E2E and edge suites stay green.

### T4: Manifest model and JSON
- **Goal:** a deterministic manifest writer, and a hostile-input-safe parser.
- **Owns:** `src/core/book_manifest.cpp` (new), `src/core/json_util.hpp`, `src/core/json_util.cpp`, `tests/unit/book_manifest/book_manifest_test.cpp` (new), `tests/unit/book_manifest/book_manifest_hostile_test.cpp` (new).
- **Build files for build-fixer:** none.
- **What changes:**
  - **`json_util`:**
    - New overload `parse_untrusted(std::string_view, std::string_view what, const ParseLimits&)`, where `ParseLimits{std::size_t max_bytes; std::size_t max_depth; Errc code;}`.
    - `FieldReader` gets an optional `Errc` (default `network`).
    - Existing signatures and behaviour are unchanged, so `mangadex_api.cpp` is untouched.
  - **`manifest_to_json`:** `ordered_json`, keys in the order `format, generator, identifier, opf_crc32` (8 lowercase hex), `cover` (null or `{file_name, entry}`), `chapters[{id, volume, chapter, title, groups[{id,name}], publish_at, updated_at, quality ("data"|"data-saver"), images[]}]`. Written with `dump(-1, ' ', false, replace)` (V14). No clock, no version.
  - **`parse_manifest`:**
    - Cap `max_manifest_bytes`, depth `max_manifest_depth`, non-throwing parse, `FieldReader`.
    - `format` must be an unsigned integer: `> 1` gives `newer_format`, `< 1` gives `malformed`. `generator` must be exactly `"mangadex-dl"`.
    - Every string ≤ 4 KiB. Chapter and group ids satisfy `is_uuid`. `opf_crc32` is exactly 8 lowercase hex digits. `quality` is from the enum.
    - The `chapters` array is non-empty, and each chapter has 1..`max_pages_per_chapter` images. Total images ≤ `zip_max_entries`.
    - Image and cover entry names satisfy `is_held_image_entry_name` (also exposed for T9). Unknown keys are ignored.
    - Every failure maps to `malformed`, never a throw. The parsed document is never dumped, copied or compared (V4).
  - `manifest_chapter_from`, `chapter_from` and `is_held_image_entry_name` are implemented here.
- **Dependencies:** T1.
- **Done when:** `nice pixi run check` is green.
- **Tests (unit):**
  - Round trip `parse(to_json(m)) == m` for 0 and 1 covers, null labels, unicode titles, both qualities.
  - Exact compact bytes for one small manifest. Two calls give the same bytes.
  - The **hostile table**:
    - each required key missing or of the wrong type;
    - `format` 0, 2, `1.0`, `"1"`, `true`;
    - generator other;
    - a 4097-byte string;
    - a non-UUID id;
    - opf_crc32 upper-case or 7 hex digits;
    - 5001 images in one chapter;
    - 65535 total images (with cheap names);
    - an empty chapters array or empty images;
    - bad names (`OEBPS/images/../p00001.png`, `OEBPS/images/p1.png`, `OEBPS/images/p00001.svg`, `mimetype`);
    - depth 9, 1000 and 100000 (the pre-scan rejects them without recursion);
    - 16 MiB + 1 byte;
    - invalid UTF-8;
    - trailing garbage, a BOM, an empty document.
    - Every row gives the expected fault, never a crash, ASan-clean.
  - The `json_util` overload: cap, depth, and the error class propagated.

### T5: Read source and bounded zip index (CD reader)
- **Goal:** a generic, overflow-safe STORED-zip CD reader over `ReadSource`, plus test fakes and forging tools.
- **Owns:** `src/core/fs/read_source.cpp` (new), `src/core/zip_index.cpp` (new), `tests/support/fake_read_source.hpp` (new), `tests/support/zip_forge.hpp` (new), `tests/unit/fs/read_source_test.cpp` (new), `tests/unit/zip_index/zip_index_test.cpp` (new), `tests/unit/zip_index/zip_index_hostile_test.cpp` (new).
- **Build files for build-fixer:** none.
- **What changes:**
  - **`FdReadSource::read_exact`:**
    - Rejects `offset > size || len > size - offset` as `short_read` with no syscall.
    - A `pread` loop retries EINTR; a 0 or short return before completion is `short_read`; `-1` is `io` with errno.
    - No `string_view::data()` reaches a C API.
  - **`ZipIndex::read`:**
    - Requires `22 ≤ size ≤ zip_max_u32`, then reads the EOCD at exactly `size-22`. Checks: the signature; comment length 0; disk fields 0; both entry counts equal and ≤ `zip_max_entries`; no 0xFFFF or 0xFFFFFFFF sentinels; `cd_size ≤ 8 MiB`; `cd_offset + cd_size ≤ size-22`.
    - All sums are of u16/u32 fields widened to `std::uint64_t`, at most five terms, so they cannot overflow. The spec's "overflow builtins" become unnecessary (Rollout D4).
    - Reads the CD into one exact-size buffer and walks it with slice-before-read bounds checks. Each entry's name is copied into an owned `std::string`, and a duplicate name gives `duplicate_name`. The CD buffer is freed before returning, and a sorted name index is built.
  - **`locate_entry_data`:**
    - Reads 30 bytes plus the name length at `lho`. The signature, flags, method, CRC, sizes, name length and name bytes must equal the CD.
    - `data = lho + 30 + name_len + extra_len`, and `data + csize ≤ cd_offset`.
  - **Test support:**
    - `fake_read_source.hpp`: a byte-vector source with fault injection (fail read #k with EIO, short-read at read #k, mutate bytes after read #k, report a size different from the data). It counts reads.
    - `zip_forge.hpp`: build archives from `(name, bytes)` lists through core `ZipWriter` into a `MemorySink`; raw mutators (`set_u16/u32_at`, `append_comment`, `truncate`, `patch_byte`); file helpers `patch_file_byte(path, offset)` and `rewrite_entries(path, transform)` (re-zip with `zip_reader` plus core `ZipWriter`; V8).
- **Dependencies:** T1.
- **Done when:** `nice pixi run check` and `nice pixi run -e gcc check` are green (size math is `-Wconversion`/`-Wsign-conversion`-sensitive in both compilers).
- **Tests (unit):**
  - Happy path on `ZipWriter` output (1 entry, 100 entries, names with spaces); `find` and `entries` order; `locate_entry_data` equals `zip_reader`'s `data_offset`.
  - `FdReadSource` on a temp file: exact, EOF short read, and an offset+length that would overflow u64 (rejected without a syscall).
  - **Hostile CD table:** each row must give a `ZipError`, never a crash, ASan/UBSan-clean.
    - truncation at every length 0..N of a small archive;
    - a 1-byte comment;
    - EOCD not at `size-22`;
    - disk ≠ 0;
    - entry counts unequal;
    - count = 0xFFFF;
    - cd_size or cd_offset = 0xFFFFFFFF;
    - `cd_offset = cd_size = 0xFFFFFFFE` (sum past the end);
    - a claimed cd_size of 9 MiB (rejected before allocation);
    - a central header with a bad signature;
    - a name length overrunning the CD;
    - extra or comment lengths overrunning;
    - a duplicate name;
    - method 8 (the index records it, and T9 rejects it);
    - a local header mismatch in each field;
    - `lho` pointing into the CD or past EOF;
    - a local extra length pushing the data past `cd_offset`;
    - size 21 and size `zip_max_u32 + 1` via the fake size.
  - **Fuzz-style loop:**
    - Fixed-seed `std::mt19937`, 2000 mutations of 1–4 random bytes on a 3-entry archive, plus an exhaustive single-byte flip over the EOCD and CD region.
    - Each result is an index or an error, never a crash.
    - The seed is printed with `INFO`.
  - **Read faults:** EIO and short read injected at each read index (`io` vs `short_read` propagate).

### T6: OutputDir: open an existing file, replace it if unchanged
- **Goal:** fd-relative opening of `DIR/<name>` with an identity record, and an atomic publish that replaces only an unchanged inode.
- **Owns:** `src/core/fs/output_dir.cpp`, `src/core/fs/output_dir_testing.hpp` (only if a new fault seam is needed), `src/core/fs/errno_text.hpp` (new, core-private), `src/core/fs/errno_text.cpp` (new), `tests/unit/fs/output_dir_existing_test.cpp` (new).
- **Build files for build-fixer:** none.
- **What changes:**
  - **`errno_text`:** the existing anonymous `errno_text` and `strerror_r` overloads move to `fs/errno_text.{hpp,cpp}` (`[[nodiscard]] std::string errno_text(int)`), so T10 can reuse them. `output_dir.cpp` calls it.
  - **`open_existing(name)`:**
    1. First `fstatat(AT_SYMLINK_NOFOLLOW)`: ENOENT gives `absent`, and not `S_ISREG` gives `not_regular` **without opening** (so no FIFO or device is ever opened). EACCES/EPERM/EIO give `unreadable` with the errno; any other errno gives `Errc::failure`.
    2. Then `openat(fd_, name, O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC)`. ENOENT means it vanished (`absent`); ELOOP/ENXIO/ENODEV give `not_regular` (V11); EACCES/EPERM/EIO give `unreadable`; any other errno gives `Errc::failure` (EMFILE included, so exit 1).
    3. Then `fstat` on the fd, which must be `S_ISREG` (TOCTOU-safe re-check), and the `FileIdentity` is filled from that fstat.
    - This is a hardening deviation from the spec's "open, then fstat" (Rollout D3).
  - **`publish_replacing(temp, name, expected)`:**
    - `sync_and_close` first.
    - O_TMPFILE: link to a fresh `.part-` name (the existing `with_unique_name` path, with an `UnlinkGuard`). Fallback: the named temporary already exists.
    - Then `fstatat(fd_, name, AT_SYMLINK_NOFOLLOW)`:
      - identity equal: `renameat(temp, name)` gives `replaced`;
      - ENOENT: no-clobber publish (the existing `link_proc` or `renameat2(RENAME_NOREPLACE)` / `linkat` fallback) gives `written`, and EEXIST there gives `changed`;
      - differs: the temporary is discarded (guard unlinks the `.part-` name, State destructor) and the result is `changed`.
    - A best-effort dir fsync after `replaced` or `written`.
    - The check-to-rename window is documented in a comment (cpp-security §4).
- **Dependencies:** T1.
- **Done when:** `nice pixi run check` and `nice pixi run -e gcc check` are green (syscall-sensitive).
- **Tests (unit):**
  - **`open_existing`:**
    - a regular file (identity fields equal the test's own `stat`);
    - absent;
    - a symlink to a file, a dangling symlink, a FIFO (returns at once, no hang; V3), a directory and an AF_UNIX socket, each `not_regular`;
    - a mode-000 file as `unreadable` (`SKIP` with the reason when euid is 0);
    - an invalid component (`..`, `a/b`) as `Errc::failure`.
  - **`publish_replacing`**, each on **both** the O_TMPFILE path and the forced fallback (`force_fallback_tempfiles`), with no `.part-` file left in any case:
    - replaced (content swapped, old fd still reads old bytes: V5/V6);
    - vanished (target unlinked before publish: `written`);
    - changed (target replaced by another file or rewritten in place: `changed`, original untouched);
    - `force_renameat2_unsupported` on the vanished path.

### T7: Update decision (pure matching, re-pick, final list, reconciliation)
- **Goal:** the table-tested heart of the feature: classification, the final chapter list and the report lists, with no I/O.
- **Owns:** `src/core/update_plan.cpp` (new), `tests/unit/update_plan/update_plan_test.cpp` (new), `tests/unit/update_plan/reconcile_test.cpp` (new).
- **Build files for build-fixer:** none.
- **What changes:**
  - **Matching** (planned chapter `p` against held chapters; each held chapter is consumed at most once; held order is the tie-break; the first rule wins):
    1. the same id, ASCII case-insensitive;
    2. the same key `(normalize_label(volume) or none, normalize_label(chapter))`, where an empty label counts as none, and a null or empty chapter label is keyed by id only (as dedupe does);
    3. a planned `(v≠none, c)` against a leftover held `(none, c)`.
  - **Re-pick (decision 5):** only for rules 2 and 3, and only when `preferred_groups` is non-empty. It applies when `group_preference_rank(P.groups) < group_preference_rank(H.groups)`, strictly.
    - P is downloaded and recorded in `repicked`; P counts as missing.
    - **H is consumed by the match, so it is neither extra nor reused.** This is what makes `clean-regrouped --group B` incomplete rather than diverged.
    - Equal ranks (both unmatched included) keep H.
  - **State:** no missing gives `complete`; missing and no extra gives `incomplete`; missing and extra gives `diverged`.
  - **Final list** (plan reading order):
    - matched by id: the planned `Chapter`, `reuse`;
    - matched by number and kept: `chapter_from(H)` with the plan's volume and chapter labels, `reuse`, quality = H's, plus a `KeptRelease`;
    - missing or re-picked: the planned `Chapter`, `download`, quality = the run's.
  - **`fall_back_to_download`:** returns a new decision with that entry switched to the planned release for its key (download, run quality). Any `KeptRelease` for it is removed.
  - **`final_epub`:** copies the `PlannedEpub` with `chapters` = the final list. `planned_pages` = held image counts + planned pages.
  - **`reconcile_skipped`:** for each `KeptRelease`, erase H's `duplicate` entry if present (by id, case-insensitive), then append P as `{reason: duplicate, kept: H}` with P's labels. Appends follow final-list order, which is deterministic.
  - **`new_chapter_ids` / `reused_chapter_ids`** follow the final list. **`chapter_labels`:** "Ch. 4" / "Vol. 1 Ch. 1" / "Oneshot"; at most 5, then "and K more".
- **Dependencies:** T1.
- **Done when:** `nice pixi run check` is green.
- **Tests (unit, criteria 1, 2 and decision 5's `[unit]` row):**
  - **Matching table:**
    - same ids;
    - same numbers with different ids: complete, H kept with plan labels and manifest metadata;
    - an added chapter: incomplete, with exact `missing` and new/reused lists;
    - extras only: complete;
    - extras + missing: diverged;
    - a loose chapter moved into a volume (rule 3);
    - id-keyed null and empty labels;
    - a relabel with the same id (plan labels win);
    - normalisation `"07"` == `"7.0"`;
    - a held duplicate key (second held is extra);
    - case-insensitive ids.
  - **Rank:**
    - better replaces (missing, `repicked`, H not extra);
    - equal keeps;
    - worse keeps;
    - both unmatched keep;
    - no `--group` keeps;
    - matching by id is never re-picked;
    - group matched by name case-insensitively and by UUID.
  - **Final list:** contributors and `modified` computed from `final_epub` (through `chapter_from`); mixed qualities preserved per chapter; `fall_back_to_download` for a kept-by-number H and for a same-id H.
  - **Reconcile:** H present or absent in skipped; two substitutions; order; a fallback removes the substitution.
  - **Iterator safety:** inputs are shuffled and the output is checked to hold only indices and values, ASan-clean.

### T8: EpubWriter writes the manifest
- **Goal:** every new EPUB carries `META-INF/mangadex-dl.json` as its last STORED entry, consistent with the archive.
- **Owns:** `src/core/epub.cpp`, `tests/unit/epub/epub_manifest_test.cpp` (new).
- **Build files for build-fixer:** none.
- **What changes:**
  - `page_image_entry` and `cover_image_entry` become the single naming source (used internally too).
  - `finish(cover_file_name, chapters)`:
    - validates that the `[first_page, first_page + page_count)` ranges tile all pages exactly, in order (else `Errc::failure`);
    - runs the existing finish body, keeping the CRC of the exact `content.opf` bytes (`crc32` from `zip.hpp`);
    - fills each `record.images` from the page media types, and `cover` = `{file_name, cover entry}` only when a cover was added **and** a name was given;
    - writes `manifest_to_json` with `add_text(manifest_entry_name, …)` after `toc.ncx`.
  - The plain `finish()` is unchanged.
- **Dependencies:** T1, T4.
- **Done when:** `nice pixi run check` is green.
- **Tests (unit, criterion 3):**
  - Build into a `MemorySink`, then read it back with `zip_reader`:
    - the manifest is the last entry, method 0, extra 0;
    - `parse_manifest` succeeds;
    - `opf_crc32` equals `reference_crc32(content.opf)` and the CD CRC;
    - every image name and the cover entry exist and match the archive order;
    - identifier equals the OPF's `dc:identifier`.
  - With and without a cover; a cover added but no name gives `cover: null`.
  - Two builds are byte-identical.
  - Tiling errors (gap, overlap, wrong total) are rejected before anything is written.
  - The existing `epub_test.cpp` stays untouched and green.

### T9: Inspecting existing EPUBs, and held-page reads
- **Goal:** classify one existing file as ours (`HeldBook`) or foreign (with its reason), and read one held page with full validation.
- **Owns:** `src/core/existing_epub.cpp` (new), `tests/support/held_book_builder.hpp` (new: builds our-format archives with `zip_forge` + `manifest_to_json`, with knobs for every fault), `tests/unit/existing_epub/inspect_test.cpp` (new), `tests/unit/existing_epub/inspect_hostile_test.cpp` (new), `tests/unit/existing_epub/held_page_test.cpp` (new).
- **Build files for build-fixer:** none.
- **What changes:**
  - **`inspect_existing`**, in spec order:
    1. `ZipIndex::read`. A `read_error` gives `unreadable` with the errno; any other fault, including `short_read`, gives `not_ours`.
    2. Entry 0 must be `mimetype`.
    3. Manifest entry: absent gives `not_ours`. Otherwise it must have method 0, flags 0 and `csize == usize ≤ max_manifest_bytes` (checked **before** allocating). `locate_entry_data` must pass and the CRC must match.
    4. `parse_manifest`: `newer_format` gives `newer_format`; `malformed` gives `not_ours`.
    5. The identifier must equal the expected one (else `other_book`). `content.opf` must be in the CD (else `not_ours`) and its CD CRC must equal `opf_crc32` (else `changed_by_other_program`).
    6. References:
       - each image and the cover entry is in the CD and referenced exactly once across chapters and cover;
       - it has method 0 and flags 0, and `0 < csize == usize ≤ max_image_bytes`;
       - `locate_entry_data` passes, which puts the data in `[0, cd_offset)`;
       - the media type comes from the extension;
       - any failure gives `not_ours`.
    - The result is a `HeldBook`. Nothing read from the file ever appears in a message.
  - **`read_held_page`:** `read_exact(data_offset, size)` into an exact-size owned vector, then the CRC compare, `sniff_image`, a side of 0 or above `max_image_side` (the sniffer enforces it), and the type must equal the entry's extension. CRC, sniff or type failures give `content`; read faults give `read`.
  - **`foreign_reason_text` and `foreign_remedy`:** fixed strings per the spec, with the remedy mapping in Rollout D8.
- **Dependencies:** T1, T4, T5.
- **Done when:** `nice pixi run check` and `nice pixi run -e gcc check` are green.
- **Tests (unit, criteria 4 and 5):**
  - Happy path. `HeldBook` pages are parallel to the chapters, and `data_offset` equals `zip_reader`'s.
  - **Hostile manifest and reference table:** each row must classify as foreign with the exact reason, never crash, ASan/UBSan-clean.
    - no manifest;
    - manifest deflated (method 8), flags ≠ 0, csize ≠ usize;
    - a claimed manifest size of 16 MiB + 1 (no allocation; the fake source counts reads);
    - a manifest CRC mismatch;
    - a manifest whose data overlaps the CD;
    - malformed JSON, a deep nesting bomb, format 2 (`newer_format`);
    - another identifier (`other_book`);
    - an OPF rewritten with a fresh CD CRC but a stale `opf_crc32` (`changed_by_other_program`);
    - `content.opf` missing;
    - an image name outside the pattern, a duplicate reference (two chapters, and chapter plus cover), an entry missing from the CD;
    - an image with method 8, csize ≠ usize, size 0, or 32 MiB + 1 (claimed);
    - image data overlapping the CD or past the end;
    - a local header disagreeing with the CD;
    - a reference to `mimetype` or `OEBPS/content.opf`;
    - EIO at each read index (`unreadable`) and a short read at each index (`not_ours`).
  - **Held page:** CRC mismatch, non-image bytes, a side of 0 or 65536 (crafted PNG IHDR), and an extension/type mismatch each give `content`; EIO and a short read give `read`; V7 sniffs the same `ImageInfo` as the downloaded bytes for PNG, GIF, JPEG-header and WebP-header.
  - A fuzz-style loop: 1000 fixed-seed mutations of a valid book; each gives a `HeldBook` or a foreign result, never a crash.

### T10: Download integration (inspection, pre-pass, rebuild, publish)
- **Goal:** `run_download` skips complete files, rebuilds incomplete ones with page reuse, and leaves diverged and foreign files alone, with the old file intact on every failure and interrupt path.
- **Owns:** `src/core/download/execute_phase.cpp`, `src/core/download/internal.hpp`, `src/core/download/existing.hpp` (new, private), `src/core/download/existing.cpp` (new), `tests/unit/download/download_update_test.cpp` (new), `tests/unit/download/existing_assess_test.cpp` (new), `tests/unit/download/download_test.cpp` (adjust only expectations that legitimately change, such as observer warnings for foreign files).
- **Build files for build-fixer:** none.
- **What changes:**
  - **`existing.{hpp,cpp}`** declares `Assessment{ExistingKind kind; ExistingHandle handle; std::optional<HeldBook> held; std::optional<UpdateDecision> decision; PlannedEpub final; std::optional<InspectFailure> foreign; std::vector<std::vector<ImageInfo>> page_info; std::optional<ImageInfo> cover_info; bool reuse_cover;}`.
    - `assess(RunContext&, const OutputDir&, const PlannedEpub&, bool real_run)` does `open_existing`, then `FdReadSource`, then `inspect_existing`, then `decide_update(epub, held.manifest, options.groups, options.quality)`.
    - `prepass(...)` runs only for a real-run `incomplete`. For each reuse chapter and each page, it calls `read_held_page`, records `ImageInfo` and runs `stop_now` per page.
      - Any page fault switches the chapter to download (`fall_back_to_download`) and warns `<path>: Ch. 3 in the existing file is damaged; downloading it again`.
      - The cover is reused only when the plan's cover `file_name` equals `manifest.cover.file_name` and the entry validates; otherwise it is downloaded.
    - Formats the core-emitted lines:
      - `on_warning`: foreign (`<path>: left alone: <reason>; <remedy>`, where `unreadable` appends `(<errno_text>)`), diverged (N/M counts per spec), damaged;
      - `on_verbose(1)`: kept release, replacing release (`(<H group>) with <P group>'s (--group)`) and `updating <path>: …`.
  - **`produce()`:**
    - `--overwrite`: `exists()` decides `existing` (`unchecked` or none). The publish path is unchanged.
    - Otherwise assess:
      - absent: today's path, `existing` none;
      - `not_regular`, `unreadable` or foreign: `exists` + `foreign` + warning;
      - complete: `exists` + `complete`, `epub` = `final`, `pages` = held images, fd closed;
      - diverged: `exists` + `diverged`, `epub.chapters` = the held chapters (Rollout D2), warning;
      - incomplete: if `plan_error`, `failed` + `incomplete` (no page reads). Otherwise: pre-pass, `on_epub_begin(final)`, `create_temp`, `EpubBuild` over the final list, then `publish_replacing(identity)`. `replaced` gives written with `replaced:true`; `written` gives written with `replaced:false`; `changed` gives `exists` + `foreign` (`changed_during_update`) + warning.
    - `new_chapters` and `reused_chapters` are filled only for `incomplete` (Rollout D1). `report.skipped = reconcile_skipped(…)` after each file.
  - **`EpubBuild`:**
    - Takes the final `PlannedEpub`, the `FinalChapter` list, and an optional `{FdReadSource&, HeldBook&, page_info}`.
    - A reuse chapter calls `add_nav_entry` (`nav_label` / `nav_group` of the final chapter), then per page `read_held_page`. A fault, or an `ImageInfo` different from the pre-pass, is a file failure: "the existing file changed or could not be read during the update" (`Errc::incomplete`, not a run abort). `stop_now` runs per page, and `add_page` and `on_pages` follow.
    - A download chapter goes through `add_chapter` as today, unchanged (FetchPool).
    - The cover is copied or downloaded.
    - Always `writer_.finish(cover_file_name, book_chapters)`. The `EpubWriter` call sequence is identical to a fresh build.
  - **`dry_run()`:** the same assessment without the pre-pass (structure only: CD, manifest and local headers; never page data; Rollout D11). complete gives `exists`; incomplete gives `planned` + `replaced:true` + lists; diverged and foreign give `exists` + warnings. `reconcile_skipped` is applied, and nothing is ever written or created.
  - **Declaration order** (borrow order) inside `produce()`: `Assessment` (owns the fd), then `FdReadSource`, then `TempFile`, then `EpubBuild` (scoped in `build_epub`), then publish. On every early return or unwind, the temporary is discarded and then the fd is closed.
- **Dependencies:** batches 1–3 (T1–T9).
- **Done when:** `nice pixi run check`, `nice pixi run -e gcc check` and `nice pixi run tsan -R 'unit\.download'` are green. Existing goldens are untouched (report JSON keys unchanged until T11).
- **Tests (unit; fixture transport + instant clock on `temp_dir` trees from T2; FakeTransport hooks for injection):**
  - **Basic flows:**
    - clean then clean: all `complete`, no transport call to `/at-home/` or `/uploads/`;
    - clean then `clean-grown-reuse-only`: Vol 02 `incomplete` and written with `replaced`, new `[ch4]`, reused `[ch3]`, Vol 01 byte-unchanged;
    - a fresh `clean-grown` build is byte-identical to that rebuilt Vol 02 at jobs 1 and 8.
  - **Regroup:**
    - regrouped without `--group`: complete, kept substitutions in `skipped`;
    - `--group "Bravo Scans"` on `clean-regrouped-served`: both rebuilt, byte-identical to a fresh download of that tree;
    - `--group "Charlie Scans"`: complete;
    - regrouped-grown: Vol 01 is byte-identical to a fresh `clean-mixed-reference` build, contributors A then B.
  - **Damage and change:**
    - a damaged page (`patch_file_byte` at a page `data_offset`): warning, ch3 downloaded, `reused_chapters` empty, byte-identical to fresh;
    - the existing file rewritten in place from a FakeTransport hook during the cover or new-chapter fetch (so between pre-pass and copy): `failed`, exit 6, the temporary gone.
  - **Publish races:** the target unlinked from a hook (`written`, `replaced:false`); the target replaced from a hook (`exists`/`foreign`, untouched).
  - **Interrupts:** `simulate_interrupt_for_testing` from hooks during the pre-pass, the copy and the download each give `Errc::interrupted`, the old file is byte-unchanged and there is no `.part-` or temporary; both temp paths via `ScopedOutputDirFault`.
  - **Foreign:** a FIFO, a symlink and a directory at the target name; `not_ours`, `other_book` and `changed_by_other_program` via `rewrite_entries`.
  - **`plan_error`:** complete gives `exists`; incomplete gives `failed`.
  - **Dry runs:** a dry run over an existing DIR writes nothing (directory snapshot).
  - **Idempotence:** run, update, run gives all complete.
  - **Layouts:** `--layout chapters` and `single` update cases.
  - **Threads:** observer calls only on the test thread.

### T11: Report JSON, console output and help text (then the bless)
- **Goal:** the user-visible contract: new `epubs[]` keys, the closing note, the new `-v` lines, the failed-update suffix, and the reworded `--overwrite` help and footer.
- **Owns:** `src/core/report.cpp`, `src/app/cmd_download_output.hpp`, `src/app/cmd_download_output.cpp`, `src/app/cmd_download.cpp`, `tests/unit/report/report_test.cpp`, `tests/cli/download_output_test.cpp`, `tests/cli/download_cli_test.cpp` (the re-run note expectation only).
- **Build files for build-fixer:** none.
- **What changes:**
  - **`report_to_json`:** after `replaced` come `existing` (null, `"complete"`, `"incomplete"`, `"diverged"`, `"foreign"` or `"unchecked"`), then `new_chapters` and `reused_chapters` (arrays of ids). `format_version` stays 1.
  - **`ConsoleObserver`:**
    - `finish()` counts `existing == complete` and prints the unprefixed, uncoloured note `mangadex-dl: N files are already up to date; pass --overwrite to download them again` (`1 file is …`), hidden by `-q`. The old "N files already exist" warning is removed.
    - `-v` for complete: `<path>: up to date (N chapters)`.
    - `-v` for a dry-run `planned` with `existing == incomplete`: `would update <path> (1 new chapter, 2 reused: Ch. 4)`, with labels from `chapter_labels` over the new chapters. `would replace` stays for `unchecked`.
    - A failed outcome with `existing == incomplete` gets `print_error(… " (the existing file was kept)")`.
    - Everything passes through `terminal_safe`.
  - **`cmd_download.cpp`:** `--overwrite` help becomes "Re-download files even if up to date", and the footer paragraph becomes the spec text verbatim.
- **Dependencies:** T10.
- **Done when:**
  - `nice pixi run check` is green **except** `e2e.download.help` and `e2e.download.json-dry-run`. Each must fail only with the planned golden diff: the help text, and the three new keys with `existing: null`, `new_chapters: []`, `reused_chapters: []`.
  - The track hands off **bless request:** `pixi run bless '^e2e\.download\.(help|json-dry-run)$'`, reason "new epubs[] keys (existing/new_chapters/reused_chapters); reworded --overwrite help and re-run footer".
  - After the human blesses, `check` is fully green and the batch is integrated.
- **Tests:**
  - **Unit (`report_test.cpp`):** every `ExistingKind` spelling; key order; empty and non-empty lists; existing exact-document tests updated.
  - **In-process (`download_output_test.cpp`):**
    - note counting (complete only; foreign and diverged not counted), singular and plural, `-q` hides it, never ANSI with `--color=always`;
    - the "up to date" and "would update" lines (with ≤5 labels plus "and K more");
    - the error suffix;
    - `warning:` still yellow under `--color=always`, and plain under `NO_COLOR`.
  - **`download_cli_test.cpp`:** the re-run note text and the verbose expectations.

### T12: User documentation
- **Goal:** document the new contract.
- **Owns:** `README.md`, `docs/design/download-spec.md`.
- **Build files for build-fixer:** none.
- **What changes:**
  - **README:**
    - the flag table line for `--overwrite`;
    - Output (around lines 145–169): skip/update semantics, the note, `-v` lines, the JSON schema with the new keys, `exists` = "left alone", `replaced:true` without `--overwrite`, `pages` for `exists`;
    - line 308: replacement happens only for our own incomplete files, atomically and losslessly, and as a new inode, so mode, owner and hard links are not kept;
    - line 316: the `.part-` note now also applies to updates;
    - the manifest (`META-INF/mangadex-dl.json`), mixed `data`/`data-saver` chapters in a rebuild, the TOCTOU window, and `--title` orphans.
  - **`download-spec.md`:** flag table, JSON, stderr notices, the no-clobber section and acceptance 11 wording, plus a pointer to `skip-existing-spec.md`.
- **Dependencies:** none for content (from the spec and this plan). It runs in batch 4 only for review cadence.
- **Done when:** `nice pixi run check` is green, and cli-ux-reviewer reads it at the board.
- **Tests:** none.

### T13a: `[cli]` acceptance: skip, update, reuse, regroup
- **Owns:** `tests/cli/download_update_cli_test.cpp` (new). It uses `download_cli_harness.hpp`, `mdx_fixture.hpp` and `zip_reader.hpp`.
- **Tests:**
  - **Criterion 7:** clean twice gives exit 0, empty stdout, stderr **exactly** the note, and JSON `existing: "complete"` with `pages` equal to the held count.
  - **Criterion 8:** reuse proof against `clean-grown-reuse-only`: exit 0, stdout only the Vol. 02 path, JSON `incomplete` / `replaced: true` / `new_chapters: [ch4]` / `reused_chapters: [ch3]`, Vol. 01 byte-unchanged, stderr empty (proves cover reuse).
  - **Criterion 9:** the rebuilt Vol. 02 is byte-identical to a fresh `clean-grown` build at `-j1` and `-j8`.
  - **Criterion 10:** `clean-regrouped` rebuilds nothing.
  - **Criterion 11:** regrouped-grown gives a byte-identical Vol. 01 against `clean-mixed-reference`, contributors A then B, `skipped_chapters` with B's ch 1–2 kept as A's ids, the `-v` kept lines, and Vol. 02 complete.
  - **Decision 5:**
    - `--group "Bravo Scans"` on `clean-regrouped-served` rebuilds both volumes, byte-identical to a fresh B download, with the `-v` replacing lines and the re-picked ids in `new_chapters`;
    - without `--group`, nothing is rebuilt;
    - with `--group "Charlie Scans"`, nothing is rebuilt.
  - **Criterion 15:** dry run on `clean-grown` gives stdout = the Vol. 02 path, `-v` `would update … (1 new chapter, 1 reused: Ch. 4)`, JSON `planned` + `replaced: true` + lists, and no file modified (snapshot).
  - **Idempotence:** run, update, run.
  - **Layouts:** `--layout chapters` and `--layout single` update cases.
- **Dependencies:** T11 (blessed).
- **Done when:** `nice pixi run check` and `nice pixi run tsan -R 'cli\.download'` are green.

### T13b: `[cli]` acceptance: damaged, diverged, foreign, failed, TTY
- **Owns:** `tests/cli/download_existing_cli_test.cpp` (new). It uses `zip_forge.hpp` and `held_book_builder.hpp`.
- **Tests:**
  - **Criterion 12:** a damaged held page gives the warning, ch 3 downloaded, exit 0, byte-identical, and `reused_chapters: []`.
  - **Criterion 13:** `clean-diverged` gives Vol. 02 untouched, the exact diverged warning, exit 0 and `existing: "diverged"`.
  - **Criterion 14:** foreign files are each left untouched with their exact reason and remedy, exit 0, and nothing from the file echoed. Then `--overwrite` replaces all of them (`unchecked`). The cases:
    - no manifest;
    - random bytes;
    - another identifier (rezip);
    - an OPF edited without updating `opf_crc32` (rezip);
    - a symlink;
    - a FIFO;
    - a directory;
    - mode 000 (SKIP as root).
  - **Criterion 16:** a failed new-chapter download (`--group "Bravo Scans"` on `clean-regrouped`, which serves nothing) gives exit 6, both old files byte-unchanged, and an error ending "(the existing file was kept)". `-q` hides every new note and warning. `--json --color=always` produces no ESC.
  - **Criterion 20 (in-process share):** `stderr_tty` progress appears while updating (`[2/2] Vol. 02`), the note is never coloured, `warning:` is yellow with `--color=always` and plain with `NO_COLOR`.
- **Dependencies:** T11.
- **Done when:** `nice pixi run check` and `nice pixi run tsan -R 'cli\.download'` are green.

### T14: Installed-binary E2E cases and edge additions
- **Owns:** `tests/e2e/cases/download.cmake`, `tests/e2e/cases/_edge.cmake`.
- **Cases:** none are GOLDEN; exact exit codes; streams asserted separately.
  - `download.cmake`:
    - `update-reuse`: PRE_RUN clean; main `clean-grown-reuse-only`. stdout exactly `^out/Clean Title - Vol\. 02\.epub\n$`, stderr empty, EXPECT_EPUB Vol 02, EXPECT_UNCHANGED Vol 01, EXPECT_NO_GLOB `out/.*.part-*`.
    - `update-json`: `EXPECT_JSON` with `"existing": "incomplete"`.
    - `rerun-unchanged`, tightened: stderr exactly the note.
    - `up-to-date-json`: `"existing": "complete"`.
    - `up-to-date-quiet`: `-q`, stderr empty.
    - `update-dry-run`: `-v -n`; stdout = the Vol 02 path; the `would update` line; EXPECT_UNCHANGED for both.
    - `regroup-kept`.
    - `regroup-group-b` (both paths, EXPECT_EPUB both).
    - `regroup-group-c` (stdout empty, EXPECT_UNCHANGED both).
    - `update-failed`: exit 6, stdout empty, stderr regex `\(the existing file was kept\)\n`, EXPECT_UNCHANGED both.
    - `diverged-warning`: EXPECT_NO_ANSI.
    - `tty-update-progress`: TTY, merged output matches `\[2/2\] Vol\. 02`.
    - `tty-up-to-date`: TTY, EXPECT_NO_ANSI (the note is uncoloured).
  - `_edge.cmake`:
    - `dev-full-update`: PRE_RUN clean; `clean-grown-reuse-only` with `STDOUT_FILE /dev/full`. Exit 1, stderr `write error`, EXPECT_EPUB Vol 02, EXPECT_UNCHANGED Vol 01, no `.part-`.
    - `pipe-head-update`: PRE_RUN clean; `--group "Bravo Scans"` on `clean-regrouped-served` with `PIPE_HEAD 1 EXPECT_PIPE_RESULT 0` (Rollout D7). stdout exactly the Vol 01 line, stderr empty, EXPECT_EPUB both.
    - `stdin-closed-update`: PRE_RUN + `STDIN CLOSED`, exit 0.
- **Dependencies:** T3, T11 (blessed).
- **Done when:** `nice pixi run check` and `nice pixi run release-test` are green. The release-test runs the new cases on the hardened `-O2` install, where optimizer-only UB in the offset math shows up.

---

## Parallelization plan

| Batch | Tracks (parallel) | Serialization points held (one track each) |
|---|---|---|
| **Batch 1** | T1, T2, T3 | T1: every shared public header under `include/mangadex_dl/` · T2: `tests/support/mdx_fixture.hpp` · T3: the E2E driver (`CliE2E.cmake`, `run_case.cmake`) |
| **Batch 2** | T4, T5, T6, T7 | T4: `src/core/json_util.hpp` (core-private, shared with `mangadex_api.cpp`) · otherwise new files only |
| **Batch 3** | T8, T9 | none |
| **Batch 4** | T10, T12 | none (`internal.hpp` is private to T10) |
| **Batch 5** | T11, then **⏸ bless** `pixi run bless '^e2e\.download\.(help|json-dry-run)$'` | the goldens (human-blessed) |
| **Batch 6** | T13a, T13b, T14 | T14: `_edge.cmake` |

Notes:
- No batch touches `CMakeLists.txt`, `CMakePresets.json`, `pixi.toml`/`pixi.lock`, `src/app/cli.cpp` (no new subcommand), `tests/CMakeLists.txt`, `scripts/*` or `.github/*`.
- **Why T11 comes after T10.** The new note counts `existing == complete`, which only T10 produces. Landing T11 first would leave the existing re-run test unpassable. Landing T10 first keeps it passing, because the old note counts `status == exists`.
- **One bless round.** T11's JSON keys are final the moment it lands, because the lists are empty for absent files (D1). T10 never changes the report keys.
- **Shared-build hazard.** All unit tests link into one executable. Each batch only references symbols of integrated batches; for example T8 needs T4's `manifest_to_json`, and T9 needs T4 and T5.
- A compile error in another track's file is a coordination event: report it, never fix it.
- Gates: `-e gcc check` for T1, T5, T6, T9 and T10; `tsan` for T10, T13a and T13b; `release-test` for T14 and `/harden`.

---

## Test plan (per tier)

- **Unit (Catch2, `mangadex_dl::core`):**
  - T4: manifest round trip, plus the hostile JSON table.
  - T5: CD reader and `FdReadSource`, plus the hostile CD table, the fuzz-style mutation loop and read-fault injection.
  - T6: `open_existing` across every file type, and `publish_replacing` (replaced, vanished, changed) on both temp paths.
  - T7: matching, re-pick rank, final list, reconcile.
  - T8: the manifest written by EpubWriter.
  - T9: the inspection hostile table, held-page content and read faults, a fuzz loop, V7.
  - T10: end-to-end update flows over the fixture transport: byte identity, damage fallback, a change during the copy, publish races, interrupts in each phase, dry run, layouts.
- **In-process CLI (`tests/cli/`):**
  - T11 `download_output_test.cpp`: note, `-v` lines, suffix, colour, `-q`.
  - T13a and T13b cover criteria 7–16, decision 5's three `[cli]` rows, and the in-process share of 20.
- **E2E (`tests/e2e/cases/download.cmake`, T14):** criterion 17 (PRE_RUN reuse case, up-to-date re-run, JSON, dry run, regroup, failure, TTY). The driver gains `PRE_RUN` and `EXPECT_UNCHANGED` (T3).
- **Edge suite (`_edge.cmake`, T14):** `dev-full-update`, `pipe-head-update` (exit 0, D7) and `stdin-closed-update`. The existing bulk SIGPIPE cases still prove policy A.
- **Sanitizer controls:** none new. The new risk classes (heap reads from pread buffers, offset math, views into owned buffers) are covered by the existing `heap-overflow`, `dangling-view-heap` and `dangling-view-scope` controls. Unsigned wrap is not UB (and is prevented by widening). No new threads, so `data-race` covers TSan. memory-safety-reviewer confirms that the controls fired.
- **TSan:** `nice pixi run tsan` must pass whole before the board. The FetchPool scope is unchanged, but rebuilds interleave pool runs with calling-thread copies.
- **Release-test:** `nice pixi run release-test` in T14 and `/harden`. No change to `release-check.binary` or `staged-files` (no new deps or install rules).
- **Goldens:**
  - **Changed:** `tests/e2e/golden/download.help.stdout` and `tests/e2e/golden/download.json-dry-run.stdout`. One bless, after T11: `pixi run bless '^e2e\.download\.(help|json-dry-run)$'`.
  - **Unchanged:** `download.dry-run-clean`, `edge.help`, `edge.version`.
  - **Added:** none.
  - Implementers never write goldens.
- **Human (User Test Plan inputs):**
  - criterion 21 + V1 (epubcheck and ≥2 readers): `[human: epubcheck not on conda-forge · retryable]`;
  - criterion 22 (live series, `-vv`, no 429 storm);
  - criterion 23 (real Ctrl-C during the copy and during the download);
  - the real-terminal part of criterion 20.

---

## Memory-safety watch-list (for `memory-safety-reviewer`)

**The fd from inspection through build to publish**
- `ExistingHandle::fd` (UniqueFd) is owned by T10's `Assessment`, a local of `produce()` declared before the `FdReadSource`, the `TempFile` and the `EpubBuild`.
- `FdReadSource` borrows `const UniqueFd&` (LIFETIMEBOUND) and must never outlive the `Assessment`, or be stored in `RunContext` or the report.
- Complete, diverged and foreign assessments close the fd before the next file; no fd is held across files. In a dry run each file's fd is closed in its own scope.
- On every error and interrupt return the order is: `EpubBuild` destroyed (it borrows the TempFile and the source), then the TempFile discarded, then the fd closed. Check `Errc::interrupted` from the pre-pass, the copy and the pool; `changed` from publish; and `plan_error`.
- `publish_replacing(TempFile&&)` consumes the temporary on every path. The `UnlinkGuard` for the `.part-` name is dismissed only after a successful `renameat`.
- A moved-from `Assessment` or `ExistingHandle` holds -1.

**Spans and views over pread buffers**
- `read_exact` writes into caller-owned, exact-size vectors.
- No span into the CD buffer outlives `ZipIndex::read`, because names are copied into `std::string` and the buffer is freed.
- `HeldPage::bytes` lives across exactly one `add_page` call, which copies it into the TempFile buffer. The `ZipWriter` precondition holds because the image never borrows from the sink.
- `ZipIndex::find` and `entries()` return pointers or references into an immutable index: LIFETIMEBOUND, never on a temporary (`ZipIndex::read(s)->find(x)` dangles), never kept past the index.
- `foreign_reason_text` and `remedy` return static storage.

**Offset math**
- Every u16/u32 field is widened to `std::uint64_t` before summing (≤5 terms, so no overflow).
- Every slice uses `offset > size || len > size - offset`.
- `std::in_range<std::size_t>` runs before every `static_cast` to `size_t`.
- Caps are checked before allocating: CD ≤ 8 MiB, manifest ≤ 16 MiB, image ≤ 32 MiB.
- `data + csize ≤ cd_offset` for every referenced entry.

**JSON parse limits**
- Size cap, then `depth_ok`, then the non-throwing parse, then `FieldReader` only.
- The parsed manifest document is never dumped, copied or compared (V4).
- Per-string, per-chapter and total-image caps are enforced while reading.
- V16 decides whether an element-count pre-scan is needed.

**FetchPool interplay**
- Reused chapters and the cover copy run only on the calling thread, between pool runs. No `PageFetcher` or `PageConsumer` lambda captures the source, the `HeldBook` or the fd.
- Existing captures stay pointer-by-value (`source = &pages`, `build = this`).
- `pool->run` never runs concurrently with a held-page read.

**Iterators in the matching and final-list functions**
- `decide_update`, `fall_back_to_download` and `reconcile_skipped` work on indices and return new values.
- Callers must not hold a reference into `decision.chapters` or `report.skipped` across a reassignment (`decision = fall_back_to_download(std::move(decision), …)` inside the pre-pass loop: iterate by index).
- `reconcile_skipped` uses erase-if then append, with no iterator held across either.
- `FinalChapter::held` and `planned` are indices into `HeldBook::manifest.chapters` and `PlannedEpub::chapters`, which must stay unmodified while the decision is used.

**Ownership of the inspected handle on interrupt**
- `stop_now` is checked per page in the pre-pass and the copy. It unwinds through RAII only: no manual close or unlink.
- The old file is never opened for writing (`O_RDONLY` only).

**Other**
- `errno_text` uses `strerror_r` (thread-safe).
- `FileIdentity` is a plain value.
- No new threads, signal handlers or `std::function` that escapes.
- C APIs (`openat`, `fstatat`, `renameat`) get `std::string` names, never `string_view::data()`.

---

## Rollout & risk

**CLI contract**
- No flags, exit codes or subcommands change. The `--overwrite` help and the footer are reworded (bless).
- The JSON additions (`existing`, `new_chapters`, `reused_chapters`) are additive within `format_version: 1`.
- **Semantic changes, documented by T12:**
  - `exists` now means "left alone";
  - `replaced: true` can appear without `--overwrite`;
  - **`pages` for `exists` is the held count (it was 0)**;
  - `epubs[].chapters` reflects substitutions;
  - `skipped_chapters` is reconciled.
- The stderr note changes from a `warning:` line to an unprefixed note. stderr is diagnostics, but call it out in the PR.
- **Migration:** none. The user has no prior files (gate decision 3). Every new EPUB carries the manifest.

**⏸ Decisions for plan approval** (planner calls where the spec is silent, or where I deviated):
1. **D1:** `new_chapters` and `reused_chapters` are non-empty only for `existing: "incomplete"`. They are `[]` for absent, complete, diverged, foreign and unchecked. This keeps the `json-dry-run` golden final at T11 and needs a single bless.
2. **D2:** a `diverged` outcome lists the file's held chapters (from the manifest) with `pages` = held images. `foreign` and absent list the planned chapters with `pages` 0.
3. **D3:** a hardened open: `fstatat(AT_SYMLINK_NOFOLLOW)` first, so non-regular files are never opened, then `openat` + `fstat` re-check. `ELOOP`/`ENXIO`/`ENODEV` map to "not a regular file" instead of exit 1.
4. **D4:** offset math uses u64 widening, which cannot overflow, instead of the overflow builtins the spec names. Equivalent guarantee.
5. **D5:** confirms the designer's open item: a rebuild may mix `data` and `data-saver` chapters, each recorded with its own quality. The manifest spells them `"data"` and `"data-saver"`.
6. **D6:** the `*-reuse-only`, `regrouped` and `regrouped-grown` fixture variants also do not serve cover images. This proves the cover copy path; stderr stays empty.
7. **D7:** the spec's "`| head -1` across two updates → 141" cannot be made deterministic. Two short buffered lines reach the pipe in one write at exit, so the tool exits 0 (V18). The edge case therefore asserts `EXPECT_PIPE_RESULT 0`, one line, and every `.epub` valid. SIGPIPE policy A remains proven by the bulk edge cases. Alternative: flush stdout per path (streaming), which makes the result racy, so I rejected it.
8. **D8:** foreign remedy mapping:
   - not regular / cannot be read: "rename or remove it, or use another DIR";
   - not ours / holds a different book: "use another DIR or --title";
   - changed by another program / made by a newer mangadex-dl / changed while it was being updated: "pass --overwrite to replace it".
9. **D9:** a pre-pass page read error (EIO) also falls back to download ("any failed page"). During the build copy, any fault fails the file.
10. **D10:** `EXPECT_UNCHANGED` is added to the driver alongside the spec's `PRE_RUN`.
11. **D11:** the dry-run inspection also reads local headers (a few bytes per entry) to apply the same structural rules. It never reads page data.

**Riskiest task: T10** (fd lifetime across pre-pass, build and publish; fallback; races; interrupts). De-risking:
- Everything it composes is frozen in T1 and proven in batches 2–3, including the hostile tables and the publish races in T6.
- The fixtures exist from batch 1.
- The FakeTransport hooks give deterministic mid-run injection.
- The gcc and tsan gates are required.

**Second risk: T5/T9 parsing hostile bytes.** Exhaustive truncation, single-byte flips and a fixed-seed mutation loop under ASan+UBSan, and the release-test at `-O2`.

---

## Refactor triggers

| If refuted | Tasks that change |
|---|---|
| V1 (epubcheck rejects or warns on the extra META-INF file) | Design goes back to cli-designer: the manifest moves to an OPF `<meta>` or an OEBPS file. T1 (`book_manifest.hpp` entry name), T4, T8, T9, T12. |
| V9 (epubcheck *is* on conda-forge) | No code change. Optionally a test-only `pixi add` proposal (ask, serialization point) and an epubcheck E2E case. V1 leaves the User Test Plan. |
| V10 (OCF forbids extra META-INF files) | Same as V1. |
| V3 (a FIFO open blocks) | T6 relies only on the `fstatat` pre-check, plus `O_NONBLOCK`; the TOCTOU note goes into T12. |
| V5 / V6 (rename-over or pread semantics) | T6 `publish_replacing` (copy-then-rename into a fresh name), T10 (re-read before publish), T12. |
| V11 (errno for a symlink or socket) | T6 errno mapping, T13b expectations. |
| V12 (mtime granularity) | T1 `FileIdentity` (add `st_ctim`), T6, T10 change detection. |
| V13 (pread semantics) | T5 `read_exact` loop. |
| V4 (recursion in parse or destroy) | T4 (tighter `max_manifest_depth`; already pre-scanned). |
| V14 (dump not compact or deterministic) | T4 writer (manual serialization), T8 byte-identity tests. |
| V15 (number typing) | T4 field reads. |
| V16 (RSS too high under the 16 MiB cap) | T4 (element-count pre-scan or a lower `max_manifest_bytes`; T1 constant mini-amendment). |
| V7 (sniff differs) | T9 (store media info in the manifest instead), T1 `ManifestChapter`, T4. |
| V8 (zip_reader unusable for offsets) | T5 `zip_forge` (own offset scanner), T13b. |
| V17 (`file(SHA256)` or env in `-P`) | T3 (compare with `file(READ … HEX)`), T14. |
| V18 (the pipe result is not deterministically 0) | T14 `pipe-head-update` dropped or replaced; spec criterion 19 goes back to cli-designer. |
| V19 / V20 (types and casts) | T1 `FileIdentity` field types and includes, T6. |
| V21 (clang-tidy findings) | T5, T6 (NOLINTNEXTLINE with a reason, or a restructure). |

---

## Relevant paths
- Spec: `/home/user/Mangadex-dl/docs/design/skip-existing-spec.md`
- Prior plan format: `/home/user/Mangadex-dl/docs/design/download-plan.md`, `/home/user/Mangadex-dl/docs/design/download-plan-rev1.md`
- Shared headers (T1): `/home/user/Mangadex-dl/include/mangadex_dl/epub.hpp`, `report.hpp`, `planner.hpp`, `fs/output_dir.hpp` (plus the new `book_manifest.hpp`, `zip_index.hpp`, `existing_epub.hpp`, `update_plan.hpp`, `fs/read_source.hpp`)
- Integration: `/home/user/Mangadex-dl/src/core/download/execute_phase.cpp`, `/home/user/Mangadex-dl/src/core/download/internal.hpp`
- Core edits: `/home/user/Mangadex-dl/src/core/epub.cpp`, `/home/user/Mangadex-dl/src/core/fs/output_dir.cpp`, `/home/user/Mangadex-dl/src/core/report.cpp`, `/home/user/Mangadex-dl/src/core/json_util.hpp`, `/home/user/Mangadex-dl/src/core/planner.cpp`
- App: `/home/user/Mangadex-dl/src/app/cmd_download_output.cpp`, `/home/user/Mangadex-dl/src/app/cmd_download.cpp`
- Test support: `/home/user/Mangadex-dl/tests/support/mdx_fixture.hpp`, `/home/user/Mangadex-dl/tests/support/zip_reader.hpp`
- E2E driver: `/home/user/Mangadex-dl/tests/e2e/CliE2E.cmake`, `/home/user/Mangadex-dl/tests/e2e/run_case.cmake`; cases: `/home/user/Mangadex-dl/tests/e2e/cases/download.cmake`, `/home/user/Mangadex-dl/tests/e2e/cases/_edge.cmake`
- Goldens to re-bless: `/home/user/Mangadex-dl/tests/e2e/golden/download.help.stdout`, `/home/user/Mangadex-dl/tests/e2e/golden/download.json-dry-run.stdout`
- Docs: `/home/user/Mangadex-dl/README.md`, `/home/user/Mangadex-dl/docs/design/download-spec.md`
