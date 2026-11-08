// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/cli/follows_parse_test.cpp: `follows`, `login` and `logout` through the real
// parse-then-dispatch path, run(args, Io&) (follows plan T19): the registration in cli.cpp, the
// echo policy of their parse errors, the --title message, global flags after DIR, --status next
// to DIR, the help surface (the root one-liners, logout's inherited globals (V9c), no hidden
// --api-base anywhere) and that download, batch and mdlist keep CLI11's stock texts.
//
// The echo policy (follows plan rev1, gate: "hide every value"): no parse error of these three
// commands repeats a value or a bare word, because any of them may be a password. Every case
// below types the fake password `hunter2` somewhere and asserts the exact exit 2, an empty stdout
// and that `hunter2` appears nowhere on stderr.
//
// The runs that get past parsing read a generated follows tree (support/mdx_follows_fixture.hpp)
// through the hidden `--api-base file://<tree>` seam in a fresh temporary directory, with HOME
// and the XDG directories inside it, so nothing reaches the network and every output directory
// and saved login is inside the test's own directory. Every credential is a fixture fake.
//
// Catch2 test names must not start with '-'. Every name starts with "follows parse:" so
// `pixi run test -R 'cli\.follows parse'` selects them all.

#include "app/cli.hpp"
#include "app/io.hpp"
#include "support/fs_test_util.hpp"
#include "support/io_capture.hpp"
#include "support/mdx_follows_fixture.hpp"
#include "support/temp_dir.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <format>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace md = mangadex_dl;
namespace app = mangadex_dl::app;
namespace mdx = mangadex_dl::test::mdx;
namespace fs = std::filesystem;
using Catch::Matchers::ContainsSubstring;
using md::test::Captured;
using Json = nlohmann::json;

/// The value every echo-policy case types where a password could be.
constexpr std::string_view password = "hunter2";

[[nodiscard]] std::string hint(std::string_view command) {
    return std::format("Run 'mangadex-dl {} --help' for more information.\n", command);
}

/// One `mangadex-dl: error: <message>` line plus the command's hint.
[[nodiscard]] std::string usage(std::string_view command, std::string_view message) {
    return std::format("mangadex-dl: error: {}\n{}", message, hint(command));
}

/// The fixed text for a secret-shaped extra.
[[nodiscard]] std::string secret_text(std::string_view command) {
    return usage(command, std::format("{} has no --password option: passwords and secrets never go "
                                      "on the command line (other users can see it); use the "
                                      "prompt, --password-stdin or the credentials file",
                                      command));
}

/// run(args, Io&) over `words` (argv[0] supplied), like support/io_capture.hpp's run_cli, for an
/// argument list built at run time. stdin is closed (-1).
[[nodiscard]] Captured run_words(const std::vector<std::string>& words,
                                 const app::EnvSnapshot& env = {}) {
    std::vector<const char*> argv{"mangadex-dl"};
    argv.reserve(words.size() + 1);
    for (const std::string& word : words) {
        argv.push_back(word.c_str());
    }
    std::ostringstream out;
    std::ostringstream err;
    app::Io io{.out = out, .err = err, .env = env, .stdin_fd = -1};
    Captured result;
    result.exit_code = app::run(std::span<const char* const>{argv}, io);
    result.out = out.str();
    result.err = err.str();
    return result;
}

/// A generated follows tree under a fresh TempDir, with HOME and the XDG directories next to it.
class Tree {
public:
    explicit Tree(const mdx::FollowsFixture& fixture = mdx::follows_clean_fixture())
        : tmp_(md::test::fs_util::make_temp_dir("mangadex-dl-follows-parse")) {
        const auto written = mdx::write_follows_tree(tree(), fixture);
        INFO((written ? std::string{} : written.error()));
        REQUIRE(written.has_value());
    }

    [[nodiscard]] fs::path root() const { return tmp_.path(); }
    [[nodiscard]] fs::path tree() const { return root() / "tree"; }
    [[nodiscard]] std::string out() const { return (root() / "out").string(); }
    [[nodiscard]] std::string api_base() const { return "file://" + tree().string(); }
    [[nodiscard]] std::string credentials() const {
        return (tree() / "creds" / "credentials").string();
    }
    [[nodiscard]] app::EnvSnapshot env() const {
        return app::EnvSnapshot{.home = (root() / "home").string(),
                                .xdg_config_home = (root() / "config").string(),
                                .xdg_state_home = (root() / "state").string()};
    }

private:
    md::test::TempDir tmp_; ///< set by the constructor
};

