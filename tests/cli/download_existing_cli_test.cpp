// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/cli/download_existing_cli_test.cpp: `download` over an output directory whose files the
// run must not simply skip or rebuild (skip-existing plan T13b), end to end through
// run(args, Io&) against hermetic fixture trees (tests/support/mdx_fixture.hpp):
// - criterion 12: a damaged page in a held chapter is downloaded again;
// - criterion 13: a diverged file is left alone with its warning;
// - criterion 14: foreign files (no manifest, random bytes, another language's identifier, a stale
//   OPF CRC, a symlink, a FIFO, a directory, mode 000) are left untouched with their exact reason
//   and remedy, and --overwrite replaces them;
// - criterion 16: a failed update exits 6 and keeps the old file; -q hides every new note and
//   warning; --json --color=always keeps ANSI out of stdout;
// - criterion 20 (the in-process share): update progress on a TTY, the uncoloured note, and a
//   yellow `warning:` that is plain under NO_COLOR.
//
// The "another file took this name while ours was being written" warning needs a file to appear
// between the assessment and the publish of a fresh file. The fixture backend refuses FIFOs, so no
// in-process run can block at that point deterministically; it is covered by
// tests/cli/download_output_test (the observer's exact line) and by the T10 publish-race unit tests
// (the core outcome).
//
// Catch2 test names must not start with '-' (CTest passes the name as a test spec).

#include "app/io.hpp"
#include "support/download_cli_harness.hpp"
#include "support/io_capture.hpp"
#include "support/mdx_fixture.hpp"
#include "support/zip_forge.hpp"
#include "support/zip_reader.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sys/fsuid.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

namespace md = mangadex_dl;
namespace mdx = mangadex_dl::test::mdx;
namespace zip = mangadex_dl::test::zip;
namespace forge = mangadex_dl::test::forge;
using md::test::Captured;
using md::test::Tty;
using md::test::download_cli::concat;
using md::test::download_cli::lines_of;
using md::test::download_cli::list_dir;
using md::test::download_cli::parse_document;
using md::test::download_cli::path_lines;
using md::test::download_cli::read_file;
using md::test::download_cli::run_args;
using md::test::download_cli::snapshot;
using md::test::download_cli::Tree;
using Json = nlohmann::json;

constexpr std::string_view vol1_name = "Clean Title - Vol. 01.epub";
constexpr std::string_view vol2_name = "Clean Title - Vol. 02.epub";
constexpr std::string_view manifest_entry = "META-INF/mangadex-dl.json";
constexpr std::string_view opf_entry = "OEBPS/content.opf";

constexpr std::string_view yellow_warning = "\x1b[33mwarning:\x1b[0m";
constexpr std::string_view note_one =
    "mangadex-dl: 1 file is already up to date; pass --overwrite to download it again\n";
constexpr std::string_view closing_one_of_two =
    "mangadex-dl: error: 1 of 2 files failed; run the same command again to retry them\n";

// Foreign reasons and remedies (src/core/existing_epub.cpp, plan Rollout D8), spelled out here so
// a change to the user-visible text fails this test.
constexpr std::string_view not_regular = "not a regular file";
constexpr std::string_view not_ours = "not an EPUB made by mangadex-dl";
constexpr std::string_view other_book = "holds a different book";
constexpr std::string_view changed_by_other = "changed by another program";
constexpr std::string_view remedy_rename = "rename or remove it, or use another DIR";
constexpr std::string_view remedy_title = "use another DIR or --title";
constexpr std::string_view remedy_overwrite = "pass --overwrite to replace it";

[[nodiscard]] std::string ch_id(std::uint64_t n) { return mdx::clean_chapter_id(n); }

// ---- expected lines --------------------------------------------------------------------------

[[nodiscard]] std::string warning_line(const std::filesystem::path& path,
                                       std::string_view message) {
    return std::format("mangadex-dl: warning: {}: {}\n", path.string(), message);
}

[[nodiscard]] std::string foreign_line(const std::filesystem::path& path, std::string_view reason,
                                       std::string_view remedy) {
    return warning_line(path, std::format("left alone: {}; {}", reason, remedy));
}

/// The diverged warning after "<path>: " for clean -> clean-diverged (ch 4 new, ch 3 gone).
constexpr std::string_view diverged_message =
    "MangaDex lists 1 chapter this file lacks (Ch. 4), but the file holds 1 chapter MangaDex no "
    "longer lists (Ch. 3); left it alone (pass --overwrite to rebuild it without it)";

/// The damage warning after "<path>: " for the clean Vol. 02's only chapter.
constexpr std::string_view damaged_message =
    "Ch. 3 in the existing file is damaged; downloading it again";

// ---- JSON ------------------------------------------------------------------------------------

/// The report document of `r`, REQUIRE'd to be exactly one JSON document.
[[nodiscard]] Json document_of(const Captured& r) {
    Json doc = parse_document(r.out);
    INFO(r.out);
    REQUIRE(!doc.is_discarded());
    return doc;
}

