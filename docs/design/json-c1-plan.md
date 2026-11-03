# Plan: escape DEL and the C1 controls in every `--json` document (json-c1)

Status: fact-checked (clusters A, B, C: every claim CONFIRMED, nothing refuted). Base: 577584b.

## Problem
`--json` documents are dumped with `dump(2, ' ', /*ensure_ascii=*/false, error_handler_t::replace)`.
nlohmann 3.12.0 escapes only `"`, `\` and U+0000–U+001F with that call (`serializer.hpp:464`:
`codepoint <= 0x1F || (ensure_ascii && codepoint >= 0x7F)`), so a raw DEL (`7F`) or C1 control
(`C2 80`–`C2 9F`, e.g. U+009B, the 8-bit CSI) reaches stdout unescaped. Affected: every string in
`download --json` (`report_to_json`), `batch --json` and `mdlist --json` (`batch_to_json`, which
embeds each title's report). Reproduced on the installed binary with DIR = `$'out\x7f\xc2\x9bx'`.

## Approach
Post-process the dumped text in one core-private helper:

```
ordered_json ──dump(2,' ',false,replace)──▶ valid UTF-8 text ──escape_del_and_c1──▶ + '\n' ──▶ stdout
                (C0 → \u00xx, bad UTF-8 → EF BF BD)      (7F → \u007f, C2 80..9F → \u0080..\u009f)
```

Sound because (fact-check A, V2c/V5, 26.6M-input probe, 0 failures): after `replace` the output is
always valid UTF-8; everything outside strings is ASCII; `C2` is never a continuation byte, so a
`C2 80..9F` pair is exactly U+0080..U+009F and a raw `7F` is exactly DEL, both inside a string (key
or value). `replace` never synthesizes those bytes from invalid input. Escapes use **lowercase**
hex, matching nlohmann's `"\\u%04x"` (`serializer.hpp:469`). A strict parse of `\u007f` /
`\u0080`..`\u009f` yields the original bytes, so the decoded strings are identical (V6).

Rejected: pre-escaping strings (nlohmann escapes the backslash); `ensure_ascii=true` then
un-escaping (surrogate pairs, more code, `\ufffd` spelling change); customising
`nlohmann::detail::serializer` (internal API); no built-in option exists (V4).

## Scope
In: `report_to_json` (`src/core/report.cpp`), `batch_to_json` (`src/core/batch_json.cpp`). These
are the only producers of the four stdout JSON writes (V14: `cmd_batch.cpp:165`,
`cmd_batch_jobs.cpp:324`, `cmd_download_output.cpp:160`, `cmd_mdlist.cpp:246`).

Out (unchanged): `at_home_report_json` (`mangadex_api.cpp:596`, an HTTP body sent to MangaDex) and
`manifest_to_json` (`book_manifest.cpp:356`, stored in the EPUB, read back only by
`parse_manifest`; its exact bytes are pinned by `book_manifest_test.cpp:134-144`). U+2028/U+2029
stay raw (valid JSON, not terminal controls). `format_version` stays 1 (decoded strings are
unchanged); no README change.

## Tasks (one implementer track, in this order)

### Step 1: E2E driver (`tests/e2e/run_case.cmake`, `tests/e2e/CliE2E.cmake` header docs)
- Next to `_esc` / `_invalid_utf8` (~line 41): `string(ASCII 127 _del)`, `string(ASCII 194 155 _c1_csi)`.
- `_expand` (~line 58): `{{DEL}}` → `7F`, `{{C1_CSI}}` → `C2 9B`.
- In the `EXPECT_JSON OR EXPECT_JSON_LINES` block: fail with
  `stdout: raw DEL or C1 control in the JSON (must be \u-escaped as \u007f / \u0080..\u009f)` if
  stdout contains `_del` or any `C2 xx` (xx 128..159). Build the 32 needles with
  `foreach(_b RANGE 128 159)` + `string(ASCII 194 ${_b} …)` + `string(FIND)`. Never a lone-byte or
  raw-byte regex needle (`9B` alone matches inside `ě` = `C4 9B`; V9b).
- `EXPECT_NO_ANSI`: also reject `_c1_csi` (8-bit CSI) on stdout and stderr; message
  "ANSI escape sequence (ESC[ or 8-bit CSI U+009B) found in the output (expected none)".
  `EXPECT_ANSI` unchanged. (No existing case carries these bytes, V14c/V17. Gate decision 1.)
- Document both placeholders and the new semantics in the `CliE2E.cmake` header.
- Note: CMake's `string(JSON)` is lenient (accepts raw C0 and invalid UTF-8), so the raw-byte check
  and iconv are the real guards; `ERROR_VARIABLE` reads `NOTFOUND` on success (V11c).

### Step 2: red-first E2E and unit tests
Write the cases below and run them against the unfixed binary; record that they fail (this is the
evidence that Step 1's checks fire).

### Step 3: core helper and both producers
New `src/core/json_output.hpp` (private, `#pragma once`, `<nlohmann/json_fwd.hpp>` + `<string>`;
V7) and `src/core/json_output.cpp` (globbed, V19):