struct Case {
    std::vector<std::string> args{};
    std::string err{}; ///< the exact stderr
};

/// Each case: exit 2, empty stdout, exactly `err`, and the password nowhere on stderr.
void check_cases(const std::vector<Case>& cases) {
    for (const Case& c : cases) {
        std::string joined{};
        for (const std::string& word : c.args) {
            joined += " [" + word + "]";
        }
        INFO("args:" << joined);
        const Captured r = run_words(c.args);
        CHECK(r.exit_code == 2);
        CHECK(r.out.empty());
        CHECK(r.err == c.err);
        CHECK_THAT(r.err, !ContainsSubstring(std::string{password}));
    }
}

/// Exactly one JSON document (plus the final newline), parsed; a discarded value otherwise.
[[nodiscard]] Json parse_document(const std::string& text) {
    if (!text.ends_with('\n') || !Json::accept(text)) {
        return Json::value_t::discarded;
    }
    return Json::parse(text);
}

} // namespace

// ---- the echo policy ---------------------------------------------------------------------------

TEST_CASE("follows parse: secret-shaped extras get the fixed password text, nothing else",
          "[cli][follows][parse]") {
    const std::string pw{password};
    check_cases({
        {.args = {"follows", "--password", pw}, .err = secret_text("follows")},
        {.args = {"follows", "--password=" + pw}, .err = secret_text("follows")},
        {.args = {"follows", "-p", pw}, .err = secret_text("follows")},
        {.args = {"follows", "-p" + pw}, .err = secret_text("follows")},
        // CLI11 takes the -q and leaves `-phunter2`.
        {.args = {"follows", "-qp" + pw}, .err = secret_text("follows")},
        {.args = {"follows", "out", "--client-secret=" + pw}, .err = secret_text("follows")},
        {.args = {"follows", "out", "--client-secret", pw}, .err = secret_text("follows")},
        {.args = {"follows", "out", "--client-secret-x"}, .err = secret_text("follows")},
        // The password text wins over --title and over every other extra.
        {.args = {"follows", "out", "--title", "x", "--password", pw},
         .err = secret_text("follows")},
        {.args = {"follows", "out", "--unknown", "--password=" + pw},
         .err = secret_text("follows")},
        {.args = {"login", "--password=" + pw}, .err = secret_text("login")},
        {.args = {"login", "-p", pw}, .err = secret_text("login")},
        {.args = {"logout", "-p" + pw}, .err = secret_text("logout")},
        {.args = {"logout", "--password", pw}, .err = secret_text("logout")},
        // Other spellings, in any ASCII case (security L1).
        {.args = {"follows", "out", "--pass=" + pw}, .err = secret_text("follows")},
        {.args = {"follows", "out", "--passwd", pw}, .err = secret_text("follows")},
        {.args = {"follows", "out", "--PASSWORD=" + pw}, .err = secret_text("follows")},
        {.args = {"follows", "out", "--Password", pw}, .err = secret_text("follows")},
        {.args = {"follows", "out", "--pw=" + pw}, .err = secret_text("follows")},
        {.args = {"follows", "out", "--PW", pw}, .err = secret_text("follows")},
        {.args = {"follows", "out", "--secret=" + pw}, .err = secret_text("follows")},
        {.args = {"follows", "out", "--Client-Secret=" + pw}, .err = secret_text("follows")},
        {.args = {"follows", "out", "--client_secret=" + pw}, .err = secret_text("follows")},
        {.args = {"login", "--CLIENT_SECRET", pw}, .err = secret_text("login")},
        {.args = {"logout", "--pass=" + pw}, .err = secret_text("logout")},
        // Before the subcommand: CLI11 hands the unknown words on to it (security L4).
        {.args = {"--password", pw, "follows"}, .err = secret_text("follows")},
        {.args = {"-qp" + pw, "login"}, .err = secret_text("login")},
        // A cluster before the subcommand: CLI11 takes -q and -v and leaves `-phunter2`.
        {.args = {"-qvp" + pw, "follows"}, .err = secret_text("follows")},
    });
}