/// The epubs[] entry whose path is `path` (a copy).
[[nodiscard]] Json epub_for(const Json& doc, const std::filesystem::path& path) {
    const Json& epubs = doc.at("epubs");
    const auto found = std::ranges::find_if(
        epubs, [&path](const Json& epub) { return epub.at("path") == path.string(); });
    INFO("no epubs[] entry for " << path.string());
    REQUIRE(found != epubs.end());
    return *found;
}

[[nodiscard]] std::vector<std::string> strings_of(const Json& array) {
    std::vector<std::string> out{};
    for (const Json& item : array) {
        out.push_back(item.get<std::string>());
    }
    return out;
}

[[nodiscard]] std::vector<std::string> chapter_ids(const Json& epub) {
    std::vector<std::string> out{};
    for (const Json& chapter : epub.at("chapters")) {
        out.push_back(chapter.at("id").get<std::string>());
    }
    return out;
}

[[nodiscard]] Json summary(int written, int exists, int failed) {
    return Json{{"planned", 0}, {"written", written}, {"exists", exists}, {"failed", failed}};
}

// ---- files -----------------------------------------------------------------------------------

[[nodiscard]] std::vector<std::string> both_volumes() {
    return {std::string{vol1_name}, std::string{vol2_name}};
}

/// What lstat says about a name, plus its content: the bytes of a regular file, the target of a
/// symlink, the sorted names inside a directory. Equal states mean "not touched".
struct NodeState {
    std::filesystem::file_type type = std::filesystem::file_type::none;
    ino_t inode = 0;
    std::int64_t mtime_sec = 0;
    std::int64_t mtime_nsec = 0;
    std::string content{};

    friend bool operator==(const NodeState&, const NodeState&) = default;
};

[[nodiscard]] NodeState node_state(const std::filesystem::path& path) {
    struct stat st{};
    REQUIRE(::lstat(path.c_str(), &st) == 0);
    NodeState state{
        .type = std::filesystem::symlink_status(path).type(),
        .inode = st.st_ino,
        .mtime_sec = st.st_mtim.tv_sec,
        .mtime_nsec = st.st_mtim.tv_nsec,
        .content = {},
    };
    switch (state.type) {
    case std::filesystem::file_type::regular:
        state.content = read_file(path);
        break;
    case std::filesystem::file_type::symlink:
        state.content = std::filesystem::read_symlink(path).string();
        break;
    case std::filesystem::file_type::directory:
        for (const std::string& name : list_dir(path)) {
            state.content += name;
            state.content += '\n';
        }
        break;
    default:
        break;
    }
    return state;
}

/// What lstat says about a name, without reading it (for a file this process may not be allowed
/// to read). Equal metadata means "not replaced, not rewritten, not chmod'ed".
struct NodeMeta {
    std::filesystem::file_type type = std::filesystem::file_type::none;
    ino_t inode = 0;
    off_t size = 0;
    mode_t mode = 0;
    std::int64_t mtime_sec = 0;
    std::int64_t mtime_nsec = 0;

    friend bool operator==(const NodeMeta&, const NodeMeta&) = default;
};

[[nodiscard]] NodeMeta node_meta(const std::filesystem::path& path) {
    struct stat st{};
    REQUIRE(::lstat(path.c_str(), &st) == 0);
    return NodeMeta{
        .type = std::filesystem::symlink_status(path).type(),
        .inode = st.st_ino,
        .size = st.st_size,
        .mode = st.st_mode,
        .mtime_sec = st.st_mtim.tv_sec,
        .mtime_nsec = st.st_mtim.tv_nsec,
    };
}

/// Writes `text` to `path` (created or truncated).
void write_text(const std::filesystem::path& path, std::string_view text) {
    REQUIRE(forge::write_file(path, zip::bytes_of(text)));
}

/// Rebuilds the archive at `path` with `edit` applied to its (name, data) list.
template <typename Edit> void rewrite_archive(const std::filesystem::path& path, Edit edit) {
    const std::string original = read_file(path);
    const auto rebuilt = forge::rewrite_entries(zip::byte_vector(original), edit);
    INFO((rebuilt ? std::string{} : rebuilt.error()));
    REQUIRE(rebuilt.has_value());
    REQUIRE(forge::write_file(path, *rebuilt));
}

/// Flips one byte in the middle of the first page image of the first chapter that the EPUB at
/// `path` holds, in place (same inode and size). Only that image's CRC breaks; the zip structure
/// and the manifest stay valid, so the file is still ours and the damage is found page by page.
void damage_first_page(const std::filesystem::path& path) {
    const auto archive = zip::read_archive_file(path);
    INFO((archive ? std::string{} : archive.error()));
    REQUIRE(archive.has_value());
    const zip::Entry* manifest = archive->find(manifest_entry);
    REQUIRE(manifest != nullptr);
    const Json record = Json::parse(manifest->text());
    const std::string page = record.at("chapters").at(0).at("images").at(0).get<std::string>();
    const zip::Entry* image = archive->find(page);
    REQUIRE(image != nullptr);
    REQUIRE(image->compressed_size > 2);
    const std::uint64_t at = image->data_offset + (image->compressed_size / 2);
    const std::string bytes = read_file(path);
    REQUIRE(at < bytes.size());
    const auto old_value = static_cast<unsigned char>(bytes[static_cast<std::size_t>(at)]);
    const auto new_value = static_cast<std::byte>(old_value ^ 0x5AU);
    REQUIRE(forge::patch_file_byte(path, at, new_value));
}

