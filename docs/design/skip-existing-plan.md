# Implementation plan (rev 1, after fact-check): `download` skips up-to-date EPUBs and rebuilds incomplete ones with page reuse

Design: `/home/user/Mangadex-dl/docs/design/skip-existing-spec.md` (rev 2, APPROVED). Gate decision 5 (re-pick on `--group`) wins over the body's "keep the held release" rule wherever they conflict. Prior plan format: `/home/user/Mangadex-dl/docs/design/download-plan.md`.

What I checked on 2026-10-19 (orientation only):
- `pixi list`: CLI11 2.7.2, nlohmann_json 3.12.0, Catch2 3.16.1, cmake 4.4.4, clang 23.1.3.
- `pixi search epubcheck` and `pixi search '*epubcheck*'` both return "No packages found" (V9, now confirmed).

---

## Changes from fact-check

- **V19 refuted (the reasoning, not the choice).**
  - GCC 16 `-Wuseless-cast` does not fire on lvalue or xvalue operands, so `static_cast<std::uint64_t>(st.st_dev)` is silent.
  - It does fire on prvalues that already have the target type: call results such as `static_cast<std::ptrdiff_t>(::pread(…))`, and arithmetic results.
  - `FileIdentity` keeps native types, now because they are the simplest choice and need no casts.
  - New `-e gcc` rule for every task (Conventions): never `static_cast` a call or arithmetic result to the type it already has. A real conversion such as `static_cast<std::size_t>(ssize_t)` is fine.
- **V20 refined.**
  - `output_dir.hpp` includes `<sys/types.h>` (for `dev_t`/`ino_t`/`off_t`) and `<ctime>` (for `time_t`). It does not include `<sys/stat.h>`.
  - `output_dir.cpp`, which calls `fstat`/`fstatat`, includes `<sys/stat.h>`.
  - include-cleaner reports only for the main file, so header hygiene is checked in the TUs that include it (the headers_test TU).
- **V12 refined.** `FileIdentity` gains `st_ctim` (seconds and nanoseconds). The cost is that a chmod or a link-count change between inspection and publish gives a conservative `changed`.
- **V5 ABA.** On ext4 a freed inode number is reused at once. The `(dev, ino)` comparison is therefore ABA-safe only while the inspection fd stays open through `publish_replacing`. This is now a T6 doc-comment contract and a T10 invariant: the `Assessment` owns the fd until publish returns.
- **V11 refined.**
  - A directory, a FIFO and a char device all open successfully with `O_RDONLY|O_NOFOLLOW|O_NONBLOCK`, so both checks stay: `fstatat` first, then `fstat` + `S_ISREG` after the open.
  - Errnos: `ELOOP` for any symlink (dangling and loops included); `ENXIO` for a UNIX socket and for a driverless char device; `ENODEV` kept (harmless, not reproduced).
  - T6 tests assert the exact errnos through a new test-only seam that skips the `fstatat` pre-check.
- **V13 refined.** `FdReadSource::read_exact` advances on any positive count, retries `EINTR`, and reports `short_read` only on a 0 return before completion.
- **V6.** overlayfs is untested (noted). T6 keeps testing both the O_TMPFILE path and the forced fallback.
- **T6 compile hazard.** The anonymous-namespace free function `open_existing(const std::filesystem::path&)` (`output_dir.cpp:384`, called from 596 and 611) would be hidden by the new member `OutputDir::open_existing`. T6 renames the free function to `open_directory`.
- **V21.** `::openat` and `::fcntl` are variadic and trip `cppcoreguidelines-pro-type-vararg`. T6 reuses the existing `open_at` wrapper; any new direct variadic call gets `// NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): <reason>`. `::pread` and `span::subspan` need nothing.
- **V16 adopted.**
  - A hostile 16 MiB manifest peaks at about 0.5 GiB in the release build and 0.85 GiB under ASan.
  - T4 adds a structural-separator bound, `max_manifest_separators = 1 << 21`, to the pre-scan, justified against `zip_max_entries`.
  - Hostile tests use 1–2 MiB documents. No unit test builds a full 16 MiB adversarial document; the 16 MiB + 1 byte row checks only the byte cap, before any parse.
  - The `format` rows gain `1e0`, `-0` and `18446744073709551616` (2^64), all rejected.
- **V18.** D7 stands. T14 and D7 now state the constraint: stdout stays fully buffered (nothing flushes per path), and that case's output stays under 4 KiB.
- **V10 / V1 wording.**
  - EPUB 3.3 §4.2.6.1 reserves META-INF "for configuration files". Extra files are not prohibited, which is a grey zone, not a permission.
  - RS 3.3 §4.1.3: reading systems "MUST NOT fail" on unlisted META-INF files (there is no "ignore" rule).
  - The §4.2.2 rule that META-INF files must not be listed in the OPF manifest is confirmed.
  - epubcheck's source suggests it gives no message, but V1 stays human-only.
- **V7 adopted.** In T9 the held-entry extension check compares `file_extension(info.type)`, the builder's own function, against the entry's extension.
- **Confirmed, no change:** V3, V4, V8, V9, V14, V15, V17, V21 (`pread`/`subspan`).

---

## Approach

The logic lands as four pure or near-pure core modules behind interfaces frozen first in T1:
- the manifest model and its JSON;
- a bounded central-directory (CD) reader over an abstract `ReadSource` (pread on an fd, or a fake in tests);
- inspection of an existing file plus held-page reads;
- the matching and final-list decision.

The download orchestration integrates them late, in `execute_phase.cpp` plus one new private file. Everything is unit-tested with fake sources and hostile tables before any CLI behaviour changes.

`report_to_json`, the console note and the help text change in one late task, so the two goldens are blessed in a single round. The trade-off is six batches and modest parallelism (at most four tracks), in exchange for no cross-track symbol or golden dependency inside a batch. Reused pages are read twice (a validation pass, then the copy) to keep memory at one image.

---

## Dependencies & pixi/CMake changes

**None.**
- No `pixi add`, so `pixi.toml` and `pixi.lock` are untouched.
- No change to `CMakeLists.txt`, `CMakePresets.json`, `tests/CMakeLists.txt`, `cmake/*`, `scripts/*` or CI.
- New sources go under `src/core/**` (GLOB_RECURSE with CONFIGURE_DEPENDS, `CMakeLists.txt:89`). New tests go under `tests/unit/**` (`tests/unit/CMakeLists.txt:7`, recursive) and `tests/cli/*.cpp`. New test-support headers are header-only.
- epubcheck is not added. It is not on conda-forge (V9 ✅), and it would be a Java runtime dependency. It stays human-only (V1).
- The E2E driver (`CliE2E.cmake`, `run_case.cmake`) gains two keywords, `PRE_RUN` and `EXPECT_UNCHANGED`. That is a serialization point, owned by T3.
- No new pixi tasks, presets or CMake options.

---

## Assumptions to verify (status after fact-check)

The spec's V2 was dropped in rev 2. V1 and V3–V8 are carried over with their original numbers.

**Cluster A: EPUB / epubcheck / conda-forge**
- **V1** `[human-only]` epubcheck (current 5.x) reports no error and no warning for an extra STORED `META-INF/mangadex-dl.json` that is not listed in the OPF.
  - epubcheck's source suggests no message: `OCFChecker` exempts `META-INF/*` from OPF-003, and `checkOtherMetaFiles` validates only container, metadata and signatures.
  - It cannot be run here (V9), so it goes to the User Test Plan as `[human: epubcheck not installable via pixi · retryable]`.
  - If it reports anything, switch to an OPF `<meta>`.
- **V9** ✅ epubcheck is not on conda-forge.
- **V10** ⚠ wording corrected.
  - EPUB 3.3 §4.2.6.1 says META-INF is "reserved for configuration files". Extra files are not prohibited: a grey zone, with `com.apple.ibooks.display-options.xml` as precedent.
  - §4.2.2: META-INF files MUST NOT be listed in the package manifest (confirmed).
  - RS 3.3 §4.1.3: reading systems MUST NOT fail on such files. The spec has no "ignore" rule.

**Cluster B: Linux / glibc syscalls**
- **V3** ✅ `openat(…O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC)` of a FIFO with no writer returns at once, and `fstat` reports `S_ISFIFO`.
- **V5** ✅ with an ABA caveat. `renameat` over a file a reader holds open is safe, and the reader keeps the old inode. On ext4 a freed inode number is reused immediately once no fd holds it, so the identity comparison is ABA-safe **only while the inspection fd stays open through `publish_replacing`** (T6 contract, T10 invariant).
- **V6** ✅ (overlayfs untested). `pread` on our fd reads the inspected inode after a rename-over, on both the O_TMPFILE and the named-fallback publish paths. overlayfs was not tested; T6 tests both paths.
- **V11** ⚠ refined.
  - `ELOOP` for any symlink (dangling and loops included); `ENXIO` for a UNIX socket and for a driverless char device. `ENODEV` was not reproduced; keeping it in the map is harmless.
  - A directory, a FIFO and a char device **open successfully**, so the post-open `fstat` + `S_ISREG` check is mandatory. `fstatat(AT_SYMLINK_NOFOLLOW)` classifies all of these without opening them.