TEST_CASE("follows parse: extra words are counted, never echoed", "[cli][follows][parse]") {
    const std::string pw{password};
    const std::string one = "does not take 1 more argument (not shown in case it holds a password)";
    const std::string two =
        "does not take 2 more arguments (not shown in case they hold a password)";
    check_cases({
        {.args = {"follows", "out", pw}, .err = usage("follows", "follows " + one)},
        {.args = {"follows", "out", pw, "x"}, .err = usage("follows", "follows " + two)},
        {.args = {"follows", "out", "", pw}, .err = usage("follows", "follows " + two)},
        {.args = {"follows", "out", "--", pw}, .err = usage("follows", "follows " + one)},
        // A single-dash word longer than 2 characters may be a short option glued to a value.
        {.args = {"follows", "out", "-x" + pw}, .err = usage("follows", "follows " + one)},
        {.args = {"follows", "out", "-xyz", pw}, .err = usage("follows", "follows " + two)},
        {.args = {"login", pw}, .err = usage("login", "login " + one)},
        {.args = {"login", "-"}, .err = usage("login", "login " + one)},
        {.args = {"logout", pw}, .err = usage("logout", "logout " + one)},
        {.args = {"logout", pw, pw}, .err = usage("logout", "logout " + two)},
        {.args = {"login", "--", pw}, .err = usage("login", "login " + one)},
        {.args = {"login", ""}, .err = usage("login", "login " + one)},
        {.args = {"logout", ""}, .err = usage("logout", "logout " + one)},
    });
}

TEST_CASE("follows parse: option-looking extras are echoed cut at '=', the rest counted",
          "[cli][follows][parse]") {
    const std::string pw{password};
    const std::string these = "does not take these arguments: ";
    const std::string long_name = "--" + std::string(100, 'a');
    // U+202E RIGHT-TO-LEFT OVERRIDE, built from its bytes (a literal would trip the bidi check).
    const std::string rlo{static_cast<char>(0xE2), static_cast<char>(0x80),
                          static_cast<char>(0xAE)};
    check_cases({
        {.args = {"follows", "out", "--unknown-opt=" + pw},
         .err = usage("follows", "follows " + these + "--unknown-opt")},
        {.args = {"follows", "out", "--unknown-opt=" + pw, pw},
         .err = usage("follows", "follows " + these +
                                     "--unknown-opt (and 1 other word, not shown in case it "
                                     "holds a password)")},
        {.args = {"follows", "out", "--a=" + pw, "-x", pw, "y"},
         .err = usage("follows", "follows " + these +
                                     "--a, -x (and 2 other words, not shown in case they hold "
                                     "a password)")},
        // Through terminal_safe, and cut to 64 bytes.
        {.args = {"follows", "out", "--a\x1b[31mb=" + pw},
         .err = usage("follows", "follows " + these + "--a?[31mb")},
        {.args = {"follows", "out", long_name + "=" + pw},
         .err = usage("follows", "follows " + these + long_name.substr(0, 64))},
        // The 64-byte cut backs off to a UTF-8 boundary: a 2-byte é at bytes 63-64 and a 4-byte
        // emoji at bytes 63-66 are dropped whole; an é at 63-64 followed by more is kept.
        {.args = {"follows", "out", "--" + std::string(61, 'a') + "\xC3\xA9=" + pw},
         .err = usage("follows", "follows " + these + "--" + std::string(61, 'a'))},
        {.args = {"follows", "out", "--" + std::string(60, 'a') + "\xF0\x9F\x98\x80=" + pw},
         .err = usage("follows", "follows " + these + "--" + std::string(60, 'a'))},
        {.args = {"follows", "out", "--" + std::string(60, 'a') + "\xC3\xA9" + "b=" + pw},
         .err = usage("follows", "follows " + these + "--" + std::string(60, 'a') + "\xC3\xA9")},
        // Invalid UTF-8, a bidi override (U+202E) and a newline become '?'.
        {.args = {"follows", "out", "--a\xFF=" + pw},
         .err = usage("follows", "follows " + these + "--a?")},
        {.args = {"follows", "out", "--a" + rlo + "b=" + pw},
         .err = usage("follows", "follows " + these + "--a?b")},
        {.args = {"follows", "out", "--a\nb=" + pw},
         .err = usage("follows", "follows " + these + "--a?b")},
        {.args = {"login", "--status", "reading"},
         .err = usage("login", "login " + these +
                                   "--status (and 1 other word, not shown in case it holds a "
                                   "password)")},
        {.args = {"logout", "--credentials", pw},
         .err = usage("logout", "logout " + these +
                                    "--credentials (and 1 other word, not shown in case it holds "
                                    "a password)")},
        {.args = {"logout", "--no-input"},
         .err = usage("logout", "logout " + these + "--no-input")},
    });
}