/// `count` bytes from a fixed xorshift sequence: deterministic, and not a zip archive.
[[nodiscard]] std::string noise_bytes(std::size_t count) {
    std::string out(count, '\0');
    std::uint32_t state = 0x9E3779B9U;
    for (char& c : out) {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        c = static_cast<char>(state & 0xFFU);
    }
    return out;
}

/// `clean` with every chapter in French: the same title and file names, another identifier
/// (`urn:mangadex:<id>:fr:<unit>`).
[[nodiscard]] mdx::FixtureManga clean_in_french() {
    mdx::FixtureManga manga = mdx::clean_fixture();
    for (mdx::FixtureChapter& chapter : manga.chapters) {
        chapter.lang = "fr";
    }
    return manga;
}

/// `clean-grown` whose new chapter (vol 2 ch 4) cannot be downloaded: an update of a `clean`
/// Vol. 02 fails after the pre-pass, while building the replacement.
[[nodiscard]] mdx::FixtureManga clean_grown_ch4_unserved() {
    mdx::FixtureManga manga = mdx::clean_grown_fixture();
    for (mdx::FixtureChapter& chapter : manga.chapters) {
        if (chapter.id == ch_id(mdx::clean_ch4_n)) {
            chapter.served = false;
        }
    }
    return manga;
}

// ---- process state ---------------------------------------------------------------------------

/// Permission bits do not restrict root (CAP_DAC_OVERRIDE). As root, this switches the calling
/// thread's filesystem uid to nobody for its lifetime, which also drops the filesystem
/// capabilities from its effective set; the destructor switches back to 0. Only filesystem
/// permission checks of this thread change. Does nothing when not root.
class ScopedUnprivilegedFs {
public:
    ScopedUnprivilegedFs() noexcept {
        if (::geteuid() != 0) {
            return;
        }
        static_cast<void>(::setfsuid(nobody_uid));
        // setfsuid reports only the previous value: a second call shows whether the first took.
        switched_ = std::cmp_equal(::setfsuid(nobody_uid), nobody_uid);
        usable_ = switched_;
    }
    ScopedUnprivilegedFs(const ScopedUnprivilegedFs&) = delete;
    ScopedUnprivilegedFs& operator=(const ScopedUnprivilegedFs&) = delete;
    ScopedUnprivilegedFs(ScopedUnprivilegedFs&&) = delete;
    ScopedUnprivilegedFs& operator=(ScopedUnprivilegedFs&&) = delete;
    ~ScopedUnprivilegedFs() {
        if (switched_) {
            static_cast<void>(::setfsuid(0));
        }
    }

    /// Whether permission bits now restrict this thread.
    [[nodiscard]] bool usable() const noexcept { return usable_; }

private:
    static constexpr uid_t nobody_uid = 65534;
    bool switched_ = false;
    bool usable_ = true;
};

// ---- foreign cases ---------------------------------------------------------------------------

enum class Foreign : std::uint8_t {
    no_manifest,
    random_bytes,
    other_language,
    stale_opf_crc,
    symlink,
    fifo,
    directory,
};

[[nodiscard]] std::string_view name_of(Foreign which) noexcept {
    switch (which) {
    case Foreign::no_manifest:
        return "no manifest";
    case Foreign::random_bytes:
        return "random bytes";
    case Foreign::other_language:
        return "another language's identifier";
    case Foreign::stale_opf_crc:
        return "an edited OPF with a stale CRC";
    case Foreign::symlink:
        return "a symlink";
    case Foreign::fifo:
        return "a FIFO";
    case Foreign::directory:
        return "a directory";
    }
    return "?";
}

struct ForeignText {
    std::string_view reason{};
    std::string_view remedy{};
};

[[nodiscard]] ForeignText text_of(Foreign which) noexcept {
    switch (which) {
    case Foreign::no_manifest:
    case Foreign::random_bytes:
        return {.reason = not_ours, .remedy = remedy_title};
    case Foreign::other_language:
        return {.reason = other_book, .remedy = remedy_title};
    case Foreign::stale_opf_crc:
        return {.reason = changed_by_other, .remedy = remedy_overwrite};
    case Foreign::symlink:
    case Foreign::fifo:
    case Foreign::directory:
        return {.reason = not_regular, .remedy = remedy_rename};
    }
    return {};
}