```cpp
namespace mangadex_dl::json_output {
/// Precondition: `text` is UTF-8 JSON text from nlohmann dump(..., error_handler_t::replace).
/// Each raw 0x7F becomes \u007f and each C2 80..C2 9F pair becomes \u0080..\u009f (lowercase
/// hex, nlohmann's C0 spelling). Every other byte is copied unchanged.
[[nodiscard]] std::string escape_del_and_c1(std::string text);
/// dump(2, ' ', ensure_ascii=false, error_handler_t::replace), escape_del_and_c1, then '\n'.
[[nodiscard]] std::string dump_document(const nlohmann::ordered_json& doc);
}
```

Algorithm: a counting pass computes `extra` (+5 per `7F`, +4 per `C2 [80-9F]`, skipping the
pair's second byte); `extra == 0` → return `text` (fast path); guard
`extra > text.max_size() - text.size()` → `std::length_error` (unreachable in practice, would reach
`main` as 70; say so); else `reserve` and one rewrite pass with identical skip logic and a
`constexpr std::string_view` hex table. A `C2` at the end or not followed by 80..9F is copied as is.
Bytes via `static_cast<unsigned char>`.

- `report_to_json` → `return json_output::dump_document(detail::report_json(report));`
- `batch_to_json` → `return json_output::dump_document(doc);`
- Replace the comments at `report.cpp:277-279` and `batch_json.cpp:184-189`: nlohmann escapes C0
  (ESC included), `dump_document` escapes DEL and C1, so the document carries neither a 7-bit nor
  an 8-bit escape sequence.
- `src/core/report_json.hpp` header comment: `report_to_json()` is exactly
  `json_output::dump_document(report_json(r))`.
- `docs/design/list-factcheck.md` (V17b / decision 11) and `docs/design/list-plan.md`: a one-line
  "resolved by json-c1" note.

## Test plan
Unit, `tests/unit/json_output/json_output_test.cpp` (`[json][json_output]`), with a test helper
`tests/support/json_bytes.hpp` (`has_raw_del_or_c1(std::string_view)`):
1. `""` → `""`.
2. Byte-identical: ASCII JSON, `進撃の巨人`, `😀`, `EF BF BD`, `ě` (`C4 9B`), U+201B (`E2 80 9B`),
   U+0100 (`C4 80`); boundaries `C2 A0`, `C2 AD`, `C2 BF`.
3. `7F` → `\u007f`; each U+0080..U+009F → `std::format("\\u{:04x}", cp)`.
4. Adjacent and mixed: `7F 7F`, `C2 9B C2 9B`, `a 7F C2 85 b` → exact strings.
5. Trailing lone `C2` copied unchanged (bounds, under ASan); `C2 7F` → `C2` + `\u007f`.
6. `dump_document(doc) == doc.dump(2,' ',false,replace) + "\n"` for a nested doc with C0, CJK and
   invalid UTF-8 but no DEL/C1; exactly one trailing `\n`.
7. Property: every scalar U+0000..U+FFFF (minus surrogates) plus U+10000, U+1F600, U+10FFFF:
   equals the raw dump unless cp ∈ {7F, 80..9F}, else `"\"\\u00xx\"\n"`; strict parse round-trips.
8. Keys: `{"k\x7f": {"n": ["v\xc2\x9b"]}}` → `"k\u007f"`, `"v\u009b"` at depth 3.
9. Invalid UTF-8 pinned by fact-check V2b: `C1 9B` → FFFD FFFD; `C2 C2 9B` → raw FFFD + `\u009b`;
   `C2 7F` → FFFD + `\u007f`; lone `9B` → FFFD; output parses, `!has_raw_del_or_c1`.

Unit, extended:
- `report_test.cpp:340`: add DEL, `C2 9B`, `C2 9C`, `C2 9D` to the hostile title; no raw DEL/C1;
  `\u009b[31m` and `\u007f` present; exact round trip. Valid-UTF-8 test: add `é`, U+00A0 (raw).
- `batch_json_test.cpp:160` `redump` delegates to `json_output::dump_document` (required: the
  parsed embedded report holds real DEL/C1, V18); `:362` add `\x7f\xc2\x9b` to the hostile title;
  `:392` → "never an ESC, DEL or C1 byte", add C1 to `source`/`dir`/`error`, DEL to the title.
- `batch_json_list_test.cpp:144`: add DEL and U+009B to the list name; rewrite header lines 5-8.

E2E edge (`tests/e2e/cases/_edge.cmake`), DIR is the injection path (V13, verified on the binary):
- `json-del-c1-dir`: `${EDGE_JSON_ARGS} "out{{DEL}}{{C1_CSI}}x"`
- `batch-del-c1-dir-json`: `batch --json --dry-run ${_edge_batch_clean_api} ${_edge_batch_in}/clean.csv "out{{DEL}}{{C1_CSI}}x"`
- `mdlist-del-c1-dir-json`: `mdlist --json --dry-run ${_edge_mdlist_clean_api} ${_edge_mdlist_l1} "out{{DEL}}{{C1_CSI}}x"`

each `EXPECT_EXIT 0 EXPECT_JSON EXPECT_NO_ANSI EXPECT_STDERR_EMPTY` with a stdout regex for
`out\u007f\u009bx`. Every existing `EXPECT_JSON` / `EXPECT_NO_ANSI` case also gains the stronger
checks. No in-process CLI case (the app only prints the core string). No sanitizer control.

Goldens: none change (V14c: no golden holds `7F` or `C2 80..9F`). No bless request.

## Memory-safety watch-list
- The `text[i + 1]` read after a `C2` at `i == size - 1`: bounds check first, in both passes.
- Count pass and rewrite pass share identical skip logic (`reserve` is only an optimisation).
- The `extra` overflow guard; `char` → `unsigned char`; no use of `text` after the fast-path move.
- `dump_document(detail::report_json(report))` binds a temporary to `const&` for the full expression.
- No stored `string_view`/`span`; tests keep `report_to_json(...)` in a `const std::string`.

## Gates
`nice pixi run check` and `nice pixi run -e gcc check` (byte math under `-Wconversion` /
`-Wuseless-cast`). Track reviewers: `quality-reviewer` (build-review mode for the driver) and
`memory-safety-reviewer`. Board: `quality-reviewer`, `memory-safety-reviewer`, `security-reviewer`,
`cli-ux-reviewer`.

## Residuals (flagged, not fixed here)
- Human-mode stdout prints `outcome.path` raw (`cmd_download_output.cpp:204,212`; batch/mdlist
  reuse it), so a DIR holding U+009B reaches a terminal as an 8-bit CSI. Stdout is data and DIR is
  the user's own argument; a candidate follow-up.
- `last_resort(e.what())` (`io.cpp:212-217`) prints raw exception text to stderr; not reachable by
  any current case.

## Gate decisions (settled by the user at plan approval)
1. `EXPECT_NO_ANSI` gains the 8-bit CSI on **stdout and stderr**.
2. Scope as above: the two non-stdout dumps and U+2028/9 untouched; no `format_version` bump; no
   README change. The extras (residuals, manifest/at-home escaping, U+2028/9, skill drift) are
   recorded in `TODO.md`.
3. **One implementer track**: driver → red-first tests → fix.