TEST_CASE("follows parse: a flag given =value is 'takes no value', without the value",
          "[cli][follows][parse]") {
    const std::string pw{password};
    check_cases({
        {.args = {"follows", "out", "--no-input=" + pw},
         .err = usage("follows", "--no-input takes no value")},
        {.args = {"follows", "out", "--password-stdin=" + pw},
         .err = usage("follows", "--password-stdin takes no value")},
        {.args = {"follows", "out", "--dry-run=" + pw},
         .err = usage("follows", "--dry-run takes no value")},
        {.args = {"follows", "out", "--fail-fast=" + pw},
         .err = usage("follows", "--fail-fast takes no value")},
        // Global flags after the subcommand: found on the root.
        {.args = {"follows", "out", "--json=" + pw},
         .err = usage("follows", "--json takes no value")},
        {.args = {"follows", "out", "--verbose=" + pw},
         .err = usage("follows", "--verbose takes no value")},
        {.args = {"login", "--no-input=" + pw}, .err = usage("login", "--no-input takes no value")},
        {.args = {"login", "--quiet=" + pw}, .err = usage("login", "--quiet takes no value")},
        {.args = {"logout", "--json=" + pw}, .err = usage("logout", "--json takes no value")},
        // A global flag before the subcommand: CLI11 converts it once the subcommand is known.
        {.args = {"--json=" + pw, "follows"}, .err = usage("follows", "--json takes no value")},
    });
}

TEST_CASE("follows parse: other parse errors keep CLI11's text (non-secret options)",
          "[cli][follows][parse]") {
    check_cases({
        {.args = {"follows", "out", "--jobs", "9"},
         .err = usage("follows", "--jobs: Value 9 not in range [1 - 8]")},
        {.args = {"follows", "out", "--layout", "zz"},
         .err = usage("follows", "--layout: zz not in {volumes,chapters,single}")},
        {.args = {"follows", "out", "--status"},
         .err = usage("follows", "--status: 1 required WORD,... missing")},
        {.args = {"login", "--credentials"},
         .err = usage("login", "--credentials: 1 required FILE missing")},
    });
}

TEST_CASE("follows parse: a password typed after --password-stdin becomes DIR and is never "
          "echoed",
          "[cli][follows][parse]") {
    // stdin is closed: the --password-stdin check fails before DIR is ever looked at.
    const Captured r = run_words({"follows", "--password-stdin", std::string{password}});
    CHECK(r.exit_code == 2);
    CHECK(r.out.empty());
    CHECK(r.err == usage("follows", "--password-stdin was given, but stdin held no password"));
    CHECK_THAT(r.err, !ContainsSubstring(std::string{password}));
}

TEST_CASE("follows parse: --title says follows has none", "[cli][follows][parse]") {
    const std::string text =
        usage("follows", "follows has no --title option; to name a title yourself, download it "
                         "on its own with 'mangadex-dl download <title> --title ...'");
    check_cases({
        {.args = {"follows", "out", "--title", "X"}, .err = text},
        {.args = {"follows", "out", "--title=X"}, .err = text},
        {.args = {"follows", "--title", "X"}, .err = text},
    });
}

TEST_CASE("follows parse: download, batch and mdlist keep CLI11's stock texts",
          "[cli][follows][parse]") {
    // The echo policy is only for follows, login and logout: these stay byte-identical.
    const std::vector<Case> cases{
        {.args = {"mdlist", "x", "y", "z"},
         .err = usage("mdlist", "The following argument was not expected: z")},
        {.args = {"batch", "f.csv", "--dry-run=x"},
         .err = usage("batch", "Could not convert: --dry-run = x")},
        {.args = {"download", "x", "--password", "y"},
         .err = usage("download", "The following argument was not expected: --password")},
    };
    for (const Case& c : cases) {
        const Captured r = run_words(c.args);
        CHECK(r.exit_code == 2);
        CHECK(r.out.empty());
        CHECK(r.err == c.err);
    }
}

// ---- argv shapes that parse ---------------------------------------------------------------------