/// Puts `which` at the name of `vol1`, an EPUB the clean title wrote. `scratch` is an empty
/// directory outside the output directory (the symlink's target lives there).
void make_foreign(Foreign which, const std::filesystem::path& vol1,
                  const std::filesystem::path& scratch) {
    switch (which) {
    case Foreign::no_manifest:
        rewrite_archive(vol1, [](std::vector<forge::ForgeEntry>& entries) {
            REQUIRE(std::erase_if(entries, [](const forge::ForgeEntry& entry) {
                        return entry.name == manifest_entry;
                    }) == 1);
        });
        return;
    case Foreign::random_bytes:
        write_text(vol1, noise_bytes(4096));
        return;
    case Foreign::other_language: {
        // The same title in French has the same file names but the identifier
        // urn:mangadex:<id>:fr:v1.
        const Tree french{clean_in_french()};
        const std::filesystem::path out = french.path("out");
        const Captured r = run_args(french.download(out, {"--lang", "fr"}));
        INFO(r.err);
        REQUIRE(r.exit_code == 0);
        REQUIRE(r.out == path_lines(out, {vol1_name, vol2_name}));
        std::filesystem::copy_file(out / vol1_name, vol1,
                                   std::filesystem::copy_options::overwrite_existing);
        return;
    }
    case Foreign::stale_opf_crc:
        rewrite_archive(vol1, [](std::vector<forge::ForgeEntry>& entries) {
            const auto opf = std::ranges::find_if(
                entries, [](const forge::ForgeEntry& entry) { return entry.name == opf_entry; });
            REQUIRE(opf != entries.end());
            const forge::Bytes edit = forge::text_bytes("<!-- edited by hand -->\n");
            opf->data.insert(opf->data.end(), edit.begin(), edit.end());
        });
        return;
    case Foreign::symlink: {
        const std::filesystem::path target = scratch / "real.epub";
        std::filesystem::rename(vol1, target);
        std::filesystem::create_symlink(target, vol1);
        return;
    }
    case Foreign::fifo:
        std::filesystem::remove(vol1);
        REQUIRE(::mkfifo(vol1.c_str(), 0644) == 0);
        return;
    case Foreign::directory:
        std::filesystem::remove(vol1);
        std::filesystem::create_directory(vol1);
        write_text(vol1 / "keep.txt", "not an EPUB");
        return;
    }
}

} // namespace

// CHECK(!x) rather than CHECK_FALSE(x) throughout: clang-analyzer flags Catch2's Flags | Flags cast
// (optin.core.EnumCastOutOfRange) inside CHECK_FALSE / REQUIRE_FALSE.

// ---- criterion 12: a damaged held page ---------------------------------------------------------

TEST_CASE("download existing cli: a damaged held page is downloaded again, byte-identical",
          "[cli][download][existing][damaged]") {
    const Tree clean{mdx::clean_fixture()};
    const Tree grown{mdx::clean_grown_fixture()};

    // The reference: a fresh clean-grown download into an empty directory.
    const std::filesystem::path fresh = grown.path("fresh");
    REQUIRE(run_args(grown.download(fresh)).exit_code == 0);
    const std::string fresh_vol2 = read_file(fresh / vol2_name);

    const bool json = GENERATE(false, true);
    CAPTURE(json);
    const std::filesystem::path out = clean.path("out");
    REQUIRE(run_args(clean.download(out)).exit_code == 0);
    const std::string old_vol1 = read_file(out / vol1_name);
    damage_first_page(out / vol2_name); // ch 3, the chapter Vol. 02 holds

    const std::vector<std::string> args = grown.download(out);
    const Captured r = run_args(json ? concat({"--json"}, args) : args);
    INFO(r.err);
    CHECK(r.exit_code == 0);
    CHECK(r.err == warning_line(out / vol2_name, damaged_message) + std::string{note_one});
    CHECK(list_dir(out) == both_volumes()); // no temporaries left
    CHECK(read_file(out / vol1_name) == old_vol1);
    CHECK(read_file(out / vol2_name) == fresh_vol2);

    if (!json) {
        CHECK(r.out == path_lines(out, {vol2_name}));
        return;
    }
    const Json doc = document_of(r);
    const Json vol2 = epub_for(doc, out / vol2_name);
    CHECK(vol2.at("status") == "written");
    CHECK(vol2.at("replaced") == true);
    CHECK(vol2.at("existing") == "incomplete");
    CHECK(strings_of(vol2.at("new_chapters")) ==
          std::vector<std::string>{ch_id(3), ch_id(mdx::clean_ch4_n)});
    CHECK(vol2.at("reused_chapters").empty());
    CHECK(chapter_ids(vol2) == std::vector<std::string>{ch_id(3), ch_id(mdx::clean_ch4_n)});
    CHECK(vol2.at("pages") == 4);
    CHECK(epub_for(doc, out / vol1_name).at("existing") == "complete");
    CHECK(doc.at("summary") == summary(1, 1, 0));
}