- **V12** ⚠ refined. `st_mtim` alone missed a same-size rewrite whose mtime was restored with `futimens`, while `st_ctim` still differed. `FileIdentity` therefore holds `(st_dev, st_ino, st_size, st_mtim, st_ctim)`. The cost is that a chmod or link-count change gives a conservative `changed`.
- **V13** ⚠ refined. `pread` may return a positive short count: the `MAX_RW_COUNT` cap, a signal after a partial transfer, NFS or FUSE. So: advance on any positive count, retry `EINTR`, and treat only a 0 return before completion as `short_read`. `O_NONBLOCK` has no effect on regular files.

**Cluster C: nlohmann_json 3.12.0**
- **V4** ✅ Parsing and destruction are iterative. Dump, copy and `==` recurse, so the parsed manifest document is never dumped, copied or compared, and `depth_ok` runs first.
- **V14** ✅ `ordered_json::dump(-1, ' ', false, replace)` is compact, keeps insertion order, and is deterministic.
- **V15** ✅ `1` parses as `number_unsigned`. `1.0`, `-1`, `"1"`, `true`, **`1e0`, `-0` and `18446744073709551616`** are all rejected for `format`.
- **V16** ⚠ adopted. A hostile 16 MiB manifest peaks at about 0.5 GiB in the release build and about 0.85 GiB under ASan.
  - T4 adds a separator-count bound (`max_manifest_separators = 1 << 21`) to the pre-scan, which cuts the worst case by about 3x.
  - Tests stay at 1–2 MiB.

**Cluster D: image sniffing**
- **V7** ✅ by construction: the same bytes give the same `ImageInfo`. Adopted: T9's extension check uses `file_extension(info.type)`.

**Cluster E: CMake 4.4.4 (floor 3.31), E2E driver, test support**
- **V8** ✅ `zip_reader.hpp` reads, CRC-verifies and exposes `data_offset`, but has no writer. Tests locate offsets before patching (after a patch, `read_archive` fails on the CRC). Rewrites go through core `ZipWriter` in `tests/support/zip_forge.hpp`.
- **V17** ✅ `file(SHA256)` works in `-P` mode, and sequential `execute_process` calls share the `set(ENV{})` environment.
- **V18** ✅ A tool writing fewer than 4 KiB of fully buffered stdout into `| head -n 1` exits 0, deterministically. This depends on stdout staying fully buffered (see D7).

**Cluster F: compiler warnings and clang-tidy**
- **V19** ✗ refuted, then reworded.
  - `-Wuseless-cast` (GCC 16.2) is silent on lvalue and xvalue operands. `static_cast<std::uint64_t>(st.st_dev)` would compile.
  - It fires on **prvalues that already have the target type**: `static_cast<std::ptrdiff_t>(::pread(…))` (`ssize_t` is `long`), `static_cast<off_t>(v + 1)` with an `int64_t` `v`, and other arithmetic results.
  - `FileIdentity` still uses the native types, because that is simplest and needs no casts.
- **V20** ⚠ refined. `<sys/types.h>` and `<sys/stat.h>` do not satisfy include-cleaner for `time_t`/`timespec`; `<ctime>` is required. The header includes `<sys/types.h>` + `<ctime>` only. The `.cpp` that calls `fstat` includes `<sys/stat.h>`. Header hygiene is checked in the TUs that include the header.
- **V21** ✅ with a note. `::pread` and `span::subspan` need no NOLINT. `::open`, `::openat` and `::fcntl` are variadic and fire `cppcoreguidelines-pro-type-vararg`: reuse the existing `open_at` wrapper in `output_dir.cpp`, or put `// NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): <reason>` before each new call.

**Clusters:** Cluster A: V1, V9, V10 · Cluster B: V3, V5, V6, V11, V12, V13 · Cluster C: V4, V14, V15, V16 · Cluster D: V7 · Cluster E: V8, V17, V18 · Cluster F: V19, V20, V21.

Open item: only V1 (human).

---

## Task breakdown

**Conventions for every task:**
- Namespace `mangadex_dl`. Aggregates take DMIs and designated initializers. Classes with an invariant get `[[nodiscard]]` factories.
- `#pragma once`. No nlohmann in public headers.
- Every "Done when" includes `nice pixi run check` green (long timeout) and **both** track reviewers APPROVED.
- Within a batch, a track's code and tests may only call symbols defined by that track itself or by an already-integrated batch. All unit tests link into one executable.
- The T1 headers are **frozen** from batch 2 on. A needed signature change is a coordination event (the orchestrator runs a mini-amendment between batches).
- **GCC `-Wuseless-cast` rule (V19):** never `static_cast` a call result or an arithmetic result to the type it already has. Examples: `static_cast<ssize_t>(::pread(…))`, `static_cast<std::uint64_t>(a + b)` where both are `std::uint64_t`, `static_cast<off_t>(off + 1)`. Converting between different types (`static_cast<std::size_t>(ssize_t)`, after a range check) is fine. Casting a named lvalue of the same type is silent but pointless, so don't do it either.
- **Variadic C calls (V21):** `::open`, `::openat` and `::fcntl` go through the existing `open_at` wrapper (or a sibling), or get `// NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): <reason>`.

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
    - Constants: `manifest_entry_name = "META-INF/mangadex-dl.json"`, `manifest_generator = "mangadex-dl"`, `manifest_format = 1` (`std::uint64_t`), `max_manifest_bytes = 16 MiB`, `max_manifest_string_bytes = 4096`, `max_manifest_depth = 8`, **`max_manifest_separators = std::size_t{1} << 21`** (V16; justified in T4).
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
    - `class FdReadSource final : public ReadSource{ FdReadSource(const UniqueFd& fd MANGADEX_DL_LIFETIMEBOUND, std::uint64_t size) noexcept; … }`. It borrows the fd, and the size is fixed at inspection. Doc comment: positive short counts are continued; only a 0 return before completion is `short_read` (V13).
  - **`zip_index.hpp`:**
    - `max_central_directory_bytes = 8 MiB`.
    - `struct ZipEntryRecord{std::string name{}; std::uint16_t flags = 0, method = 0; std::uint32_t crc32 = 0, compressed_size = 0, uncompressed_size = 0, local_header_offset = 0;}`
    - `enum class ZipFault{read_error, short_read, size_out_of_range, no_end_record, unsupported, bad_directory, duplicate_name, bad_local_header, data_out_of_bounds};`
    - `struct ZipError{ZipFault fault = ZipFault::bad_directory; int error = 0;}`
    - `class ZipIndex{ static std::expected<ZipIndex, ZipError> read(ReadSource&); const ZipEntryRecord* find(std::string_view) const noexcept LIFETIMEBOUND; const std::vector<ZipEntryRecord>& entries() const noexcept LIFETIMEBOUND; std::uint64_t central_directory_offset() const noexcept; }` Move-only value. Entries are kept in CD order; the index is immutable after `read`.
    - `[[nodiscard]] std::expected<std::uint64_t, ZipError> locate_entry_data(ReadSource&, const ZipIndex&, const ZipEntryRecord&);` returns the first data byte. It guarantees the local header agrees with the CD and that the data lies within `[0, cd_offset)`.
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
    - `[[nodiscard]] PlannedEpub final_epub(const PlannedEpub&, const UpdateDecision&);`
    - `[[nodiscard]] std::vector<std::string> new_chapter_ids(const UpdateDecision&);` and `reused_chapter_ids(const UpdateDecision&);`
    - `[[nodiscard]] std::vector<SkippedChapter> reconcile_skipped(std::vector<SkippedChapter>, std::span<const KeptRelease>, const PlannedEpub&);`
    - `[[nodiscard]] std::string chapter_labels(std::span<const Chapter>, std::size_t max_shown = 5);`
  - **`epub.hpp` additions:**
    - `inline constexpr std::string_view opf_entry_name = "OEBPS/content.opf";`
    - `[[nodiscard]] std::string page_image_entry(std::size_t index, MediaType);` and `cover_image_entry(MediaType);`
    - `struct EpubBookChapter{ManifestChapter record{}; std::size_t first_page = 0; std::size_t page_count = 0;}`
    - `[[nodiscard]] Result<void> EpubWriter::finish(std::optional<std::string> cover_file_name, std::vector<EpubBookChapter> chapters);` It does what `finish()` does, then writes the manifest last. The existing `finish()` stays: no manifest, used by tests.
  - **`report.hpp`:**
    - `enum class ExistingKind{none, complete, incomplete, diverged, foreign, unchecked};`
    - `EpubOutcome` gains, after `error`, `ExistingKind existing = ExistingKind::none; std::vector<std::string> new_chapters{}; std::vector<std::string> reused_chapters{};`.
    - Doc comments: `exists` now means "left alone"; `replaced` also covers updates.
  - **`fs/output_dir.hpp` additions:**
    - Includes: `<sys/types.h>` (`dev_t`, `ino_t`, `off_t`) and `<ctime>` (`time_t`). **Not** `<sys/stat.h>` (V20).
    - `struct FileIdentity{dev_t device{}; ino_t inode{}; off_t size{}; time_t mtime_sec{}; long mtime_nsec{}; time_t ctime_sec{}; long ctime_nsec{};}` with a defaulted `==`.
      - Native types are used because they are the simplest and need no casts (V19). The fields are assigned straight from `struct stat` lvalues.
      - `ctime` is included per V12. A chmod or link-count change therefore gives a conservative `changed`.
    - `enum class ExistingOpen{absent, opened, not_regular, unreadable};`
    - `struct ExistingHandle{ExistingOpen status = ExistingOpen::absent; UniqueFd fd{}; FileIdentity identity{}; int error = 0;}` (`error`: the errno that decided `unreadable`, or `not_regular` when an open failed; 0 when `fstat` decided)
    - `[[nodiscard]] Result<ExistingHandle> OutputDir::open_existing(std::string_view name) const;`
    - `enum class ReplaceOutcome{replaced, written, changed};`
    - `[[nodiscard]] Result<ReplaceOutcome> OutputDir::publish_replacing(TempFile&&, std::string_view final_name, const FileIdentity& expected);`
    - Doc contract (V5 ABA): `expected` is meaningful only while the `ExistingHandle` fd that produced it is still open. The caller must keep that fd open until `publish_replacing` returns, otherwise a reused inode number can match.
  - **`planner.hpp` / `planner.cpp`:** `[[nodiscard]] std::size_t group_preference_rank(std::span<const ScanGroup> groups, std::span<const std::string> preferred);` is the anonymous `preference_rank` made public: ASCII case-insensitive name or id, and `preferred.size()` when nothing matches. `pick_release` calls it, with behaviour unchanged.