TEST_CASE("follows parse: global flags after DIR apply", "[cli][follows][parse]") {
    const Tree t{};
    const Captured r = run_words({"follows", t.out(), "--dry-run", "--json", "-q", "--credentials",
                                  t.credentials(), "--api-base", t.api_base()},
                                 t.env());
    CHECK(r.exit_code == 0);
    CHECK(r.err.empty());
    const Json doc = parse_document(r.out);
    REQUIRE(!doc.is_discarded());
    CHECK(doc.at("dry_run") == true);
    CHECK(doc.at("source") == "follows");
    REQUIRE(doc.at("titles").size() == 1);
    CHECK(doc.at("titles").at(0).at("dir") == t.out());
    CHECK(!fs::exists(t.out()));
}

TEST_CASE("follows parse: --status takes one value per occurrence and never swallows DIR",
          "[cli][follows][parse]") {
    const Tree t{mdx::follows_status_fixture()};
    const Captured r = run_words({"follows", "--status", "reading", t.out(), "--status",
                                  "completed,dropped", "--dry-run", "--json", "--credentials",
                                  t.credentials(), "--api-base", t.api_base()},
                                 t.env());
    CHECK(r.exit_code == 4); // 6666… (completed) is not on the fixture
    const Json doc = parse_document(r.out);
    REQUIRE(!doc.is_discarded());
    CHECK(doc.at("follows").at("statuses") == Json::array({"reading", "completed", "dropped"}));
    CHECK(doc.at("follows").at("selected") == 2);
    REQUIRE(doc.at("titles").size() == 2);
    CHECK(doc.at("titles").at(0).at("dir") == t.out());
    CHECK(!fs::exists(t.out()));
}

// ---- help --------------------------------------------------------------------------------------

TEST_CASE("follows parse: the root help lists the three new commands", "[cli][follows][help]") {
    const Captured r = run_words({"--help"});
    CHECK(r.exit_code == 0);
    CHECK(r.err.empty());
    const std::string pad(36 - 2, ' ');
    CHECK_THAT(r.out,
               ContainsSubstring("\n  follows                           Download every title "
                                 "you follow on MangaDex\n" +
                                 pad + "  (needs a login)\n"));
    CHECK_THAT(r.out,
               ContainsSubstring("\n  login                             Log in to MangaDex and "
                                 "save the login for\n" +
                                 pad + "  follows\n"));
    CHECK_THAT(r.out, ContainsSubstring("\n  logout                            Remove the saved "
                                        "MangaDex login\n"));
}

TEST_CASE("follows parse: no help shows the hidden --api-base", "[cli][follows][help]") {
    for (const std::vector<std::string>& args : std::vector<std::vector<std::string>>{
             {"--help"},
             {"download", "--help"},
             {"batch", "--help"},
             {"mdlist", "--help"},
             {"follows", "--help"},
             {"login", "--help"},
             {"logout", "--help"},
         }) {
        INFO("args: " << args.front() << (args.size() > 1 ? " " + args[1] : std::string{}));
        const Captured r = run_words(args);
        CHECK(r.exit_code == 0);
        CHECK(r.err.empty());
        CHECK_THAT(r.out, ContainsSubstring("\nExit status:\n"));
        CHECK_THAT(r.out, !ContainsSubstring("--api-base"));
        CHECK_THAT(r.out, !ContainsSubstring("\x1b"));
    }
}

TEST_CASE("follows parse: logout --help lists the inherited global flags (V9c)",
          "[cli][follows][help]") {
    const Captured r = run_words({"logout", "--help"});
    CHECK(r.exit_code == 0);
    CHECK(r.err.empty());
    CHECK_THAT(r.out, ContainsSubstring("\nmangadex-dl logout [OPTIONS]\n"));
    CHECK_THAT(r.out,
               ContainsSubstring("\nOPTIONS:\n"
                                 "  -h, --help                        Print this help message and "
                                 "exit\n"
                                 "      --version                     Print the version and exit\n"
                                 "      --json                        Write machine-readable JSON "
                                 "to stdout\n"
                                 "  -q, --quiet                       Only print errors\n"
                                 "  -v, --verbose                     Print more detail to stderr "
                                 "(repeatable)\n"
                                 "      --color WORD [auto]           Color stderr: auto, always "
                                 "or never\n\n"));
    CHECK_THAT(r.out, !ContainsSubstring("--credentials"));
    CHECK_THAT(r.out, !ContainsSubstring("--password-stdin"));
    CHECK_THAT(r.out, !ContainsSubstring("POSITIONALS"));
}