// ---- criterion 13: diverged --------------------------------------------------------------------

TEST_CASE("download existing cli: a diverged file is left alone with its warning",
          "[cli][download][existing][diverged]") {
    const Tree clean{mdx::clean_fixture()};
    const Tree diverged{mdx::clean_diverged_fixture()};
    const std::filesystem::path out = clean.path("out");
    REQUIRE(run_args(clean.download(out)).exit_code == 0);
    const auto before = snapshot(out);
    const NodeState vol2_before = node_state(out / vol2_name);

    const Captured human = run_args(diverged.download(out));
    CHECK(human.exit_code == 0);
    CHECK(human.out.empty());
    CHECK(human.err == warning_line(out / vol2_name, diverged_message) + std::string{note_one});
    CHECK(snapshot(out) == before);
    CHECK(node_state(out / vol2_name) == vol2_before);

    const Captured json = run_args(concat({"--json"}, diverged.download(out)));
    CHECK(json.exit_code == 0);
    CHECK(json.err == human.err);
    const Json doc = document_of(json);
    const Json vol2 = epub_for(doc, out / vol2_name);
    CHECK(vol2.at("status") == "exists");
    CHECK(vol2.at("existing") == "diverged");
    CHECK(vol2.at("replaced") == false);
    CHECK(vol2.at("new_chapters").empty());
    CHECK(vol2.at("reused_chapters").empty());
    CHECK(chapter_ids(vol2) == std::vector<std::string>{ch_id(3)}); // D2: what the file holds
    CHECK(vol2.at("pages") == 2);
    CHECK(epub_for(doc, out / vol1_name).at("existing") == "complete");
    CHECK(doc.at("summary") == summary(0, 2, 0));
    CHECK(snapshot(out) == before);
    CHECK(node_state(out / vol2_name) == vol2_before);

    // The remedy the warning names: --overwrite rebuilds it without the chapter MangaDex dropped.
    const std::filesystem::path fresh = diverged.path("fresh");
    REQUIRE(run_args(diverged.download(fresh)).exit_code == 0);
    const Captured overwritten =
        run_args(concat({"--json"}, diverged.download(out, {"--overwrite"})));
    CHECK(overwritten.exit_code == 0);
    CHECK(overwritten.err.empty());
    const Json replaced = epub_for(document_of(overwritten), out / vol2_name);
    CHECK(replaced.at("status") == "written");
    CHECK(replaced.at("replaced") == true);
    CHECK(replaced.at("existing") == "unchecked");
    CHECK(read_file(out / vol2_name) == read_file(fresh / vol2_name));
}

// ---- criterion 14: foreign files ---------------------------------------------------------------

