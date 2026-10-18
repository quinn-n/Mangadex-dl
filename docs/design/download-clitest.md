# cli-tester results (installed binary, fixture mode, 2026-10-18)

Acceptance: 1–6, 10–18 ✅; 19 ⚠️ (download.help golden pending cli-ux); 22 ✅ mechanical (real SIGINT → 130, SIGTERM → 143 on a slow scratch fixture; only the finished Vol. 01 left, no partial, no `.part-*`; re-run resumes) — O_TMPFILE path only; 7–9 unit-only; 20, 21, 23 not run (human/network).
Checklist: help/version, usage errors, no CLI11 100–114 leak, stream split, /dev/full → 1, SIGPIPE → silent 141 (459 KB bulk), stdin closed/null, no ANSI on non-TTY, --json valid and ANSI-free under --color=always/CLICOLOR_FORCE/pty, LC_ALL=C, invalid-UTF-8 DIR, pty progress + yellow warning, NO_COLOR/TERM=dumb/--json/-q suppress progress. jobs 1 vs 8 and dev vs release byte-identical EPUBs; unzip/XML sanity ✅.

Defects (minor): D1 CLI11 parse errors lack `mangadex-dl:` prefix (→ cli-ux); D2 filename sanitiser keeps non-characters (U+FFFE); D3 OPF `dc:title` keeps U+202E (filename drops it); D4 info (pty merges streams).
Gaps: G1 no automated real-signal E2E case with partial-file check (needs a slow fixture); G2 incomplete-case ordering/partial assertions (check existing).
Could not verify: live MangaDex (20), EPUB readers + epubcheck (21), static binary HTTPS on a clean machine incl. CA discovery (23); all HTTPS/TLS/proxy/retry/429/MD@H behaviour; `.part-` fallback without O_TMPFILE; SIGPIPE leaving `.part-`; real terminal rendering; other OS/arch; release-build signal behaviour.