TEST_CASE("follows parse: follows and login --help show their options", "[cli][follows][help]") {
    SECTION("follows") {
        const Captured r = run_words({"follows", "--help"});
        CHECK(r.exit_code == 0);
        CHECK_THAT(r.out, ContainsSubstring("\nmangadex-dl follows [OPTIONS] [DIR]\n"));
        CHECK_THAT(r.out, ContainsSubstring("\n      --status WORD,...             Only follows "
                                            "with these reading statuses\n"));
        CHECK_THAT(r.out, ContainsSubstring("\n      --credentials FILE            Read login "
                                            "details from FILE\n"));
        CHECK_THAT(r.out, !ContainsSubstring("--title"));
        CHECK_THAT(r.out, !ContainsSubstring("--password "));
    }
    SECTION("login") {
        const Captured r = run_words({"login", "--help"});
        CHECK(r.exit_code == 0);
        CHECK_THAT(r.out, ContainsSubstring("\nmangadex-dl login [OPTIONS]\n"));
        CHECK_THAT(r.out, ContainsSubstring("\n      --password-stdin              Read the "
                                            "password from stdin\n"));
        CHECK_THAT(r.out, !ContainsSubstring("--status"));
        CHECK_THAT(r.out, !ContainsSubstring("POSITIONALS"));
    }
}

TEST_CASE("follows parse: without a command, an unknown option shows only its name (security L2)",
          "[cli][follows][parse]") {
    const std::string pw{password};
    const std::string root_hint = "Run 'mangadex-dl --help' for more information.\n";
    const std::string secret =
        "mangadex-dl: error: there is no --password option: passwords and secrets never go on the "
        "command line (other users can see it); use the prompt, --password-stdin or the "
        "credentials file\n" +
        root_hint;
    const auto usage_root = [&root_hint](std::string_view message) {
        return std::format("mangadex-dl: error: {}\n{}", message, root_hint);
    };
    const auto unknown = [&root_hint](std::string_view name) {
        return std::format("mangadex-dl: error: unknown option '{}'\n{}", name, root_hint);
    };
    check_cases({
        {.args = {"--password=" + pw}, .err = secret},
        {.args = {"--password", pw}, .err = secret},
        {.args = {"-p" + pw}, .err = secret},
        {.args = {"-qp" + pw}, .err = secret},
        {.args = {"--PASS=" + pw}, .err = secret},
        {.args = {"--client_secret=" + pw}, .err = secret},
        // CLI11 takes the -q of `-q=hunter2` and leaves `-=hunter2`.
        {.args = {"-q=" + pw}, .err = unknown("-")},
        {.args = {"--x=" + pw}, .err = unknown("--x")},
        // Before a command, the command's own echo policy applies (CLI11 hands it the word).
        {.args = {"--x=" + pw, "follows"},
         .err = usage("follows", "follows does not take these arguments: --x")},
        {.args = {"--a\x1b[31m=" + pw}, .err = unknown("--a?[31m")},
        {.args = {"--PASSWORD=" + pw}, .err = secret},
        // A single-dash word longer than 2 characters shows only `-x`: it may be a short option
        // glued to its value (security residual 1).
        {.args = {"-x" + pw}, .err = unknown("-x")},
        {.args = {"-P" + pw}, .err = unknown("-P")},
        {.args = {"-qx" + pw}, .err = unknown("-x")},
        {.args = {"-vP" + pw}, .err = unknown("-P")},
        {.args = {"--color=never", "-P" + pw}, .err = unknown("-P")},
        // A global flag given =value: the name only, also once a command was reached.
        {.args = {"--version=" + pw}, .err = usage_root("--version takes no value")},
        {.args = {"--verbose", "--version=" + pw}, .err = usage_root("--version takes no value")},
        {.args = {"--version=" + pw, "download"},
         .err = usage("download", "--version takes no value")},
    });
    // A bare word is still named: it is a mistyped command, not an option value.
    const Captured r = run_words({"folows"});
    CHECK(r.exit_code == 2);
    CHECK(r.err.starts_with("mangadex-dl: error: unknown command 'folows' "));
}

TEST_CASE("follows parse: an unknown command lists the six commands", "[cli][follows][parse]") {
    const Captured r = run_words({"folows"});
    CHECK(r.exit_code == 2);
    CHECK(r.out.empty());
    CHECK(r.err == "mangadex-dl: error: unknown command 'folows' (commands: download, batch, "
                   "mdlist, follows, login, logout)\n"
                   "Run 'mangadex-dl --help' for more information.\n");
}