TEST_CASE("download existing cli: a foreign file is left untouched with its reason and remedy",
          "[cli][download][existing][foreign]") {
    const Foreign which =
        GENERATE(Foreign::no_manifest, Foreign::random_bytes, Foreign::other_language,
                 Foreign::stale_opf_crc, Foreign::symlink, Foreign::fifo, Foreign::directory);
    CAPTURE(name_of(which));
    const ForeignText expected = text_of(which);

    const Tree tree{mdx::clean_fixture()};
    const std::filesystem::path out = tree.path("out");
    const std::filesystem::path scratch = tree.path("elsewhere");
    std::filesystem::create_directory(scratch);
    REQUIRE(run_args(tree.download(out)).exit_code == 0);
    const std::string clean_vol1 = read_file(out / vol1_name);
    const std::string clean_vol2 = read_file(out / vol2_name);

    make_foreign(which, out / vol1_name, scratch);
    const NodeState before = node_state(out / vol1_name);
    const auto scratch_before = snapshot(scratch);
    const std::string warning = foreign_line(out / vol1_name, expected.reason, expected.remedy);

    const Captured human = run_args(tree.download(out));
    INFO(human.err);
    CHECK(human.exit_code == 0);
    CHECK(human.out.empty());
    CHECK(human.err == warning + std::string{note_one}); // Vol. 02 is up to date
    CHECK(node_state(out / vol1_name) == before);
    CHECK(read_file(out / vol2_name) == clean_vol2);
    CHECK(list_dir(out) == both_volumes());
    CHECK(snapshot(scratch) == scratch_before);

    const Captured json = run_args(concat({"--json"}, tree.download(out)));
    CHECK(json.exit_code == 0);
    CHECK(json.err == human.err);
    const Json doc = document_of(json);
    const Json vol1 = epub_for(doc, out / vol1_name);
    CHECK(vol1.at("status") == "exists");
    CHECK(vol1.at("existing") == "foreign");
    CHECK(vol1.at("replaced") == false);
    CHECK(vol1.at("pages") == 0); // D2: the planned chapters, nothing held
    CHECK(chapter_ids(vol1) == std::vector<std::string>{ch_id(1), ch_id(2)});
    CHECK(vol1.at("new_chapters").empty());
    CHECK(vol1.at("reused_chapters").empty());
    CHECK(epub_for(doc, out / vol2_name).at("existing") == "complete");
    CHECK(doc.at("summary") == summary(0, 2, 0));
    CHECK(node_state(out / vol1_name) == before);

    // --overwrite never inspects: the name is replaced by a fresh file (a new inode), whatever
    // was there, and a symlink's target is never written through.
    const Captured overwritten = run_args(concat({"--json"}, tree.download(out, {"--overwrite"})));
    INFO(overwritten.err);
    CHECK(snapshot(scratch) == scratch_before);
    if (which == Foreign::directory) {
        // A file can never replace a directory: that file fails before anything is downloaded
        // for it (exit 6), the directory stays as it was, and the run goes on with Vol. 02.
        const std::string failed_line =
            std::format("mangadex-dl: error: {}: cannot replace a directory; rename or remove "
                        "it, or use another DIR\n",
                        (out / vol1_name).string());
        CHECK(overwritten.exit_code == 6);
        CHECK(overwritten.err == failed_line + "mangadex-dl: error: 1 of 2 files failed; run the "
                                               "same command again to retry them\n");
        const Json doc_over = document_of(overwritten);
        const Json failed = epub_for(doc_over, out / vol1_name);
        CHECK(failed.at("status") == "failed");
        CHECK(failed.at("existing") == "foreign");
        CHECK(failed.at("replaced") == false);
        CHECK(failed.at("error") ==
              "cannot replace a directory; rename or remove it, or use another DIR");
        const Json vol2 = epub_for(doc_over, out / vol2_name);
        CHECK(vol2.at("status") == "written");
        CHECK(vol2.at("existing") == "unchecked");
        CHECK(doc_over.at("summary") == summary(1, 0, 1));
        CHECK(node_state(out / vol1_name) == before);
        CHECK(read_file(out / vol2_name) == clean_vol2);
        CHECK(list_dir(out) == both_volumes());

        // --dry-run says the same, and nothing is touched.
        const Captured dry = run_args(tree.download(out, {"--overwrite", "--dry-run"}));
        CHECK(dry.exit_code == 6);
        CHECK(dry.out == (out / vol2_name).string() + "\n");
        CHECK(dry.err == failed_line + "mangadex-dl: error: 1 of 2 files would fail\n");
        CHECK(node_state(out / vol1_name) == before);
        return;
    }
    const Json replaced = epub_for(document_of(overwritten), out / vol1_name);
    CHECK(replaced.at("existing") == "unchecked");
    CHECK(overwritten.exit_code == 0);
    CHECK(overwritten.err.empty());
    CHECK(replaced.at("status") == "written");
    CHECK(replaced.at("replaced") == true);
    CHECK(std::filesystem::symlink_status(out / vol1_name).type() ==
          std::filesystem::file_type::regular);
    CHECK(read_file(out / vol1_name) == clean_vol1);
    CHECK(list_dir(out) == both_volumes());
}

TEST_CASE("download existing cli: an unreadable file (mode 000) is left alone with its errno",
          "[cli][download][existing][foreign]") {
    const Tree tree{mdx::clean_fixture()};
    const std::filesystem::path out = tree.path("out");
    REQUIRE(run_args(tree.download(out)).exit_code == 0);
    const std::string clean_vol1 = read_file(out / vol1_name);
    std::filesystem::permissions(out / vol1_name, std::filesystem::perms::none);
    if (::geteuid() == 0) {
        // The run below uses nobody's filesystem uid with root's filesystem gid, so the group
        // bits apply to it. The temporary directory is 0700: let it reach the fixture tree
        // (readable inside), and let it pass the run's check that the output directory is
        // writable (nothing is written: Vol. 02 is up to date).
        std::filesystem::permissions(tree.root().parent_path(), std::filesystem::perms::group_exec,
                                     std::filesystem::perm_options::add);
        std::filesystem::permissions(
            out, std::filesystem::perms::group_write | std::filesystem::perms::group_exec,
            std::filesystem::perm_options::add);
    }
    // Metadata only: a mode-000 file cannot be read by a non-root test run.
    const NodeMeta before = node_meta(out / vol1_name);
    REQUIRE(before.type == std::filesystem::file_type::regular);
    REQUIRE((before.mode & 07777U) == 0U);

    Captured r{};
    {
        const ScopedUnprivilegedFs unprivileged{};
        if (!unprivileged.usable()) {
            SKIP("running as root without a usable setfsuid: permission bits do not restrict root");
        }
        r = run_args(tree.download(out));
    }
    INFO(r.err);
    CHECK(r.exit_code == 0);
    CHECK(r.out.empty());
    CHECK(r.err ==
          foreign_line(out / vol1_name, "cannot be read (Permission denied)", remedy_rename) +
              std::string{note_one});
    CHECK(node_meta(out / vol1_name) == before);
    // Readable again for the owner: the bytes were never touched either.
    std::filesystem::permissions(out / vol1_name, std::filesystem::perms::owner_read,
                                 std::filesystem::perm_options::add);
    CHECK(read_file(out / vol1_name) == clean_vol1);

    const Captured overwritten = run_args(concat({"--json"}, tree.download(out, {"--overwrite"})));
    CHECK(overwritten.exit_code == 0);
    CHECK(overwritten.err.empty());
    const Json vol1 = epub_for(document_of(overwritten), out / vol1_name);
    CHECK(vol1.at("status") == "written");
    CHECK(vol1.at("replaced") == true);
    CHECK(vol1.at("existing") == "unchecked");
    CHECK(read_file(out / vol1_name) == clean_vol1);
}