- **Dependencies:** none.
- **Done when:** `nice pixi run check` **and** `nice pixi run -e gcc check` are green, and `headers_test.cpp` includes every new or edited header (so include-cleaner checks header hygiene through that TU, V20).
- **Tests (unit):**
  - `headers_test.cpp` includes the headers.
  - `static_assert`s on `manifest_entry_name` and `max_manifest_bytes == max_api_json_bytes`.
  - `EpubOutcome{}` defaults; `FileIdentity{} == FileIdentity{}`.
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
    - Every shared chapter keeps byte-identical metadata and images with `clean`.
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
  - Unserved chapters have no at-home document and no images; unserved covers are absent.
  - Shared chapters are equal field by field across variants.
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
    - The driver takes `file(SHA256)` of each file after the pre-run, where a missing file fails the case, and again after the main run. They must match (V17 ✅).
  - Header comment updated. The spec file carries `CASE_PRE_RUN` and `CASE_EXPECT_UNCHANGED`.
  - **Self-test case `rerun-unchanged`** in `download.cmake`: PRE_RUN `download --api-base file://{{FIXTURES}}/clean <id> out`, then the same command. Asserts EXPECT_EXIT 0, EXPECT_STDOUT_EMPTY, EXPECT_UNCHANGED for both volumes, and EXPECT_NO_GLOB `out/.*.part-*`. Stderr is deliberately not asserted yet; T14 tightens it.
- **Dependencies:** none.
- **Done when:** `nice pixi run check` is green; `nice pixi run test -R '^e2e\.'` is green; the hand-off shows a local (uncommitted) negative check, where a PRE_RUN with a bad id reports "pre-run failed".
- **Tests:** the self-test case. The existing E2E and edge suites stay green.

### T4: Manifest model and JSON
- **Goal:** a deterministic manifest writer, and a hostile-input-safe parser with bounded memory.
- **Owns:** `src/core/book_manifest.cpp` (new), `src/core/json_util.hpp`, `src/core/json_util.cpp`, `tests/unit/book_manifest/book_manifest_test.cpp` (new), `tests/unit/book_manifest/book_manifest_hostile_test.cpp` (new).
- **Build files for build-fixer:** none.
- **What changes:**
  - **`json_util`:**
    - New overload `parse_untrusted(std::string_view, std::string_view what, const ParseLimits&)`, where `ParseLimits{std::size_t max_bytes; std::size_t max_depth; std::size_t max_separators; Errc code;}`.
    - `max_separators` (0 = unlimited) is enforced by the existing linear pre-scan. It counts the structural characters `, : [ {` outside strings, in the same pass as `depth_ok`, without allocating or recursing.
    - `FieldReader` gets an optional `Errc` (default `network`).
    - Existing signatures and behaviour are unchanged, so `mangadex_api.cpp` is untouched.
  - **Separator bound (V16):** `max_manifest_separators = 1 << 21` (2,097,152). The justification goes in a code comment and a test:
    - An EPUB holds at most `zip_max_entries` (65534) entries, and every page costs two (image + XHTML). So a file has at most about 32764 pages, and therefore at most 32764 chapters.
    - Our writer emits about 20 separators per chapter, plus 4 per group, plus 1 per image. That is about 1.2M for 32764 chapters with 4 groups each: well under 2^21.
    - Only a pathological file at the entry cap with more than about 11 groups per chapter would exceed it. That file would be classified `foreign` (a warning; `--overwrite` fixes it), which is accepted.
  - **`manifest_to_json`:** `ordered_json`, keys in the order `format, generator, identifier, opf_crc32` (8 lowercase hex), `cover` (null or `{file_name, entry}`), `chapters[{id, volume, chapter, title, groups[{id,name}], publish_at, updated_at, quality ("data"|"data-saver"), images[]}]`. Written with `dump(-1, ' ', false, replace)` (V14 ✅). No clock, no version.
  - **`parse_manifest`:**
    - Cap `max_manifest_bytes`, depth `max_manifest_depth`, separators `max_manifest_separators`, non-throwing parse, `FieldReader`.
    - `format` must be an unsigned integer: `> 1` gives `newer_format`, `< 1` gives `malformed`. `generator` must be exactly `"mangadex-dl"`.
    - Every string ≤ 4 KiB. Chapter and group ids satisfy `is_uuid`. `opf_crc32` is exactly 8 lowercase hex digits. `quality` is from the enum.
    - The `chapters` array is non-empty, and each chapter has 1..`max_pages_per_chapter` images. Total images ≤ `zip_max_entries`.
    - Entry names satisfy `is_held_image_entry_name`. Unknown keys are ignored.
    - Every failure maps to `malformed`, never a throw. The parsed document is never dumped, copied or compared (V4).
  - `manifest_chapter_from`, `chapter_from` and `is_held_image_entry_name` are implemented here.
- **Dependencies:** T1.
- **Done when:** `nice pixi run check` is green.
- **Tests (unit):**
  - Round trip `parse(to_json(m)) == m` for 0 and 1 covers, null labels, unicode titles, both qualities.
  - Exact compact bytes for one small manifest. Two calls give the same bytes.
  - **Bound test:** a manifest built in memory with 32764 one-image chapters and 4 groups each passes `parse_manifest`, and its separator count is below `max_manifest_separators`.
  - **Hostile table:**
    - each required key missing or of the wrong type;
    - `format` 0, 2, `1.0`, **`1e0`, `-0`, `18446744073709551616`**, `"1"`, `true` (V15);
    - generator other;
    - a 4097-byte string;
    - a non-UUID id;
    - opf_crc32 upper-case or 7 hex digits;
    - 5001 images in one chapter;
    - 65535 total images (with cheap names);
    - an empty chapters array or empty images;
    - bad names (`OEBPS/images/../p00001.png`, `OEBPS/images/p1.png`, `OEBPS/images/p00001.svg`, `mimetype`);
    - depth 9, 1000 and 100000 (the pre-scan rejects them without recursion);
    - **a separator flood of 1–2 MiB** (`[0,0,0,…]` with more than 2^21 separators) rejected by the pre-scan before nlohmann parses it;
    - 16 MiB + 1 bytes (rejected by the byte cap before any parse);
    - invalid UTF-8;
    - trailing garbage, a BOM, an empty document.
    - Every row gives the expected fault, never a crash, ASan-clean.
  - **Memory rule (V16):** no unit test builds or parses a full-size (16 MiB) adversarial document. The largest parsed hostile input is 2 MiB (an ASan OOM risk under `ctest -j`).
  - The `json_util` overload: cap, depth, separators, and the error class propagated.

### T5: Read source and bounded zip index (CD reader)
- **Goal:** a generic, overflow-safe STORED-zip CD reader over `ReadSource`, plus test fakes and forging tools.
- **Owns:** `src/core/fs/read_source.cpp` (new), `src/core/zip_index.cpp` (new), `tests/support/fake_read_source.hpp` (new), `tests/support/zip_forge.hpp` (new), `tests/unit/fs/read_source_test.cpp` (new), `tests/unit/zip_index/zip_index_test.cpp` (new), `tests/unit/zip_index/zip_index_hostile_test.cpp` (new).
- **Build files for build-fixer:** none.
- **What changes:**
  - **`FdReadSource::read_exact`:**
    - Rejects `offset > size || len > size - offset` as `short_read` with no syscall.
    - Then a `pread` loop (V13):
      - a positive count, short or full, advances the buffer and the offset and continues;
      - `-1` with `EINTR` retries;
      - any other `-1` is `io` with errno;
      - a `0` return before completion is `short_read`.
    - Converting `ssize_t` to `std::size_t` uses `static_cast<std::size_t>` after the `> 0` check (a real conversion). Never cast the call result to its own type (V19).
  - **`ZipIndex::read`:**
    - Requires `22 ≤ size ≤ zip_max_u32`, then reads the EOCD at exactly `size-22`. Checks: the signature; comment length 0; disk fields 0; both entry counts equal and ≤ `zip_max_entries`; no 0xFFFF or 0xFFFFFFFF sentinels; `cd_size ≤ 8 MiB`; `cd_offset + cd_size ≤ size-22`.
    - All sums are of u16/u32 fields widened to `std::uint64_t` (at most five terms, so no overflow).
    - Reads the CD into one exact-size buffer and walks it with slice-before-read bounds checks. Each entry's name is copied into an owned `std::string`, and a duplicate name gives `duplicate_name`. The CD buffer is freed before returning, and a sorted name index is built.
  - **`locate_entry_data`:**
    - Reads 30 bytes plus the name length at `lho`. The signature, flags, method, CRC, sizes, name length and name bytes must equal the CD.
    - `data = lho + 30 + name_len + extra_len`, and `data + csize ≤ cd_offset`.
  - **Test support:**
    - `fake_read_source.hpp`: a byte-vector source with fault injection: EIO at read #k, a 0 return at read #k, **a positive short count at read #k** (which must be continued, not failed), mutated bytes after read #k, and a reported size different from the data. It counts reads.
    - `zip_forge.hpp`: archive building through core `ZipWriter`; raw mutators; `patch_file_byte`; `rewrite_entries` (V8 ✅).
- **Dependencies:** T1.
- **Done when:** `nice pixi run check` and `nice pixi run -e gcc check` are green.
- **Tests (unit):**
  - Happy path on `ZipWriter` output; `find` and `entries` order; `locate_entry_data` equals `zip_reader`'s `data_offset`.
  - `FdReadSource` on a temp file: exact; EOF short read; an overflowing offset+length (rejected with no syscall).
  - Fake source: a positive short count followed by the rest gives success; a 0 return gives `short_read`; EIO gives `io`.
  - **Hostile CD table:** each row must give a `ZipError`, never a crash, ASan/UBSan-clean.
    - truncation at every length 0..N;
    - a 1-byte comment;
    - EOCD not at `size-22`;
    - disk ≠ 0;
    - entry counts unequal;
    - count = 0xFFFF;
    - cd_size or cd_offset = 0xFFFFFFFF;
    - `cd_offset = cd_size = 0xFFFFFFFE`;
    - a claimed cd_size of 9 MiB (rejected before allocation);
    - a bad central header signature;
    - name, extra or comment lengths overrunning the CD;
    - a duplicate name;
    - method 8 recorded;
    - a local header mismatch in each field;
    - `lho` pointing into the CD or past EOF;
    - a local extra length pushing the data past `cd_offset`;
    - size 21 and size `zip_max_u32 + 1` via the fake size.
  - **Fuzz-style loop:** fixed-seed `std::mt19937`, 2000 mutations of 1–4 bytes, plus an exhaustive single-byte flip over the EOCD and CD. The seed is printed with `INFO`.
  - **Read faults:** EIO and a 0 return injected at each read index.

### T6: OutputDir: open an existing file, replace it if unchanged
- **Goal:** fd-relative opening of `DIR/<name>` with an identity record, and an atomic publish that replaces only an unchanged inode.
- **Owns:** `src/core/fs/output_dir.cpp`, `src/core/fs/output_dir_testing.hpp`, `src/core/fs/errno_text.hpp` (new, core-private), `src/core/fs/errno_text.cpp` (new), `tests/unit/fs/output_dir_existing_test.cpp` (new).
- **Build files for build-fixer:** none.
- **What changes:**
  - **Compile hazard fix (first step):**
    - Rename the anonymous-namespace free function `open_existing(const std::filesystem::path&)` (`output_dir.cpp:384`) to **`open_directory`**, and update its unqualified call sites in `OutputDir::open` (596) and `OutputDir::open_if_exists` (611).
    - Otherwise the new member `OutputDir::open_existing` would hide it, which a probe confirmed is a compile error.
  - **`errno_text`:** the anonymous `errno_text` and `strerror_r` overloads move to `fs/errno_text.{hpp,cpp}` (`[[nodiscard]] std::string errno_text(int)`).
  - **Includes:** `output_dir.cpp` keeps `<sys/stat.h>` (it calls `fstat`/`fstatat`) and includes `<ctime>` if it names `time_t` (V20).
  - **`open_existing(name)`** (V11: both checks are mandatory, because a directory, a FIFO and a char device all open successfully):
    1. `fstatat(fd_, name, AT_SYMLINK_NOFOLLOW)`: `ENOENT` gives `absent`, and not `S_ISREG` gives `not_regular` **without opening**. `EACCES`/`EPERM`/`EIO` give `unreadable` with the errno; any other errno gives `Errc::failure`.
    2. `open_at(fd_, name, O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC)`, reusing the existing variadic wrapper (V21).
       - `ENOENT` gives `absent` (it vanished).
       - `ELOOP` (any symlink, dangling and loops included), `ENXIO` (a UNIX socket or a driverless char device) and `ENODEV` (kept, harmless) give `not_regular`, with `error` set.
       - `EACCES`/`EPERM`/`EIO` give `unreadable`.
       - Any other errno gives `Errc::failure` (`EMFILE` included, so exit 1).
    3. `fstat` on the fd: anything other than `S_ISREG` gives `not_regular` (`error` 0, fd closed). The `FileIdentity` is filled from that fstat (`st_dev`, `st_ino`, `st_size`, `st_mtim`, `st_ctim`, assigned from lvalues with no casts).
    - New test-only seam in `output_dir_testing.hpp`: `force_skip_existing_precheck(bool)` skips step 1, so the tests reach step 2's errno mapping and step 3's re-check.
  - **`publish_replacing(temp, name, expected)`:**
    - `sync_and_close` first.
    - O_TMPFILE: link to a fresh `.part-` name with an `UnlinkGuard`. Fallback: the named temporary already exists.
    - Then `fstatat(fd_, name, AT_SYMLINK_NOFOLLOW)` and compare the full identity, ctime included:
      - equal: `renameat` gives `replaced`;
      - `ENOENT`: a no-clobber publish gives `written`, and `EEXIST` there gives `changed`;
      - differs: the temporary is discarded and the result is `changed`.
    - Best-effort dir fsync after `replaced` or `written`.
    - The doc comment and the code state two things:
      - the cpp-security §4 check-to-rename window;
      - **the V5 ABA contract:** the identity comparison is meaningful only while the caller still holds the inspection fd, because ext4 reuses a freed inode number immediately.
    - The V6 overlayfs caveat (untested) is noted.