// ---- criterion 16: a failed update, -q, --json --color=always ----------------------------------

TEST_CASE("download existing cli: a failed update exits 6 and keeps the existing file",
          "[cli][download][existing][failed]") {
    const Tree clean{mdx::clean_fixture()};
    const Tree broken{clean_grown_ch4_unserved()};
    const std::filesystem::path out = clean.path("out");
    REQUIRE(run_args(clean.download(out)).exit_code == 0);
    const auto before = snapshot(out);
    const NodeState vol2_before = node_state(out / vol2_name);
    const std::string error_prefix =
        std::format("mangadex-dl: error: {}: ", (out / vol2_name).string());
    constexpr std::string_view kept = " (the existing file was kept)\n";

    const Captured human = run_args(broken.download(out));
    INFO(human.err);
    CHECK(human.exit_code == 6);
    CHECK(human.out.empty());
    const std::vector<std::string> lines = lines_of(human.err);
    REQUIRE(lines.size() == 3);
    CHECK((lines[0] + '\n').starts_with(error_prefix));
    CHECK((lines[0] + '\n').ends_with(kept));
    CHECK(lines[1] + '\n' == note_one);
    CHECK(lines[2] + '\n' == closing_one_of_two);
    CHECK(snapshot(out) == before); // byte-unchanged, and no temporary left
    CHECK(node_state(out / vol2_name) == vol2_before);

    // -q hides the note; errors are never hidden.
    const Captured quiet = run_args(concat({"-q"}, broken.download(out)));
    CHECK(quiet.exit_code == 6);
    CHECK(quiet.out.empty());
    CHECK(quiet.err == lines[0] + '\n' + std::string{closing_one_of_two});
    CHECK(snapshot(out) == before);

    const Captured json = run_args(concat({"--json"}, broken.download(out)));
    CHECK(json.exit_code == 6);
    CHECK(json.err == human.err);
    const Json doc = document_of(json);
    const Json vol2 = epub_for(doc, out / vol2_name);
    CHECK(vol2.at("status") == "failed");
    CHECK(vol2.at("existing") == "incomplete");
    CHECK(vol2.at("bytes") == 0);
    REQUIRE(vol2.at("error").is_string());
    // The suffix is the console's; the report keeps the bare error.
    CHECK(lines[0] == std::format("mangadex-dl: error: {}: {}{}", (out / vol2_name).string(),
                                  vol2.at("error").get<std::string>(),
                                  kept.substr(0, kept.size() - 1)));
    CHECK(strings_of(vol2.at("new_chapters")) == std::vector<std::string>{ch_id(mdx::clean_ch4_n)});
    CHECK(strings_of(vol2.at("reused_chapters")) == std::vector<std::string>{ch_id(3)});
    CHECK(epub_for(doc, out / vol1_name).at("existing") == "complete");
    CHECK(doc.at("summary") == summary(0, 1, 1));
    CHECK(snapshot(out) == before);

    // The red error path with colour forced on terminals: stdout stays plain JSON.
    const Captured colored =
        run_args(concat({"--json", "--color=always"}, broken.download(out)),
                 Tty{.stdin_tty = true, .stdout_tty = true, .stderr_tty = true});
    CHECK(colored.exit_code == 6);
    CHECK(!colored.out.contains('\x1b'));
    CHECK(epub_for(document_of(colored), out / vol2_name).at("status") == "failed");
    // The colour really was on: the update's error line is red, and still ends with the suffix.
    CHECK(colored.err.starts_with(
        std::format("mangadex-dl: \x1b[31merror:\x1b[0m {}: ", (out / vol2_name).string())));
    CHECK(colored.err.contains(kept));
    CHECK(snapshot(out) == before);
}

TEST_CASE("download existing cli: -q hides every new note and warning",
          "[cli][download][existing][quiet]") {
    const Tree clean{mdx::clean_fixture()};
    const Tree grown{mdx::clean_grown_fixture()};
    const Tree diverged{mdx::clean_diverged_fixture()};

    SECTION("up to date") {
        const std::filesystem::path out = clean.path("out");
        REQUIRE(run_args(clean.download(out)).exit_code == 0);
        const Captured r = run_args(concat({"-q"}, clean.download(out)));
        CHECK(r.exit_code == 0);
        CHECK(r.out.empty());
        CHECK(r.err.empty());
    }
    SECTION("diverged") {
        const std::filesystem::path out = clean.path("out");
        REQUIRE(run_args(clean.download(out)).exit_code == 0);
        const Captured r = run_args(concat({"-q", "-v"}, diverged.download(out)));
        CHECK(r.exit_code == 0);
        CHECK(r.out.empty());
        CHECK(r.err.empty());
    }
    SECTION("foreign") {
        const std::filesystem::path out = clean.path("out");
        REQUIRE(run_args(clean.download(out)).exit_code == 0);
        write_text(out / vol1_name, noise_bytes(512));
        const Captured r = run_args(concat({"-q"}, clean.download(out)));
        CHECK(r.exit_code == 0);
        CHECK(r.out.empty());
        CHECK(r.err.empty());
    }
    SECTION("damaged held page, then an update") {
        const std::filesystem::path out = clean.path("out");
        REQUIRE(run_args(clean.download(out)).exit_code == 0);
        damage_first_page(out / vol2_name);
        const Captured r = run_args(concat({"-q", "-v"}, grown.download(out)));
        CHECK(r.exit_code == 0);
        CHECK(r.out == path_lines(out, {vol2_name})); // stdout is data: -q keeps it
        CHECK(r.err.empty());
    }
}

TEST_CASE("download existing cli: --json --color=always keeps ANSI out of stdout",
          "[cli][download][existing][json][color]") {
    const Tree clean{mdx::clean_fixture()};
    const Tree diverged{mdx::clean_diverged_fixture()};
    const std::filesystem::path out = clean.path("out");
    REQUIRE(run_args(clean.download(out)).exit_code == 0);
    write_text(out / vol1_name, noise_bytes(512)); // foreign Vol. 01, diverged Vol. 02

    const Captured r = run_args(concat({"--json", "--color=always"}, diverged.download(out)),
                                Tty{.stdin_tty = true, .stdout_tty = true, .stderr_tty = true});
    CHECK(r.exit_code == 0);
    CHECK(!r.out.contains('\x1b'));
    const Json doc = document_of(r);
    CHECK(epub_for(doc, out / vol1_name).at("existing") == "foreign");
    CHECK(epub_for(doc, out / vol2_name).at("existing") == "diverged");
    // The color really was on: both warnings are yellow on stderr.
    CHECK(r.err == std::format("mangadex-dl: {} {}: left alone: {}; {}\n"
                               "mangadex-dl: {} {}: {}\n",
                               yellow_warning, (out / vol1_name).string(), not_ours, remedy_title,
                               yellow_warning, (out / vol2_name).string(), diverged_message));
}

// ---- criterion 20 (in-process share): TTY progress, the note, warning colors -------------------

TEST_CASE("download existing cli: an update draws progress on a TTY; the note is never colored",
          "[cli][download][existing][tty]") {
    const Tree clean{mdx::clean_fixture()};
    const Tree reuse{mdx::clean_grown_reuse_only_fixture()};
    const std::filesystem::path out = clean.path("out");
    REQUIRE(run_args(clean.download(out)).exit_code == 0);

    const Captured r = run_args(concat({"--color=always"}, reuse.download(out)),
                                Tty{.stdout_tty = false, .stderr_tty = true});
    INFO(r.err);
    CHECK(r.exit_code == 0);
    CHECK(r.out == path_lines(out, {vol2_name}));
    // The progress label is the volume as MangaDex spells it (download_cli_test: "Vol. 1").
    CHECK(r.err.contains("\r[2/2] Vol. 2: 0/4 pages"));
    CHECK(r.err.contains("\r[2/2] Vol. 2: 4/4 pages"));
    CHECK(!r.err.contains("[1/2]")); // the up-to-date Vol. 01 is not built
    CHECK(!r.err.contains('\x1b'));  // nothing warned, and the note is plain even when forced
    CHECK(r.err.ends_with(note_one));
}

TEST_CASE("download existing cli: warning: is yellow on a TTY and plain under NO_COLOR",
          "[cli][download][existing][tty][color]") {
    const Tree clean{mdx::clean_fixture()};
    const Tree diverged{mdx::clean_diverged_fixture()};
    const std::filesystem::path out = clean.path("out");
    REQUIRE(run_args(clean.download(out)).exit_code == 0);
    const std::string message = std::format("{}: {}", (out / vol2_name).string(), diverged_message);

    const Captured colored = run_args(diverged.download(out), Tty{.stderr_tty = true});
    CHECK(colored.exit_code == 0);
    CHECK(colored.err == std::format("mangadex-dl: {} {}\n{}", yellow_warning, message, note_one));

    const Captured plain = run_args(diverged.download(out), Tty{.stderr_tty = true},
                                    md::app::EnvSnapshot{.no_color = true});
    CHECK(plain.exit_code == 0);
    CHECK(plain.err == std::format("mangadex-dl: warning: {}\n{}", message, note_one));
    CHECK(!plain.err.contains('\x1b'));
}