- **Dependencies:** T1.
- **Done when:** `nice pixi run check` and `nice pixi run -e gcc check` are green.
- **Tests (unit):**
  - **`open_existing`, normal path:**
    - a regular file (identity equals the test's own `stat`, ctime included);
    - absent;
    - a symlink to a file, a dangling symlink, a symlink loop, a FIFO (returns at once; V3 ✅), a directory and an AF_UNIX socket, each `not_regular` via the pre-check;
    - a mode-000 file as `unreadable` (`SKIP` when euid is 0);
    - an invalid component as `Errc::failure`.
  - **With `force_skip_existing_precheck`**, the **exact errnos** are asserted on `ExistingHandle::error`:
    - symlink, dangling symlink, loop: `ELOOP`;
    - AF_UNIX socket: `ENXIO`;
    - directory and FIFO: they open, then the post-open `fstat` gives `not_regular` with `error == 0`, the fd closed, and no hang.
    - A driverless char device needs `mknod` privileges. It is not created in tests, so its `ENXIO` is documented only (fact-check V11).
  - **`publish_replacing`**, each on **both** the O_TMPFILE path and the forced fallback, with no `.part-` file left in any case:
    - replaced (the old fd still reads the old bytes; V5/V6);
    - vanished (`written`);
    - changed by replacing the file;
    - changed by an in-place same-size rewrite whose mtime is restored with `futimens` (caught by ctime, V12);
    - `force_renameat2_unsupported` on the vanished path.
    - The tests keep the inspection fd open across every `publish_replacing` call (V5).

### T7: Update decision (pure matching, re-pick, final list, reconciliation)
- **Goal:** the table-tested heart of the feature: classification, the final chapter list and the report lists, with no I/O.
- **Owns:** `src/core/update_plan.cpp` (new), `tests/unit/update_plan/update_plan_test.cpp` (new), `tests/unit/update_plan/reconcile_test.cpp` (new).
- **Build files for build-fixer:** none.
- **What changes:**
  - **Matching** (planned chapter `p` against held chapters; each held chapter is consumed at most once; held order is the tie-break; the first rule wins):
    1. the same id, ASCII case-insensitive;
    2. the same key `(normalize_label(volume) or none, normalize_label(chapter))`, where an empty label counts as none, and a null or empty chapter label is keyed by id only;
    3. a planned `(v≠none, c)` against a leftover held `(none, c)`.
  - **Re-pick (decision 5):** only for rules 2 and 3, only with a non-empty `preferred_groups`, and only when `group_preference_rank(P.groups) < group_preference_rank(H.groups)`, strictly.
    - P is downloaded and recorded in `repicked`; P counts as missing.
    - **H is consumed by the match, so it is neither extra nor reused.**
    - Equal ranks keep H.
  - **State:** no missing gives `complete`; missing and no extra gives `incomplete`; missing and extra gives `diverged`.
  - **Final list** (plan reading order):
    - matched by id: the planned `Chapter`, `reuse`;
    - matched by number and kept: `chapter_from(H)` with the plan's labels, `reuse`, quality = H's, plus a `KeptRelease`;
    - missing or re-picked: the planned `Chapter`, `download`, quality = the run's.
  - **`fall_back_to_download`:** returns a new decision with that entry switched to the planned release for its key. Any `KeptRelease` for it is removed.
  - **`final_epub`:** `chapters` = the final list, and `planned_pages` = held images + planned pages.
  - **`reconcile_skipped`:** erase H's `duplicate` entry, then append P as `{duplicate, kept: H}` in final-list order.
  - **`new_chapter_ids` / `reused_chapter_ids`; `chapter_labels`** (at most 5, then "and K more").
- **Dependencies:** T1.
- **Done when:** `nice pixi run check` is green.
- **Tests (unit, criteria 1, 2 and decision 5's `[unit]` row):**
  - **Matching table:**
    - same ids;
    - same numbers with different ids: complete, H kept with plan labels and manifest metadata;
    - an added chapter: incomplete, with exact lists;
    - extras only: complete;
    - extras + missing: diverged;
    - a loose chapter moved into a volume;
    - id-keyed null and empty labels;
    - a relabel with the same id;
    - normalisation `"07"` == `"7.0"`;
    - a held duplicate key;
    - case-insensitive ids.
  - **Rank:**
    - better replaces (H not extra);
    - equal, worse, and both unmatched keep;
    - no `--group` keeps;
    - matching by id is never re-picked;
    - group matched by name and by UUID.
  - **Final list:** contributors and `modified` from `final_epub`; mixed qualities preserved; `fall_back_to_download` for a kept H and for a same-id H.
  - **Reconcile:** H present or absent; two substitutions; order; a fallback removes the substitution.
  - Inputs are shuffled; ASan-clean.

### T8: EpubWriter writes the manifest
- **Goal:** every new EPUB carries `META-INF/mangadex-dl.json` as its last STORED entry, consistent with the archive.
- **Owns:** `src/core/epub.cpp`, `tests/unit/epub/epub_manifest_test.cpp` (new).
- **Build files for build-fixer:** none.
- **What changes:**
  - `page_image_entry` and `cover_image_entry` become the single naming source (used internally too).
  - `finish(cover_file_name, chapters)`:
    - validates that the page ranges tile all pages exactly, in order (else `Errc::failure`);
    - runs the existing finish body, keeping the CRC of the exact `content.opf` bytes;
    - fills each `record.images` from the page media types (through `page_image_entry`), and `cover` only when a cover was added **and** a name was given;
    - writes the manifest after `toc.ncx`.
  - The plain `finish()` is unchanged.
- **Dependencies:** T1, T4.
- **Done when:** `nice pixi run check` is green.
- **Tests (unit, criterion 3):**
  - Build into a `MemorySink`, then read it back with `zip_reader`:
    - the manifest is the last entry, method 0, extra 0;
    - `parse_manifest` succeeds;
    - `opf_crc32` equals `reference_crc32(content.opf)` and the CD CRC;
    - the image and cover names match the archive;
    - identifier equals `dc:identifier`.
  - With and without a cover; a cover with no name gives `cover: null`.
  - Two builds are byte-identical.
  - Tiling errors are rejected before anything is written.
  - `epub_test.cpp` stays untouched and green.

### T9: Inspecting existing EPUBs, and held-page reads
- **Goal:** classify one existing file as ours (`HeldBook`) or foreign (with its reason), and read one held page with full validation.
- **Owns:** `src/core/existing_epub.cpp` (new), `tests/support/held_book_builder.hpp` (new), `tests/unit/existing_epub/inspect_test.cpp` (new), `tests/unit/existing_epub/inspect_hostile_test.cpp` (new), `tests/unit/existing_epub/held_page_test.cpp` (new).
- **Build files for build-fixer:** none.
- **What changes:**
  - **`inspect_existing`**, in spec order:
    1. `ZipIndex::read`. A `read_error` gives `unreadable` with the errno; any other fault gives `not_ours`.
    2. Entry 0 must be `mimetype`.
    3. Manifest entry: absent gives `not_ours`. Otherwise it must have method 0, flags 0 and `csize == usize ≤ max_manifest_bytes` (checked **before** allocating). `locate_entry_data` must pass and the CRC must match.
    4. `parse_manifest`: `newer_format` gives `newer_format`; `malformed` gives `not_ours`.
    5. The identifier must match (else `other_book`). `content.opf` must be present (else `not_ours`), and its CD CRC must equal `opf_crc32` (else `changed_by_other_program`).
    6. References:
       - each image and the cover entry is in the CD and referenced exactly once;
       - it has method 0 and flags 0, and `0 < csize == usize ≤ max_image_bytes`;
       - `locate_entry_data` passes;
       - the media type comes from the extension;
       - any failure gives `not_ours`.
    - Nothing read from the file ever appears in a message.
  - **`read_held_page`:**
    - `read_exact` into an exact-size owned vector, the CRC compare, then `sniff_image`.
    - **The type check compares `file_extension(info.type)` (the function the builder uses to name entries) against the held entry name's extension** (V7).
    - CRC, sniff or extension failures give `content`; read faults give `read`.
  - **`foreign_reason_text` and `foreign_remedy`:** fixed strings, with the remedy mapping in Rollout D8.
- **Dependencies:** T1, T4, T5.
- **Done when:** `nice pixi run check` and `nice pixi run -e gcc check` are green.
- **Tests (unit, criteria 4 and 5):**
  - Happy path; `data_offset` equals `zip_reader`'s.
  - **Hostile manifest and reference table:** each row must classify as foreign with the exact reason, never crash, ASan/UBSan-clean.
    - no manifest;
    - manifest deflated, flags ≠ 0, csize ≠ usize;
    - a claimed manifest size of 16 MiB + 1 (no allocation; the fake source counts reads);
    - a manifest CRC mismatch;
    - a manifest whose data overlaps the CD;
    - malformed JSON, a nesting bomb, a 1–2 MiB separator flood (V16);
    - format 2;
    - another identifier;
    - a stale `opf_crc32`;
    - `content.opf` missing;
    - an image name outside the pattern, a duplicate reference (two chapters, and chapter plus cover), an entry missing from the CD;
    - an image with method 8, csize ≠ usize, size 0, or 32 MiB + 1 (claimed);
    - image data overlapping the CD or past the end;
    - a local header mismatch;
    - a reference to `mimetype` or `OEBPS/content.opf`;
    - EIO at each read index (`unreadable`) and a 0 return at each index (`not_ours`).
  - **Held page:** CRC mismatch, non-image bytes, a side of 0 or 65536, and a `.png` entry holding GIF bytes each give `content`; EIO and a 0 return give `read`; V7 sniffs the same `ImageInfo` as the downloaded bytes for all four fixture formats.
  - A fuzz-style loop: 1000 fixed-seed mutations of a valid book.

### T10: Download integration (inspection, pre-pass, rebuild, publish)
- **Goal:** `run_download` skips complete files, rebuilds incomplete ones with page reuse, and leaves diverged and foreign files alone, with the old file intact on every failure and interrupt path.
- **Owns:** `src/core/download/execute_phase.cpp`, `src/core/download/internal.hpp`, `src/core/download/existing.hpp` (new, private), `src/core/download/existing.cpp` (new), `tests/unit/download/download_update_test.cpp` (new), `tests/unit/download/existing_assess_test.cpp` (new), `tests/unit/download/download_test.cpp` (adjust only expectations that legitimately change).
- **Build files for build-fixer:** none.
- **What changes:**
  - **`existing.{hpp,cpp}`** declares `Assessment{ExistingKind kind; ExistingHandle handle; std::optional<HeldBook> held; std::optional<UpdateDecision> decision; PlannedEpub final; std::optional<InspectFailure> foreign; std::vector<std::vector<ImageInfo>> page_info; std::optional<ImageInfo> cover_info; bool reuse_cover;}`.
    - **Invariant (V5 ABA):** for an `incomplete` file in a real run, `Assessment::handle.fd` stays open until `publish_replacing` returns. It is never closed, moved or reset earlier, because the identity comparison is only safe while the inode stays referenced. The `Assessment` is a `produce()` local declared before everything that borrows it.
    - `assess(RunContext&, const OutputDir&, const PlannedEpub&, bool real_run)` does `open_existing`, then `FdReadSource`, then `inspect_existing`, then `decide_update(…)`.
    - `prepass(...)` runs only for a real-run `incomplete`. For each reuse chapter and each page, it calls `read_held_page`, records `ImageInfo` and runs `stop_now` per page.
      - Any page fault switches the chapter to download (`fall_back_to_download`) and warns `<path>: Ch. 3 in the existing file is damaged; downloading it again`.
      - The cover is reused only when the plan's cover `file_name` equals the manifest's and the entry validates.
    - Formats the core-emitted lines: `on_warning` for foreign (with `(<errno_text>)` for `unreadable`), diverged (N/M counts) and damaged; `on_verbose(1)` for kept release, replacing release and `updating <path>: …`.
  - **`produce()`:**
    - `--overwrite`: `exists()` decides `existing` (`unchecked` or none). The publish path is unchanged.
    - Otherwise assess:
      - absent: today's path;
      - `not_regular`, `unreadable` or foreign: `exists` + `foreign` + warning;
      - complete: `exists` + `complete`, `epub` = `final`, `pages` = held images, fd closed;
      - diverged: `exists` + `diverged`, the held chapters listed (D2), warning;
      - incomplete: if `plan_error`, `failed` + `incomplete`. Otherwise: pre-pass, `on_epub_begin(final)`, `create_temp`, `EpubBuild`, then `publish_replacing(identity)` **while the handle's fd is still open**. `replaced` gives written with `replaced:true`; `written` gives written with `replaced:false`; `changed` gives `exists` + `foreign` (`changed_during_update`) + warning. The fd closes after publish.
    - The lists are filled only for `incomplete` (D1). `report.skipped = reconcile_skipped(…)` after each file.
  - **`EpubBuild`:**
    - Takes the final `PlannedEpub`, the `FinalChapter` list, and an optional `{FdReadSource&, HeldBook&, page_info}`.
    - A reuse chapter calls `add_nav_entry`, then per page `read_held_page`. A fault, or an `ImageInfo` different from the pre-pass, is a file failure: "the existing file changed or could not be read during the update" (`Errc::incomplete`). `stop_now` runs per page, and `add_page` and `on_pages` follow.
    - A download chapter goes through `add_chapter` (FetchPool), unchanged.
    - The cover is copied or downloaded.
    - Always `writer_.finish(cover_file_name, book_chapters)`. The `EpubWriter` call sequence is identical to a fresh build.
  - **`dry_run()`:** the same assessment without the pre-pass (structure only; D11). complete gives `exists`; incomplete gives `planned` + `replaced:true` + lists; diverged and foreign give `exists` + warnings. `reconcile_skipped` is applied, and nothing is written or created.
  - **Declaration order** in `produce()`: `Assessment` (owns the fd), then `FdReadSource`, then `TempFile`, then `EpubBuild` (scoped), then publish. On every early return or unwind, the temporary is discarded and then the fd is closed.
- **Dependencies:** batches 1–3 (T1–T9).
- **Done when:** `nice pixi run check`, `nice pixi run -e gcc check` and `nice pixi run tsan -R 'unit\.download'` are green. Existing goldens are untouched.
- **Tests (unit; fixture transport + instant clock on `temp_dir` trees; FakeTransport hooks for injection):**
  - **Basic flows:**
    - clean then clean: all `complete`, no `/at-home/` or `/uploads/` calls;
    - clean then `clean-grown-reuse-only`: Vol 02 `incomplete` and written with `replaced`, new `[ch4]`, reused `[ch3]`, Vol 01 byte-unchanged;
    - a fresh `clean-grown` build is byte-identical to that rebuilt Vol 02 at jobs 1 and 8.
  - **Regroup:**
    - regrouped without `--group`: complete;
    - `--group "Bravo Scans"` on `clean-regrouped-served`: both rebuilt, byte-identical to a fresh download;
    - `--group "Charlie Scans"`: complete;
    - regrouped-grown: Vol 01 is byte-identical to `clean-mixed-reference`, contributors A then B.
  - **Damage and change:**
    - a damaged page: warning, ch3 downloaded, `reused_chapters` empty, byte-identical;
    - the existing file rewritten in place from a hook between pre-pass and copy: `failed`, exit 6, no temporary.
  - **Publish races:** the target unlinked from a hook (`written`, `replaced:false`); the target replaced from a hook (`exists`/`foreign`).
  - **Interrupts:** during the pre-pass, the copy and the download each give `Errc::interrupted`, the old file byte-unchanged and no temporary, on both temp paths.
  - **Foreign:** a FIFO, a symlink and a directory at the target name; `not_ours`, `other_book` and `changed_by_other_program`.
  - **`plan_error`:** complete gives `exists`; incomplete gives `failed`.
  - **Dry runs** write nothing.
  - **Idempotence:** run, update, run.
  - **Layouts:** `chapters` and `single` update cases.
  - **Threads:** observer calls only on the test thread.

### T11: Report JSON, console output and help text (then the bless)
- **Goal:** the user-visible contract.
- **Owns:** `src/core/report.cpp`, `src/app/cmd_download_output.hpp`, `src/app/cmd_download_output.cpp`, `src/app/cmd_download.cpp`, `tests/unit/report/report_test.cpp`, `tests/cli/download_output_test.cpp`, `tests/cli/download_cli_test.cpp` (the re-run note expectation only).
- **Build files for build-fixer:** none.
- **What changes:**
  - **`report_to_json`:** after `replaced` come `existing` (null or the enum spelling), `new_chapters` and `reused_chapters`. `format_version` stays 1.
  - **`ConsoleObserver`:**
    - `finish()` counts `existing == complete` and prints the unprefixed, uncoloured note `mangadex-dl: N files are already up to date; pass --overwrite to download them again` (`1 file is …`), hidden by `-q`. The old "already exist" warning is removed.
    - `-v` for complete: `<path>: up to date (N chapters)`.
    - `-v` for a dry-run `planned` with `existing == incomplete`: `would update <path> (1 new chapter, 2 reused: Ch. 4)`. `would replace` stays for `unchecked`.
    - A failed outcome with `existing == incomplete` gets the `" (the existing file was kept)"` suffix.
    - Everything passes through `terminal_safe`.
  - **`cmd_download.cpp`:** `--overwrite` help becomes "Re-download files even if up to date", and the footer paragraph becomes the spec text verbatim.
- **Dependencies:** T10.
- **Done when:**
  - `nice pixi run check` is green **except** `e2e.download.help` and `e2e.download.json-dry-run`, each failing only with the planned golden diff.
  - The track hands off **bless request:** `pixi run bless '^e2e\.download\.(help|json-dry-run)$'`, reason "new epubs[] keys (existing/new_chapters/reused_chapters); reworded --overwrite help and re-run footer".
  - After the human blesses, `check` is fully green.
- **Tests:**
  - **Unit (`report_test.cpp`):** every `ExistingKind` spelling; key order; empty and non-empty lists; existing exact-document tests updated.
  - **In-process (`download_output_test.cpp`):**
    - note counting (complete only), singular and plural, `-q`, never ANSI;
    - the "up to date" and "would update" lines (with ≤5 labels plus "and K more");
    - the error suffix;
    - `warning:` yellow and plain under `NO_COLOR`.
  - **`download_cli_test.cpp`:** the re-run note text.

### T12: User documentation
- **Owns:** `README.md`, `docs/design/download-spec.md`.
- **What changes:**
  - **README:**
    - the flag table line for `--overwrite`;
    - Output (around lines 145–169): skip/update semantics, the note, `-v` lines, the JSON keys, `exists` = "left alone", `replaced:true` without `--overwrite`, `pages` for `exists`;
    - line 308: replacement happens only for our own incomplete files, atomically, as a new inode, so mode, owner and hard links are not kept;
    - line 316: the `.part-` note also applies to updates;
    - the manifest (in META-INF, which EPUB 3.3 reserves for configuration files, without forbidding extra ones);
    - mixed `data`/`data-saver` chapters in a rebuild;
    - the TOCTOU window, including that a chmod during an update makes the file count as changed (V12);
    - `--title` orphans.
  - **`download-spec.md`:** flag table, JSON, stderr notices, the no-clobber section and acceptance 11 wording, plus a pointer to `skip-existing-spec.md`.
- **Dependencies:** none for content. It runs in batch 4 for review cadence.
- **Done when:** `nice pixi run check` is green.
- **Tests:** none.

### T13a: `[cli]` acceptance: skip, update, reuse, regroup
- **Owns:** `tests/cli/download_update_cli_test.cpp` (new).
- **Tests:**
  - **Criterion 7:** clean twice gives exit 0, empty stdout, stderr exactly the note, and JSON `complete` with `pages` = the held count.
  - **Criterion 8:** reuse proof against `clean-grown-reuse-only`: JSON `incomplete` / `replaced: true` / `new_chapters: [ch4]` / `reused_chapters: [ch3]`, Vol 01 byte-unchanged, stderr empty.
  - **Criterion 9:** byte identity at `-j1` and `-j8`.
  - **Criterion 10:** `clean-regrouped` rebuilds nothing.
  - **Criterion 11:** regrouped-grown gives Vol 01 identical to `clean-mixed-reference`, contributors A then B, `skipped_chapters` reconciled, the `-v` kept lines, Vol 02 complete.
  - **Decision 5:** `--group "Bravo Scans"` rebuilds both volumes, byte-identical to a fresh B download, with the replacing lines and re-picked ids in `new_chapters`; without `--group`, nothing; with `--group "Charlie Scans"`, nothing.
  - **Criterion 15:** dry run on `clean-grown` gives stdout = the Vol 02 path, the `would update` line, JSON `planned` + `replaced` + lists, no file modified.
  - Idempotence; `chapters` and `single` layouts.
- **Dependencies:** T11 (blessed).
- **Done when:** `nice pixi run check` and `nice pixi run tsan -R 'cli\.download'` are green.

### T13b: `[cli]` acceptance: damaged, diverged, foreign, failed, TTY
- **Owns:** `tests/cli/download_existing_cli_test.cpp` (new).
- **Tests:**
  - **Criterion 12:** a damaged held page.
  - **Criterion 13:** diverged.
  - **Criterion 14:** foreign files, each with its exact reason and remedy, then `--overwrite` gives `unchecked`. The cases:
    - no manifest;
    - random bytes;
    - another identifier;
    - a stale OPF CRC;
    - a symlink;
    - a FIFO;
    - a directory;
    - mode 000 (SKIP as root).
  - **Criterion 16:** a failed update gives exit 6, old files unchanged, and the error suffix; `-q` hides every new note and warning; `--json --color=always` has no ESC.
  - **Criterion 20 (in-process share):** progress `[2/2] Vol. 02`, the note uncoloured, `warning:` yellow and plain under `NO_COLOR`.
- **Dependencies:** T11.
- **Done when:** `nice pixi run check` and `nice pixi run tsan -R 'cli\.download'` are green.

### T14: Installed-binary E2E cases and edge additions
- **Owns:** `tests/e2e/cases/download.cmake`, `tests/e2e/cases/_edge.cmake`.
- **Cases:** none are GOLDEN; exact exit codes; streams asserted separately.
  - `download.cmake`:
    - `update-reuse`: PRE_RUN clean; main `clean-grown-reuse-only`. stdout exactly `^out/Clean Title - Vol\. 02\.epub\n$`, stderr empty, EXPECT_EPUB Vol 02, EXPECT_UNCHANGED Vol 01, EXPECT_NO_GLOB `out/.*.part-*`.
    - `update-json`.
    - `rerun-unchanged`, tightened: stderr exactly the note.
    - `up-to-date-json`.
    - `up-to-date-quiet`.
    - `update-dry-run`.
    - `regroup-kept`.
    - `regroup-group-b`.
    - `regroup-group-c`.
    - `update-failed`: exit 6, stdout empty, stderr regex `\(the existing file was kept\)\n`, EXPECT_UNCHANGED both.
    - `diverged-warning`: EXPECT_NO_ANSI.
    - `tty-update-progress`.
    - `tty-up-to-date`: EXPECT_NO_ANSI.
  - `_edge.cmake`:
    - `dev-full-update`: PRE_RUN clean; `clean-grown-reuse-only` with `STDOUT_FILE /dev/full`. Exit 1, `write error`, EXPECT_EPUB Vol 02, EXPECT_UNCHANGED Vol 01, no `.part-`.
    - `pipe-head-update`: PRE_RUN clean; `--group "Bravo Scans"` on `clean-regrouped-served` with `PIPE_HEAD 1 EXPECT_PIPE_RESULT 0` (D7). stdout exactly the Vol 01 line, stderr empty, EXPECT_EPUB both.
      - **Constraint (V18):** this result is deterministic only because stdout stays fully buffered (nothing flushes per path) and the case prints under 4 KiB.
      - The case's comment must say so, and any later change that flushes stdout per path must revisit this case.
    - `stdin-closed-update`: PRE_RUN + `STDIN CLOSED`, exit 0.
- **Dependencies:** T3, T11 (blessed).
- **Done when:** `nice pixi run check` and `nice pixi run release-test` are green.

---

## Parallelization plan

| Batch | Tracks (parallel) | Serialization points held (one track each) |
|---|---|---|
| **Batch 1** | T1, T2, T3 | T1: every shared public header under `include/mangadex_dl/` · T2: `tests/support/mdx_fixture.hpp` · T3: the E2E driver |
| **Batch 2** | T4, T5, T6, T7 | T4: `src/core/json_util.hpp` (core-private, shared with `mangadex_api.cpp`) · T6: `src/core/fs/output_dir_testing.hpp` |
| **Batch 3** | T8, T9 | none |
| **Batch 4** | T10, T12 | none |
| **Batch 5** | T11, then **⏸ bless** `pixi run bless '^e2e\.download\.(help|json-dry-run)$'` | the goldens (human-blessed) |
| **Batch 6** | T13a, T13b, T14 | T14: `_edge.cmake` |

Notes:
- No batch touches `CMakeLists.txt`, `CMakePresets.json`, `pixi.toml`/`pixi.lock`, `src/app/cli.cpp`, `tests/CMakeLists.txt`, `scripts/*` or `.github/*`.
- **Why T11 comes after T10.** The new note counts `existing == complete`, which only T10 produces. Landing T10 first keeps the existing re-run test passing, because the old note counts `status == exists`.
- **One bless round.** T11's JSON keys are final the moment it lands (D1). T10 never changes the report keys.
- **Shared-build hazard.** All unit tests link into one executable. Each batch only references symbols of integrated batches.
- A compile error in another track's file is a coordination event: report it, never fix it.
- Gates: `-e gcc check` for T1, T5, T6, T9 and T10 (the `-Wuseless-cast` rule bites there); `tsan` for T10, T13a and T13b; `release-test` for T14 and `/harden`.

---

## Test plan (per tier)

- **Unit (Catch2, `mangadex_dl::core`):**
  - T4: manifest round trip, the hostile JSON table including the separator-flood and `format` rows, and the separator-bound test. No 16 MiB parse.
  - T5: CD reader and `FdReadSource` (positive short reads continued), the hostile CD table, the fuzz loop, read-fault injection.
  - T6: `open_existing` across every file type, with exact errnos via the pre-check seam; `publish_replacing` on both temp paths, including the ctime-only change.
  - T7: matching, re-pick rank, final list, reconcile.
  - T8: the manifest written by EpubWriter.
  - T9: the inspection hostile table, held-page faults, a fuzz loop, V7.
  - T10: end-to-end update flows: byte identity, fallback, a change during the copy, publish races, interrupts, dry run, layouts.
- **In-process CLI:** T11 `download_output_test.cpp`; T13a and T13b cover criteria 7–16, decision 5's `[cli]` rows, and the in-process share of 20.
- **E2E (T14):** criterion 17, plus the JSON, dry run, regroup, failure and TTY cases. The driver gains `PRE_RUN` and `EXPECT_UNCHANGED` (T3).
- **Edge suite (T14):** `dev-full-update`, `pipe-head-update` (exit 0, D7 + V18 constraint) and `stdin-closed-update`. The bulk SIGPIPE cases still prove policy A.
- **Sanitizer controls:** none new. The existing `heap-overflow`, `dangling-view-heap` and `dangling-view-scope` controls cover the new classes, and `data-race` covers TSan.
- **TSan:** `nice pixi run tsan` must pass whole before the board.
- **Release-test:** in T14 and `/harden`. No change to `release-check.binary` or `staged-files`.
- **Goldens:**
  - **Changed:** `download.help.stdout` and `download.json-dry-run.stdout`. One bless, after T11: `pixi run bless '^e2e\.download\.(help|json-dry-run)$'`.
  - **Unchanged:** `download.dry-run-clean`, `edge.help`, `edge.version`.
  - **Added:** none.
- **Human (User Test Plan inputs):**
  - criterion 21 + V1 (epubcheck and ≥2 readers): `[human: epubcheck not on conda-forge (V9) · retryable]`;
  - criterion 22 (live series, `-vv`, no 429 storm);
  - criterion 23 (real Ctrl-C during the copy and during the download);
  - the real-terminal part of criterion 20;
  - V6 on overlayfs, if the user's DIR lives there: `[human: overlayfs untested locally · retryable]`.

---

## Memory-safety watch-list (for `memory-safety-reviewer`)

**The fd from inspection through build to publish**
- `ExistingHandle::fd` is owned by T10's `Assessment`, a local of `produce()` declared before the `FdReadSource`, the `TempFile` and the `EpubBuild`.
- **ABA invariant (V5):** for an update, the fd stays open until `publish_replacing` returns. An earlier close lets ext4 reuse the inode number and defeats the identity check. Look for any `reset()`, `release()`, move or scope exit before publish.
- `FdReadSource` borrows `const UniqueFd&` (LIFETIMEBOUND) and is never stored in `RunContext` or the report.
- Complete, diverged and foreign assessments close the fd before the next file. In a dry run each file's fd is closed in its own scope.
- On every error and interrupt return the order is: `EpubBuild` destroyed, then the TempFile discarded, then the fd closed.
- `publish_replacing(TempFile&&)` consumes the temporary on every path. The `.part-` `UnlinkGuard` is dismissed only after a successful `renameat`.
- A moved-from `Assessment` or `ExistingHandle` holds -1.
- `open_existing` closes the fd when the post-open `fstat` says non-regular.

**Spans and views over pread buffers**
- `read_exact` writes into caller-owned, exact-size vectors, and the positive-short-count loop advances a `subspan`, never past `out.size()`.
- No span into the CD buffer outlives `ZipIndex::read`.
- `HeldPage::bytes` lives across exactly one `add_page` call.
- `ZipIndex::find` and `entries()` are LIFETIMEBOUND, never called on a temporary, never kept past the index.
- `foreign_reason_text` and `remedy` return static storage.

**Offset math**
- Every u16/u32 field is widened to `std::uint64_t` before summing (≤5 terms).
- Every slice uses `offset > size || len > size - offset`.
- `std::in_range<std::size_t>` runs before every cast to `size_t`.
- No `static_cast` of a call or arithmetic result to its own type (V19).
- Caps are checked before allocating: CD ≤ 8 MiB, manifest ≤ 16 MiB, image ≤ 32 MiB.
- `data + csize ≤ cd_offset` for every referenced entry.

**JSON parse limits**
- Size cap, then the depth and **separator-count** pre-scan (≤ 2^21), then the non-throwing parse, then `FieldReader` only.
- The parsed document is never dumped, copied or compared (V4).
- Peak parse memory is bounded through the separator cap (V16).

**FetchPool interplay**
- Reused chapters and the cover copy run only on the calling thread, between pool runs.
- No pool lambda captures the source, the `HeldBook` or the fd.

**Iterators in the matching and final-list functions**
- These functions work on indices and return new values.
- Inside the pre-pass loop, iterate by index around `decision = fall_back_to_download(std::move(decision), …)`.
- `reconcile_skipped` uses erase-if then append.
- `FinalChapter::held` and `planned` are indices into containers that must stay unmodified while the decision is used.

**Ownership of the inspected handle on interrupt**
- `stop_now` is checked per page. It unwinds through RAII only.
- The old file is opened `O_RDONLY` only.

**Other**
- `errno_text` uses `strerror_r`.
- `FileIdentity` is a plain value.
- No new threads.
- C APIs get `std::string` names.

---

## Rollout & risk

**CLI contract**
- No flags, exit codes or subcommands change. The `--overwrite` help and the footer are reworded (bless).
- The JSON additions are additive within `format_version: 1`.
- **Semantic changes (documented by T12):**
  - `exists` now means "left alone";
  - `replaced: true` can appear without `--overwrite`;
  - **`pages` for `exists` is the held count (it was 0)**;
  - chapters reflect substitutions;
  - `skipped_chapters` is reconciled.
- The stderr note loses its `warning:` prefix (call it out in the PR).
- A chmod or link-count change on a file during an update makes it `changed`, so it is left alone and reported foreign (V12; documented).
- **Migration:** none.

**⏸ Decisions for plan approval**
1. **D1:** `new_chapters` and `reused_chapters` are non-empty only for `existing: "incomplete"`.
2. **D2:** a `diverged` outcome lists the held chapters with `pages` = held images. `foreign` and absent list the planned chapters with `pages` 0.
3. **D3:** `fstatat` before opening, plus the `fstat` re-check after the open (both mandatory per V11). `ELOOP`/`ENXIO`/`ENODEV` map to "not a regular file", not to exit 1.
4. **D4:** u64 widening instead of overflow builtins.
5. **D5:** a rebuild may mix qualities; the manifest spells them `"data"` and `"data-saver"`.
6. **D6:** the reuse-only fixture variants also do not serve cover images.
7. **D7:** `| head -1` across two updates asserts **exit 0**, not 141 (V18). This holds only while stdout stays fully buffered (nothing flushes per path) and the output stays under 4 KiB. Bulk edge cases prove policy A.
8. **D8:** foreign remedy mapping:
   - not regular / cannot be read: "rename or remove it, or use another DIR";
   - not ours / holds a different book: "use another DIR or --title";
   - changed by another program / made by a newer mangadex-dl / changed while it was being updated: "pass --overwrite to replace it".
9. **D9:** a pre-pass read error falls back to download. During the build copy, any fault fails the file.
10. **D10:** the `EXPECT_UNCHANGED` driver keyword.
11. **D11:** the dry-run inspection reads local headers but never page data.
12. **D12 (new, V16):** `max_manifest_separators = 1 << 21`. A pathological file at the zip entry cap with more than about 11 groups per chapter would be classified `foreign`, which is accepted.

**Riskiest task: T10.** De-risked by the frozen interfaces, batches 2–3 proving every component (including T6's races and the ctime and ABA cases), the batch-1 fixtures, the FakeTransport hooks, and the gcc and tsan gates. **Second risk:** hostile bytes in T5 and T9, mitigated by exhaustive truncation, byte flips, fixed-seed fuzzing and the `-O2` release-test.

---

## Refactor triggers

Only V1 (human) and V6 on overlayfs (human) remain open. Every other trigger below is either resolved or already applied in this revision.

| Item | Status / what would change |
|---|---|
| V1 (epubcheck flags the META-INF file) | Open (human). If it does: the design goes back to cli-designer; the manifest moves to an OPF `<meta>` or an OEBPS file; T1, T4, T8, T9, T12. |
| V9 | ✅ Not on conda-forge, so no dependency change. V1 stays human. |
| V10 | ⚠ Wording fixed in T12 and V10 above. No code change. |
| V3 | ✅ No change. |
| V5 | ⚠ The ABA contract went into T6 (doc comment) and T10 (invariant). |
| V6 | ✅ locally; overlayfs is a human item. If it fails: T6 changes to copy-then-rename into a fresh name, and T10 re-reads before publish. |
| V11 | ⚠ Applied: T6 keeps both checks, adds the test seam and asserts exact errnos. |
| V12 | ⚠ Applied: `st_ctim` added to `FileIdentity` (T1), T6 tests, T12 docs. |
| V13 | ⚠ Applied: the T5 `read_exact` loop and fake-source rows. |
| V4, V14, V15, V17, V21 | ✅ No change. V15's extra rows added to T4. V21's vararg note added to the T6 conventions. |
| V16 | ⚠ Adopted: `max_manifest_separators` (T1), the pre-scan bound and test sizing (T4), the T9 flood row. |
| V7 | ✅ Adopted: the T9 extension check uses `file_extension`. |
| V8 | ✅ As planned (T5 `zip_forge`). |
| V18 | ✅ D7 stands, with the buffering constraint in T14. |
| V19 | ✗ Refuted, then reworded. `FileIdentity` keeps native types (simplest). The `-Wuseless-cast` rule is added to the Conventions. |
| V20 | ⚠ Applied: header includes are `<sys/types.h>` + `<ctime>`, and the `.cpp` includes `<sys/stat.h>`. |
| T6 name hiding | Applied: the anonymous `open_existing` is renamed to `open_directory`. |

---

## Relevant paths
- Spec: `/home/user/Mangadex-dl/docs/design/skip-existing-spec.md`
- Prior plan format: `/home/user/Mangadex-dl/docs/design/download-plan.md`, `/home/user/Mangadex-dl/docs/design/download-plan-rev1.md`
- Shared headers (T1): `/home/user/Mangadex-dl/include/mangadex_dl/epub.hpp`, `report.hpp`, `planner.hpp`, `fs/output_dir.hpp` (plus the new `book_manifest.hpp`, `zip_index.hpp`, `existing_epub.hpp`, `update_plan.hpp`, `fs/read_source.hpp`)
- T6 hazard: `/home/user/Mangadex-dl/src/core/fs/output_dir.cpp` (free function at line 384, callers at 596 and 611; `open_at` wrapper at 119)
- Integration: `/home/user/Mangadex-dl/src/core/download/execute_phase.cpp`, `/home/user/Mangadex-dl/src/core/download/internal.hpp`
- Core edits: `/home/user/Mangadex-dl/src/core/epub.cpp`, `/home/user/Mangadex-dl/src/core/report.cpp`, `/home/user/Mangadex-dl/src/core/json_util.hpp`, `/home/user/Mangadex-dl/src/core/planner.cpp`
- App: `/home/user/Mangadex-dl/src/app/cmd_download_output.cpp`, `/home/user/Mangadex-dl/src/app/cmd_download.cpp`
- Test support: `/home/user/Mangadex-dl/tests/support/mdx_fixture.hpp`, `/home/user/Mangadex-dl/tests/support/zip_reader.hpp`
- E2E driver: `/home/user/Mangadex-dl/tests/e2e/CliE2E.cmake`, `/home/user/Mangadex-dl/tests/e2e/run_case.cmake`; cases: `/home/user/Mangadex-dl/tests/e2e/cases/download.cmake`, `/home/user/Mangadex-dl/tests/e2e/cases/_edge.cmake`
- Goldens to re-bless: `/home/user/Mangadex-dl/tests/e2e/golden/download.help.stdout`, `/home/user/Mangadex-dl/tests/e2e/golden/download.json-dry-run.stdout`
- Docs: `/home/user/Mangadex-dl/README.md`, `/home/user/Mangadex-dl/docs/design/download-spec.md`
