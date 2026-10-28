# Feature spec: `mangadex-dl follows`, `login`, `logout`: downloading the titles you follow

Status: DRAFT 2026-10-28 (cli-designer). Step 2 of "download the works I follow". Step 1 (`mdlist`) is merged; see `docs/design/list-spec.md`. Several commands are designed here, in separate subsections: **`follows`** (the feature), **`login`** and **`logout`** (small helpers that only exist if gate Q3 picks them). Every recommendation below assumes the recommended gate answers. Where another answer changes the design, the delta is noted.

**Intent:** log in to MangaDex with the user's personal API client, page through `GET /user/follows/manga`, and run every followed title through the unchanged batch engine. It is exactly `mdlist`'s path, with the follows collection in place of `GET /list/{id}`.

**Who runs it:**
- A person at a terminal: the first run asks for the login details once and saves the login.
- After that, mostly cron or a script that runs the same command unattended to pick up new chapters and new follows.

**Assumptions:**
- One account per run, one output root DIR. The run-level options are `mdlist`'s, byte for byte.
- **Only the follows fetch is authenticated.** Every per-title request (`/manga/{id}`, feed, covers, at-home, images, MD@H reports) stays anonymous, exactly as today. Reasons:
  - MangaDex's auth page says to send auth headers only when a call needs them (authenticated requests can't be cached), and never to any host other than `api.mangadex.org`/`auth.mangadex.org`.
  - The 15-minute access-token lifetime then never meets the long download phase (see "Token lifetime").
- Titles run one after another; `--jobs` parallelises images within a title. No new threads.
- **Fact from the user's live test (coordinator, 2026-10-28):** an anonymous `GET /list/{id}` for a private list answers **404**. This closes list-spec V1. It matters only for the deferred "auth on `mdlist`" item.

**What was confirmed without credentials** (anonymous reads only; nothing was POSTed and no credentials were sent):
- Personal-clients docs:
  - token endpoint `POST https://auth.mangadex.org/realms/mangadex/protocol/openid-connect/token`, `application/x-www-form-urlencoded` (a form, "NOT a Json payload");
  - password grant: `grant_type=password&username&password&client_id&client_secret`;
  - refresh grant: `grant_type=refresh_token&refresh_token&client_id&client_secret`. **The refresh needs the client secret too.**
  - access token lifetime is 15 minutes;
  - clients may be "pending … requiring manual approval" and then "cannot be used yet".
- Realm discovery document (`/.well-known/openid-configuration`, public GET):
  - issuer `https://auth.mangadex.org/realms/mangadex`;
  - `revocation_endpoint …/protocol/openid-connect/revoke`, `end_session_endpoint …/logout`;
  - `client_secret_post` is among the supported auth methods; `password` and `refresh_token` among the grants.
- API spec: `GET /user/follows/manga` takes `limit` (1–100, default 10), `offset` and `includes[]`, and returns a `MangaList`. `GET /manga/status` returns one map from manga id to reading status (`reading`, `on_hold`, `plan_to_read`, `dropped`, `re_reading`, `completed`), unpaginated.
- Rate-limit page:
  - about 5 req/s per IP globally; 429, then a temporary 403 IP ban;
  - it lists only the *legacy* `POST /auth/login` 30/60 min and `/auth/refresh` 60/60 min. **The Keycloak endpoint's limits are undocumented.**
- Not confirmed (third-party code only): `expires_in`, `refresh_expires_in`, and refresh-token rotation (mangadex-full-api stores the new `refresh_token` from each refresh); "refresh token lasts about one month" (asyncdex docs).

---

## Decisions for the user at the design gate

**Q1. Where credentials come from** (client_id, client_secret, username, password). None of them ever goes on argv.

| Option | For | Against |
|---|---|---|
| A. Env vars `MANGADEX_DL_{CLIENT_ID,CLIENT_SECRET,USERNAME,PASSWORD}` only | Trivial in cron/CI; easy E2E | Env is readable through `/proc/<pid>/environ` by the same user and ends up in shell history and CI logs; secrets sit there every run; `cpp-security` allows env only as "the documented less-safe option" |
| B. A private credentials file only (`$XDG_CONFIG_HOME/mangadex-dl/credentials`, 0600 enforced) | One place, file permissions protect it | The first run needs a hand-written file; the password sits on disk |
| **C. Mix (recommended): prompt on a TTY + optional private credentials file + `--password-stdin`** | Zero setup on a terminal; the password is never stored unless the user chooses to; scripts can pipe it in; a fully unattended setup is still possible (file) | Three sources to explain, and more code (termios prompt) |
| D. C plus env vars | Maximum flexibility | All of A's downsides; a precedence question (env vs file) |

**Recommendation: C. Env vars are deferred** (additive later; they would rank above the file, which the tool applies itself).

Per-field precedence (no value passes through CLI11, so CLI11's CLI > config > env > default has nothing to arbitrate; the tool applies this order itself):
- client_id, client_secret, username: **credentials file > saved session (Q2) > TTY prompt > error (exit 8)**;
- password: **`--password-stdin` > credentials file > TTY prompt > error (exit 8)**. It is needed only when there is no usable session.

**Q2. Whether to persist the login**

| Option | For | Against |
|---|---|---|
| A. Memory only: password grant every run | No secret state on disk | Every cron run needs the password from a file or a pipe; one password POST per run against an auth endpoint with unknown limits and brute-force protection |
| **B. Persist a session (recommended)**: `$XDG_STATE_HOME/mangadex-dl/session.json`, 0600 in a 0700 dir, atomic writes. It holds client_id, client_secret, username, refresh token, access token and expiry times, **never the password**. Rewritten after every token response (rotation-safe). | `login` once in a terminal, then cron runs password-free; usually one refresh POST per run | A refresh token plus the client secret on disk equals API access to the account until it expires or is revoked; needs `logout` |
| C. Persist only the refresh token; the client secret always comes from the credentials file | Smaller session file | Forces the credentials file to exist, which defeats the zero-setup prompt path |

**Recommendation: B.** When the refresh token expires, `follows` silently falls back to a password grant if a password source exists. Otherwise it exits **8** with `run 'mangadex-dl login'`.

**Q3. Command surface**

| Option | For | Against |
|---|---|---|
| A. `follows` only (it prompts and saves; deleting the session is manual) | Smallest | No way to set up cron without downloading; no clean logout or revoke |
| **B. `follows` + top-level `login` + `logout` (recommended)** | `login` sets up cron in seconds and checks credentials; `logout` revokes and deletes; top level matches `docker login` | Two more top-level commands in the root help |
| C. A group: `auth login` / `auth logout` (later `auth status`) | Room for `status`/`whoami` | Longer to type; today it would be a group with only two members |

**Recommendation: B.** `whoami`/`status` is deferred.
- **Auth on `mdlist`** (to read your own private lists) is **deferred**. It is additive later and must be opt-in (a flag such as `mdlist --login`), never automatic, because anonymous `mdlist` behaviour (private list → 404 → exit 4, now confirmed live) is a contract.

**Q4. What "follows" means**
- A. **`GET /user/follows/manga` (recommended)**: the account's followed titles, which is what the command's name says.
- B. The library by reading status (`GET /manga/status`, filtered by `--status reading,…`): a different set (titles with a status but no follow, and the reverse, may exist), unpaginated, no inherent order.
- C. A now, plus `--status` later as an additive filter.

**Recommendation: A now, C later.** It is genuinely your call, because the mapping between the website's Library tabs and "follows" is not confirmed (VERIFY B5). Please check on the site which set you mean. AC F48 compares the count.

**Q5. Exit codes for auth.** Recommendation: **two codes**.
- **8 `login_required`**: nothing to log in with, or the saved login expired, and no way to ask. Remedy: `run 'mangadex-dl login'`.
- **9 `auth_rejected`**: MangaDex refused the details. Remedy: fix the credentials or the client.

A cron wrapper can tell "a human must type the password" from "the configuration is wrong". The alternative is one code (8) for both, with the message telling them apart.

**Q6. `--dir-per-title` default on `follows`.** Recommendation: **off, as on `mdlist`**, so the same flag means the same thing everywhere. The alternative is on by default, since follows are the big collections, plus a `--flat` flag.

---

## Summary

`mangadex-dl follows [DIR]` logs in to MangaDex as the owner of a personal API client and fetches the account's followed titles (paginated, at most 10 000). It then downloads each title into DIR through the same engine, options, stop rules, stderr wording ("item"), exit aggregation and `--json` document as `mdlist`. Only the follows request carries the access token; downloads are anonymous.
- The first interactive run asks for the client id, the client secret, the username and the password (with a hint where to create a client), logs in, and saves a session (never the password).
- Later runs, including cron, refresh that session silently.
- `mangadex-dl login` sets the session up without downloading. `mangadex-dl logout` revokes it at MangaDex (best effort) and deletes it.

Users: a person at a terminal first, then cron/scripts.

**Friction audit.** The primary task, `mangadex-dl follows ~/Manga`, takes one positional and zero flags:
- first time on a TTY: four prompts, once;
- afterwards: nothing to type until MangaDex ends the session;
- cron: zero, after one `login`.

Every credential flag is optional.

## Command surface (CLI11)

**Files:**
- app: `src/app/cmd_follows.{hpp,cpp}` (`register_follows`), plus `src/app/cmd_login.{hpp,cpp}` (`register_login`, `register_logout`). Three registration lines in `cli.cpp`, a **serialization point**.
- core: `include/mangadex_dl/auth.hpp`, `include/mangadex_dl/follows.hpp`, `src/core/auth*.cpp`, `src/core/follows*.cpp`.

### `follows`
- **Invocation:** `mangadex-dl follows [OPTIONS] [DIR]`.
- **Positional:** `DIR` (optional, default `.`), "Output directory (created if missing)", byte-identical to `mdlist`.
- **Options:**
  - Exactly `mdlist`'s set, bound by the same helpers, so the help strings are byte-identical: `bind_selection_options`, `bind_run_options` (including the hidden `--api-base`), `--fail-fast`, `--dir-per-title`.
  - New options (help texts fit the 44-column right column):
    - `--credentials FILE`: "Read login details from FILE". A path, not a secret. No `CLI::ExistingFile`: a missing file is reported when it is opened (TOCTOU), exit 1. No `capture_default_str` (the default depends on env, and the help must not). No `envname`.
    - `--password-stdin` (flag): "Read the password from stdin". Read once, after parsing and before any network use.
    - `--no-input` (flag): "Never prompt; fail if a login is needed".
  - **Not offered, on purpose:** `--password`, `--client-secret`, `--username`, `--client-id`, `--title`. See "Semantic usage checks".
- **Global flags** (`--json`, `-q`, `-v`, `--color`) work anywhere through the existing `fallthrough()`. `--version` has no `-v` alias.
- **Prompts:**
  - only when **stdin and stderr are both TTYs**, `--no-input` is absent and `--password-stdin` is absent (stdin is spoken for);
  - otherwise a missing value gives **exit 8** with the remedy. This deliberately deviates from cli-design's "needed prompt with `--no-input` → exit 2": the invocation is valid; what is missing is state.
- **Semantic usage checks** (exit 2, empty stdout, no fd opened, no request sent; each ends with `Run 'mangadex-dl follows --help' for more information.`):
  - `DIR must not be empty`.
  - `DIR looks like a MangaDex id; follows takes no title or list (use 'mangadex-dl mdlist <list>' or 'download <title>')`.
  - Every run option is validated as in `mdlist` (`detail::validate_common`).
  - `--api-base` that is neither the official API nor a `file://` fixture: `follows logs in only to MangaDex itself; --api-base must be the official API`. This keeps credentials from ever reaching a custom host.
  - `--password-stdin` with an empty stdin (EOF before any byte, `/dev/null` or closed), more than 4096 bytes, a NUL byte, or an empty first line: `--password-stdin was given, but stdin held no password`.
  - `--title`/`--title=X`: `follows has no --title option; …`, which generalises `title_option_owner`.
  - **Secret-shaped extras:**
    - `--password`, `--password=…`, `--client-secret…`, `-p`: `follows has no --password option: passwords and secrets never go on the command line (other users can see it); use the prompt, --password-stdin or the credentials file`.
    - **For `follows`, `login` and `logout`, no ExtrasError ever echoes an argument's value or a bare word.** Only option names, cut at `=`, are echoed. CLI11's default ExtrasError text lists the extras verbatim (VERIFY C1), and `follows out hunter2` must not print `hunter2`.
- **Root:** `unknown command 'x' (commands: download, batch, mdlist, follows, login, logout)`. Root `--help` lists the three new one-liners.

**Help draft (`follows --help`, for cli-ux; lines ≤ 80):**
```
Download every title you follow on MangaDex (needs a login)


mangadex-dl follows [OPTIONS] [DIR]


POSITIONALS:
  DIR [.]                           Output directory (created if missing)

OPTIONS:
  ... exactly as mdlist, including --fail-fast and --dir-per-title ...
      --credentials FILE            Read login details from FILE
      --password-stdin              Read the password from stdin
      --no-input                    Never prompt; fail if a login is needed

LOGIN:
  Reading your follows needs a MangaDex login through a personal API client.
  Create one at https://mangadex.org/settings (API Clients); a new client
  may need approval by MangaDex staff before it works. The first run in a
  terminal asks for the client id and secret, your username and password,
  logs in, and saves the login (never the password) to
  $XDG_STATE_HOME/mangadex-dl/session.json (~/.local/state/mangadex-dl/ by
  default), readable only by you. Later runs, cron jobs included, reuse it
  until MangaDex ends the session; then run 'mangadex-dl login' again.

  The details can also come from a private file (chmod 600):
  $XDG_CONFIG_HOME/mangadex-dl/credentials (~/.config/mangadex-dl/ by
  default) or --credentials FILE, one 'key = value' per line with the keys
  client_id, client_secret, username and, optionally, password.
  Never put a password on the command line.

  Only the list of follows is fetched with your login; the titles
  themselves are downloaded anonymously, one after another, as with mdlist.

Examples:
  Log in once, then download everything you follow into ~/Manga:
    mangadex-dl login
    mangadex-dl follows ~/Manga
  One folder per title:
    mangadex-dl follows ~/Manga --dir-per-title
  In a script, with the password from a password manager:
    pass show mangadex | mangadex-dl follows ~/Manga --password-stdin
  Show what would be written, without downloading anything:
    mangadex-dl follows ~/Manga --dry-run

Running the same command again skips files that already hold every chapter,
rebuilds files MangaDex has added chapters to, and downloads titles you have
followed since the last run.

Symlinks inside DIR are followed: don't point DIR at a directory that others
can write to. Each title costs several API requests: many follows make a long
run.

Exit status:
  ... exit_status_footer() ...
```

### `login`
- **Invocation:** `mangadex-dl login [OPTIONS]`. One-liner: "Log in to MangaDex and save the login for follows".
- **Options:** `--credentials FILE`, `--password-stdin`, `--no-input`, hidden `--api-base` (same rules and help strings as on `follows`). No positionals.
- **Behaviour:** always a **password grant**, even when a session exists (this is how the user renews or switches the login).
  - Values are gathered by the Q1 order.
  - The password prompt shows the username: `MangaDex password for <username>: `.
- **Output:** stdout is always empty (no data, also with `--json`, documented). stderr, not with `-q`: `mangadex-dl: logged in to MangaDex as <username>; the login is saved in <path>`.

### `logout`
- **Invocation:** `mangadex-dl logout`. One-liner: "Remove the saved MangaDex login". Hidden `--api-base` only (fixture tests). No prompts. stdout always empty.
- **Behaviour:**
  1. take the session lock and read the session;
  2. **best effort**: POST the RFC 7009 revocation (`token=<refresh>&token_type_hint=refresh_token&client_id&client_secret`) to the discovery document's `revocation_endpoint`, with a 5 s connect/stall limit, no retries, the answer ignored apart from success/failure;
  3. then `unlinkat` the session file.
- **Messages:**
  - `logged out; removed <path>`;
  - `not logged in (no saved login at <path>)` → exit 0 (idempotent);
  - a failed revoke → warning `could not end the session at MangaDex (<reason>); the saved login was removed anyway` → exit 0.
- `logout` never reads or touches the credentials file. The README says "delete it yourself if it holds your password".

## Input & output

**Sources and precedence.** There is no CLI11 config file and no `envname` anywhere. CLI11's CLI > config > env > default has nothing to arbitrate, and the CLI-level options keep "command line > built-in default". The credential precedence of Q1 is applied by the tool. If env vars are added later (Q1-D), they rank **above** the credentials file, again applied by the tool, which is the opposite of CLI11's config-beats-env.

**Credentials file:**
- Path: `--credentials FILE`, else `$XDG_CONFIG_HOME/mangadex-dl/credentials`; `XDG_CONFIG_HOME` is used only if absolute, else `$HOME/.config/…`. No usable `HOME` means no default file.
- An absent default file is not an error. An absent `--credentials` file is exit 1.
- Opening and checks: `open(O_RDONLY|O_CLOEXEC|O_NOCTTY|O_NONBLOCK)`, symlinks followed (dotfile managers), then `fstat` on the fd, which must show:
  - a regular file;
  - `st_uid == geteuid()`;
  - `(st_mode & 077) == 0`.

  Otherwise exit 1: `credentials file '<path>' is readable or writable by others (mode 0644); run 'chmod 600 <path>'`, or `… is not owned by you`, or `… is not a regular file`.
- Read bounded: at most 16 KiB (`read_bounded_fd` semantics).
- Format, lines of `key = value`:
  - keys are exactly `client_id`, `client_secret`, `username`, `password`;
  - `#` at the start of a line (after optional whitespace) begins a comment; blank lines are allowed; a final `\r` is stripped;
  - the value is the rest of the line after the first `=`, trimmed of spaces and tabs; an empty value means absent;
  - errors (exit 1, naming the line, **never the value**):
    - an unknown key (echoed only if it matches `[a-z_]{1,32}`);
    - a duplicate key;
    - a line without `=`;
    - a control byte or NUL in a value;
    - invalid UTF-8.
  - A password with leading or trailing spaces can't be written in the file; the README points to `--password-stdin`.

**Prompts** (stderr, read from the stdin TTY):
- Before the first client prompt, one line: `mangadex-dl: reading your follows needs a MangaDex personal API client; create one at https://mangadex.org/settings (API Clients)`.
- The prompts, in order:
  - `MangaDex API client id: ` (echo on);
  - `MangaDex API client secret: ` (**echo off**);
  - `MangaDex username: ` (echo on);
  - `MangaDex password for <username>: ` (**echo off**).
- Echo off means termios `~ECHO` with `TCSAFLUSH`, restored by RAII on every path, then a newline on stderr.
- Each answer: at most 4096 bytes up to `\n`. Empty, or EOF (Ctrl-D) → exit 8 `no <field> given`.
- Ctrl-C at a prompt → echo restored, nothing saved, 130. The existing handlers use `SA_RESTART`, so the reader must poll and check the pending-interrupt flag (VERIFY C5).
- In-process tests replace the real prompt through an `Io` seam (a prompter interface).

**`--password-stdin`:**
- `read_bounded_fd(io.stdin_fd, "stdin", 4096)`, read to EOF;
- the password is the first line, without its `\n`/`\r\n`;
- read once and always when the flag is given, even if a session makes it unnecessary (a predictable pipe contract).

**stdin otherwise:** read only for prompts or `--password-stdin`. Closed stdin or `/dev/null` without them changes nothing (a needed prompt is then exit 8).

**stdout:** data only, exactly `mdlist`'s: one path per written or planned file, in follows order. `login`/`logout` print nothing on stdout.

**stderr for `follows`:**
- Row prefix: `follows:<n>: <manga-uuid>: ` after `error:`/`warning:`. `<n>` is the 1-based position in the API's follows order.
- Wording is `mdlist`'s "item" wording, including the closing line, duplicate, collision and dir-per-title lines. This is `BatchSource::follows`, which shares `mdlist`'s strings.
- Run-level lines (no prefix):
  - empty: `warning: you follow no titles on MangaDex` (not with `-q`), exit 0, DIR not created;
  - session not saved: `warning: could not save the MangaDex login to <path>: <reason>; the next run will need the password again` (the run continues);
  - ignored session: `warning: ignoring the saved login at <path>: <why>` (malformed, wrong owner or mode, another account or client than the credentials file); a fresh login follows if possible.
- `-v`:
  - `mangadex-dl: logged in to MangaDex as <username> (saved login|refreshed|password)`;
  - `mangadex-dl: follows: <n> titles`;
  - per item `follows:<n>: <uuid>`, then the engine's lines.
  - The username is display-only: cut to 64 bytes on a UTF-8 boundary, passed through `terminal_safe`.
- `-vv`: the request trace. For POSTs it shows the **method, URL, status and time only**, never the body or headers: `POST https://auth.mangadex.org/realms/mangadex/protocol/openid-connect/token -> 200 (210 ms)`. The follows trace is `GET …/user/follows/manga?limit=100&offset=0 -> 200 (… ms)`; the token travels in a header, not the URL.
- **Progress:** as `mdlist` (TTY stderr only, never with `-q`/`--json`): `(2/57) [1/3] Vol. 1: 3/5 pages`. Prompts are printed before any progress line.

**`--json`** (follows only):
- One document at the end, under the same rule as `mdlist`: printed when the items ran and were not interrupted. **Never for usage errors, auth failures (8/9), follows-fetch failures (3), or interrupts.**
- It is `ordered_json`, indent 2, `error_handler_t::replace`, never ANSI (even with `CLICOLOR_FORCE=1`).
- **It is `batch`'s document plus one additive key `follows` after `source`.** `format_version` stays 1. `batch` and `mdlist` documents are byte-identical to today's.
  ```
  format_version: 1
  dry_run: bool
  source: "follows"                  the literal word (there is no positional to echo)
  follows: {                         follows only
      username: string               as configured (file/session/prompt), never from the API
      titles: int                    number of follows the API returned, duplicates included
  }
  titles: [ { line: int (position in follows order), manga_id, status, exit_code, error,
              duplicate_of (a position), report, dir } ]          exactly as mdlist
  summary: { titles: {...}, files: {...} }                        as batch
  ```
- **No token, secret, password or client id ever appears in the document.**
- Empty follows → `titles: []` plus the `follows` object.

**Color:** stderr prefixes only, through `resolve_color`. Prompts are never colored.

**Locale:** positions and counts use `std::format` without `L`. Expiry times are integers (`from_chars`/`to_chars`).

## Exit codes

New codes in `exit_codes.hpp` (a serialization point), each with a doc comment, plus `Errc::login_required` and `Errc::auth_rejected` in `error.hpp` (every `switch (Errc)` gets them, including the batch runner's `is_fatal`; VERIFY C12):

| Code | Name | Meaning / when |
|---|---|---|
| **8** | `login_required` | follows, login: no usable saved login, and the missing value can't be asked for (no TTY, `--no-input`, `--password-stdin` already used stdin, empty answer or EOF at a prompt); or no API client configured at all; or the saved session has expired or been revoked (`invalid_grant` on refresh) and no password source exists |
| **9** | `auth_rejected` | follows, login: the auth server refused the details: wrong username or password (`invalid_grant` on the password grant), wrong client id or secret (`invalid_client`), a client that is pending, disabled or not allowed this grant (`unauthorized_client`/`invalid_client`), a locked account; or the API answered 401 to a freshly obtained access token |

Messages (fixed text, never a secret; the server's `error` code is appended only if it matches `[a-z_]{1,64}`; its `error_description` only with `-v`, `terminal_safe`, cut to 200 bytes):
- 8, no client: `no MangaDex API client is set up; run 'mangadex-dl login' in a terminal, or put client_id and client_secret in <credentials path>`.
- 8, login needed: `you are not logged in to MangaDex; run 'mangadex-dl login' in a terminal (or pass --password-stdin)`.
- 8, session ended: `your saved MangaDex login has expired or was ended; run 'mangadex-dl login' in a terminal`.
- 9, password: `MangaDex rejected the username or password for <username> (invalid_grant); check them, or log in on the website to see whether the account is locked`.
- 9, client: `MangaDex rejected the API client (invalid_client); check client_id and client_secret, and whether the client is approved at https://mangadex.org/settings`.
- 9, fresh token refused: `MangaDex rejected a new access token for your follows (HTTP 401); check that the API client is approved and allowed to read your account`.

Mapping of the remaining outcomes:

| Outcome | Code |
|---|---|
| all items ok or duplicate; no follows; `login` succeeded; `logout` (also when not logged in or the revoke failed) | 0 |
| credentials file unreadable, insecure, malformed or (explicit path) missing; the session lock not acquired within 60 s; `login` could not save the session; `logout` could not remove it; backend unusable (no libcurl); DIR unusable and other global local errors (rest `not_run`); stdout write error | 1 |
| every `ParseError`; DIR checks; bad option values; non-official, non-fixture `--api-base`; empty `--password-stdin`; `--title`/`--password`-style extras | 2 |
| auth or follows endpoint: connect, DNS, TLS, timeout, 5xx/429 retries exhausted, 403 (possibly a ban, existing message); malformed token response (missing `access_token`, a bad token charset or size); malformed follows page (wrong type, a non-UUID id, over 16 MiB); more than 10 000 follows (refuse to truncate); more than 3 token requests in one run | 3 |
| items: one failure class → that code (1, 3, 4, 5, 6); several → 7 | as `mdlist` |
| SIGINT/SIGTERM (also at a prompt or during auth) | 130/143 |

- Auth and follows request failures get `with_remedy`'s network remedy. Content failures (malformed, over the cap) don't, the same split as `mdlist`'s `ListFailureKind`.
- **Help footer** (shared, so every help golden changes): row 7 is reworded to fit 80 columns now that it would name three commands, and two rows are added:
  - `7   several titles failed in different ways (see per-title results)`
  - `8   follows/login: not logged in, or the saved login expired`
  - `9   follows/login: MangaDex rejected the login details`
  - Also update the README EXIT STATUS and the `mixed_failures` doc comment.

## Behaviour & edge cases

**`follows` phases:**
1. **Validate (pure, exit 2).**
2. Install interrupt handlers. Optionally harden the process against core dumps (see "Secret hygiene"). Then `make_backend`.
3. **Read `--password-stdin`** if given (exit 2 if empty).
4. **Credentials:** read and check the credentials file.
5. **Session:**
   - take the exclusive lock (`<state dir>/session.lock`, `flock` with LOCK_NB polled every 250 ms while checking interrupts, at most 60 s → exit 1 `another mangadex-dl is using the saved login; try again`);
   - read the session (bounded 64 KiB, `parse_untrusted`, every field validated);
   - ignore it, with a warning, if it is malformed, has the wrong owner or mode, has another issuer, or its client_id/username differ from the credentials file.
6. **Get an access token,** at most 3 token POSTs per run:
   1. the saved access token, if it is still valid for at least 60 s (wall clock);
   2. else a refresh grant, if there is a refresh token;
   3. on `invalid_grant` there, or with no session, a password grant: password from the Q1 sources, prompting for missing fields if allowed, else exit 8.

   **After every successful token response the session is written atomically before anything else happens,** including before a pending interrupt is honoured, so a rotated refresh token is never lost. A failed write is a warning; the run continues.
7. **Fetch the follows:**
   - `GET <api>/user/follows/manga?limit=100&offset=<k>` with `Authorization: Bearer <access>`, through the existing `paginate` (step by the items received, stop on an empty page or `offset >= total`, refuse above the 10 000 window);
   - parse each page (`MangaList`, `data[i].type == "manga"`, `data[i].id` passes `is_uuid` → `MangaId`, attributes ignored);
   - **before each page**, if the token expires within 30 s → refresh first;
   - **on a 401 for a page** → one refresh (or password fallback), then retry that page once; a second 401 → exit 9.
   - Then release the lock, and wipe and destroy every token and secret in memory.
8. **Plan:** `plan_list`-equivalent jobs (position, DIR, options, `dir_per_title`, duplicates marked). Empty → warning, JSON if asked, exit 0.
9. **Run** via `run_batch_jobs` with `BatchSource::follows` and the follows summary, over the same anonymous `Backend`.

**Token lifetime during a long run** (the explicit answer):
- The token is used only in phase 7, which is bounded: at most 100 pages at about 250 ms each, plus any 429 back-off.
- Expiry is handled proactively before each page, plus one reactive 401 retry.
- Phase 9 (possibly hours, sequential titles with `--jobs` image workers) sends **no Authorization header at all**, so the 15-minute expiry can't bite there. Nothing refreshes during downloads, and no token is shared with worker threads.
- Rejected: an authenticated per-title client with a shared refresher. It would need a thread-safe token cache across `FetchPool` workers, would defeat the CDN cache for every title, and goes against MangaDex's "send auth only when needed".

**Auth requests:**
- POST form body, percent-encoded by a pure core function (RFC 3986 unreserved characters kept, everything else `%XX`).
- Token response body cap 64 KiB.
- `access_token` and `refresh_token` must match `[A-Za-z0-9._~+/=-]{1,16384}`. This prevents header injection and is checked before use; anything else → 3.
- `expires_in` must be an integer in 1–86400, else 900 is assumed. `refresh_expires_in`: 0 or absent → unknown (null).
- If the refresh response carries a new `refresh_token`, it replaces the old one (rotation).
- Retries: transport errors, 5xx and 429 use the existing `decide_retry` policy (429 honours `Retry-After`/`X-RateLimit-Retry-After`). **4xx other than 429 is never retried** (Keycloak brute-force protection), and the auth host gets its own 1 req/s limiter.
- **The Authorization header** is attached only to the follows URL, whose host must be `api.mangadex.org` (or the fixture). It is never sent to any other host. Redirects are already off (`FOLLOWLOCATION 0`), so no header can be forwarded.

**The five things:**
1. **Success:** paths on stdout, empty stderr.
2. **Empty:** "you follow no titles", exit 0.
3. **Usage errors:** exit 2.
4. **Runtime failures:** 1, 3, 8 or 9 as tabled.
5. **Interrupt:**
   - at a prompt: echo restored, nothing saved;
   - during auth: the session is unchanged, or fully rewritten (atomic);
   - during the follows fetch: nothing printed or created, no JSON;
   - inside items: exactly as `mdlist`.

All of them give 130/143.

**Edge suite:**
- stdin `/dev/null` or closed:
  - with a session: exit 0;
  - without one: 8, never a hang;
  - with `--password-stdin`: 2.
- `--dry-run > /dev/full` → 1 plus `mangadex-dl: write error`.
- `| head -1` → 141 (also when the parent ignores SIGPIPE).
- No ANSI when piped or with `--json` under `CLICOLOR_FORCE=1`.
- Invalid-UTF-8 DIR under `LC_ALL=C` → verbatim, valid JSON.
- An invalid-UTF-8 username in the credentials file → exit 1 (invalid UTF-8), never echoed.
- `argc == 0` is unchanged.

**Idempotency:**
- Re-runs pick up new follows and new chapters.
- Unfollowed titles' files are left alone.
- `login` twice replaces the session; `logout` twice gives 0 and 0.
- **Concurrent runs** are serialised on the session lock, so two cron runs can't burn each other's rotated refresh tokens.

**`--dry-run`:** still logs in and fetches the follows (a dry run of downloads, not of auth); it may refresh and save the session, and writes nothing in DIR.

**Large input:** at most 100 pages of at most 16 MiB each, processed one page at a time; at most 10 000 ids kept.

## Secret hygiene (`cpp-security` §9)

| Channel | Rule |
|---|---|
| argv / `ps` | No secret options exist; secret-shaped extras get a fixed, value-free message; ExtrasError never echoes values for these commands |
| env | Not read for credentials (Q1-C). `EnvSnapshot` gains only `HOME`, `XDG_CONFIG_HOME`, `XDG_STATE_HOME`, read once at startup (note: `read_env` is `noexcept` today and would now allocate) |
| stderr / `-v` / `-vv` | No token, secret or password, ever. POST traces show method + URL + status. The username is shown (not a secret). Server error text is restricted as described under "Exit codes" |
| `--json` | Only `follows.username` and the count |
| files | Session 0600 in a 0700 dir; the credentials file is checked for 0600 and owner |
| core dumps / ptrace | Recommended: `prctl(PR_SET_DUMPABLE, 0)` at the start of `follows`/`login`/`logout`, before any secret is read (no core file, no same-user `/proc/<pid>/mem`). The interplay with LSan in the dev preset must be probed (VERIFY C7); the fallback is `setrlimit(RLIMIT_CORE, 0)` |
| memory | Recommended, best-effort: a move-only `Secret` type (no formatter, no `operator<<`, `explicit_bzero` in its destructor and on reassign, capacity reserved up front so no realloc leaves copies). Copies inside libcurl (`COPYPOSTFIELDS`) and the TLS stack are out of reach; the planner may switch secret bodies to `CURLOPT_POSTFIELDS` over the caller-owned body. **Not a gate** |
| child processes | None are spawned |

## Data & state

- **Reads:**
  - the credentials file (optional);
  - the session file;
  - the follows pages (`api.mangadex.org`);
  - token, refresh and revoke answers (`auth.mangadex.org`);
  - existing EPUBs in DIR, as `mdlist` does.
- **Writes:**
  - DIR outputs exactly as `mdlist` (atomic `OutputDir` publish);
  - **session state** under `$XDG_STATE_HOME/mangadex-dl/` (`XDG_STATE_HOME` used only if absolute, else `$HOME/.local/state/mangadex-dl/`):
    - missing directories are created 0700;
    - the final directory is opened `O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC` and `fstat`ed: owned by the euid, `(mode & 077) == 0`, else the session is not used or written (warning);
    - files are reached with `openat` from that fd and `O_NOFOLLOW` (a symlinked session file is refused with a warning);
    - writes go `mkostemp`-style (`.session.json.XXXXXX`, `O_CLOEXEC`) → `fchmod 0600` → write all → `fsync` → `close` checked → `renameat` → `fsync` dir; `unlink` the temp on failure;
    - `session.lock` is 0600, created `O_NOFOLLOW`;
    - no usable `HOME` → no persistence (warning; `login` exits 1).
  - **Fixture mode** uses `fixture-session.json` in the same directory and stores the fixture issuer, so a test run can never clobber or read a real login.
- **Session file** (JSON, `format_version` 1, ≤ 64 KiB):
  - `issuer`, `client_id`, `client_secret`, `username`, `refresh_token`, `refresh_expires_at` (UNIX s or null), `access_token`, `access_expires_at`;
  - an unknown `format_version` → ignored with a warning (never rewritten by an older binary without a fresh login).
- **Nothing else:** no cache, no TMPDIR use, no credentials-file writes (the tool never creates or edits it).
- **Threat model:**
  - **Follows pages** are untrusted (ids validated before use; names unused).
  - **Token responses** are untrusted (charset and size checked).
  - **The credentials file and session** are user-owned but checked, because a world-writable session could plant someone else's token.
  - **`--api-base`** restricted, as above.

`security-reviewer` must review this feature: secrets, files, env and untrusted input are all involved.

## Engine reuse (for the planner; names are suggestions)

- **Core, new:**
  - `auth.hpp`: `ClientCredentials`, `Secret`, `Session` (validated factory returning `Result`), `parse_credentials_file`, `parse_session`/`session_to_json`, `form_encode`, `parse_token_response`, `classify_token_error` → login_required/auth_rejected/network, and an `AuthClient` over `Transport` + `Clock` with the token POST, retry and limiter;
  - `follows.hpp`: `parse_follows_page`, `follows_request_url`, `fetch_follows` (paginate with a token provider callback for refresh-before-page and 401-retry), `plan_follows`.
- **Core, changed (serialization points):**
  - `net/transport.hpp`: `HttpRequest` gains an additive `authorization` (Bearer value) field, validated against CR/LF/NUL like `content_type`. The curl transport needs a header slist on GET too.
  - `api_client.hpp`: Endpoints gains `auth` (official `https://auth.mangadex.org/realms/mangadex`; fixture `file://<root>/auth/realms/mangadex`). `ApiClient` gains an authenticated follows-page GET.
  - `batch.hpp`: `BatchSource::follows`, and a follows summary for `batch_to_json`.
  - `error.hpp`, `exit_codes.hpp`.
- **App:**
  - `cmd_follows.cpp` follows `cmd_mdlist.cpp`'s shape;
  - `cmd_login.cpp` holds login and logout;
  - `io.hpp` gains the prompter seam and the three env paths;
  - `cli.cpp`: registrations, the commands list, the extras policy;
  - `exit_help.hpp`: rows 7–9;
  - `cmd_batch_jobs.cpp`: the follows prefix label `follows`.

## Hermetic test story

No test ever contacts MangaDex. Fixture mode refuses network; non-official `--api-base` is refused for these commands; the official API is never used in CI.

1. **Unit (FakeTransport + FakeClock):**
   - scripted token, refresh, revoke and follows exchanges;
   - assert exact form bodies (passwords containing `& = + % space é`);
   - assert the Authorization header only on follows URLs and that **no recorded request to another URL carries it**;
   - rotation, expiry-before-page, 401-then-retry, invalid_grant/invalid_client, 429 with Retry-After, 5xx, malformed answers, the 3-POST cap.
2. **Fixture transport extension** (`file://<root>/…`):
   - POST to `<root>/auth/realms/mangadex/protocol/openid-connect/token`: parse the form (bounded). `grant_type=password` serves `<root>/auth/password/<username>.json`; `refresh_token` serves `<root>/auth/refresh/<token>.json`; `/revoke` serves `<root>/auth/revoke/<token>.json`. Missing file → 400 `invalid_grant`.
   - Each file is `{"expect": {client_id, client_secret, password?}, "status": 200, "body": {...}}`. A mismatched `expect` field → 401 `invalid_client` or 400 `invalid_grant`.
   - Name components must match `[A-Za-z0-9._-]{1,128}` and not be `.` or `..`.
   - Other POSTs (MD@H) keep today's 404.
   - GETs under `api/user/` require `authorization` = a token T with `<root>/auth/access/<T>.json` present. It may say `{"valid_requests": n}`, counted in memory under a mutex, after which the fixture answers 401 with a JSON error body. Pagination maps `offset=` to `offset-<n>.json` as today.
   - **Any request with an authorization value to a URL outside `api/user/` → `TransportErrc::local "fixture: Authorization sent to <url>"`,** so a leak fails the test loudly.
3. **Fixture data:**
   - generated by `tests/support/mdx_fixture.hpp` (`FixtureAuth`, `FixtureFollows`), with obviously fake values (`fixture-user`, `fixture-password-not-a-secret`); nothing secret-looking is committed (`diff-secret-scan`);
   - follows trees reuse the existing title trees: F1 clean; F2 clean + unknown; F3 empty; F4 duplicate; F5 a bad id; F6 a 2-page `bulk` set with a page size of 1; F7 `nochapters`.
4. **CLI (in-process `run()`):** prompts through the injected prompter (with the TTY flags in `Io`), credentials files created at mode 0600 in a temp `XDG_CONFIG_HOME`, sessions in a temp `XDG_STATE_HOME`.
5. **E2E:** the driver already gives each case a fresh `HOME`/`XDG_*` and scrubs `MANGADEX_DL_*`. A 0600 credentials file can't be committed (git keeps only 644/755), so positive E2E cases need a driver feature to create a private copy (VERIFY C9; `CliE2E.cmake` is shared). Otherwise positive auth paths stay at the `[cli]` tier and E2E covers the negative and permission paths plus `--password-stdin` (STDIN file) with a session seeded by a preceding `login` case.

## Acceptance criteria

1. `[unit]` `form_encode` keeps RFC 3986 unreserved characters and percent-encodes everything else. The password-grant body equals the exact expected string for a password `a&b=c+d%e f é`. Refresh and revoke bodies are exact.
2. `[unit]` `parse_credentials_file`:
   - accepts comments, blank lines, CRLF, `key = value` with trimming, and `=` inside a value;
   - rejects an unknown key (named), a duplicate, a line without `=`, a control byte or NUL, invalid UTF-8, over 16 KiB;
   - no error message contains a value.
3. `[unit]` `parse_token_response`:
   - accepts the access/refresh/expires_in/refresh_expires_in shape; `expires_in` absent → 900; `refresh_expires_in` 0 → null;
   - rejects a missing `access_token`, a token with CR/LF/space/`"`, a 16 385-byte token, non-JSON, over 64 KiB.
4. `[unit]` `classify_token_error`:
   - password grant `400 invalid_grant` → auth_rejected; `401 invalid_client` → auth_rejected (client wording); `unauthorized_client` → auth_rejected (approval wording);
   - refresh grant `400 invalid_grant` → session_expired (→ password fallback or login_required);
   - 429/5xx → retried, then network; other 4xx are never retried (FakeTransport sees exactly one POST).
5. `[unit]` Session round-trip: `session_to_json` → `parse_session` gives equality. Unknown `format_version`, a wrong issuer, a bad token charset, over 64 KiB → rejected with a reason that contains no token.
6. `[unit]` Token acquisition, the order and the cap:
   - valid saved access token → 0 POSTs;
   - expired access → 1 refresh;
   - refresh `invalid_grant` with a password → refresh + password (2);
   - no password → login_required;
   - a rotated refresh token is the one stored;
   - never more than 3 POSTs.
7. `[unit]` `fetch_follows` over FakeTransport:
   - URLs are `<api>/user/follows/manga?limit=100&offset=<k>` with `Authorization: Bearer <t>`;
   - pagination ends on `total` or an empty page;
   - a FakeClock advanced past expiry before page 2 → a refresh POST precedes page 2;
   - a 401 on page 2 → refresh, then page 2 retried once; a second 401 → auth_rejected;
   - total 10 001 → network with the cap message; a non-UUID id → malformed;
   - a stop → interrupted.
8. `[unit]` **No leak:** in a full FakeTransport `follows` run (auth + follows + one title's manga/feed/covers/at-home/images), every recorded request whose URL is not `/user/follows/manga` has an empty `authorization`, and only auth-host requests carry a form body with secrets.
9. `[unit]` `HttpRequest::authorization` containing CR, LF or NUL → `TransportErrc::local` before any I/O. With an empty `authorization`, the curl configuration is unchanged (GETs carry no `Authorization` header).
10. `[unit]` Fixture transport:
    - routes password, refresh and revoke POSTs as specified, and rejects `..` or `/` in names;
    - `valid_requests` counts down and then 401s;
    - authorization to a non-`api/user/` URL → local error;
    - MD@H POSTs still 404.
11. `[unit]` `BatchSource::follows` uses mdlist's "item" wording and the `follows:<n>: <uuid>: ` prefix. The JSON has batch's keys plus `follows{username,titles}` after `source`. `batch` and `mdlist` documents are byte-identical to before.
12. `[unit]` `Errc::login_required` → 8, `auth_rejected` → 9. `is_fatal` and every switch cover them.
13. `[cli]` First run with prompts:
    - stdin and stderr TTY flags set, a fake prompter answering client id, secret, user and password; fixture F1;
    - exit 0, `clean`'s paths on stdout;
    - the prompts in order, with the client hint first;
    - the session file exists at mode 0600 in a 0700 dir and contains no password;
    - a second run asks nothing and sends 0 or 1 token POSTs.
14. `[cli]` No TTY, no session, no credentials → exit 8 with the login message, empty stdout, no JSON (also with `--json`), DIR not created, no request sent.
15. `[cli]` `--no-input` with TTY flags set and no session → exit 8, and the prompter is never called.
16. `[cli]` A credentials file at 0644 → exit 1 with the `chmod 600` hint. A 0600 file with all four keys → exit 0 with no prompts. An explicit missing `--credentials` → exit 1.
17. `[cli]` Wrong password (fixture `expect` mismatch) → exit 9 with the password message. Wrong secret → 9 with the client/approval message. The session is unchanged.
18. `[cli]` A session whose refresh is answered `invalid_grant`:
    - with a password in the credentials file → silent password login, exit 0, session rewritten;
    - without one → exit 8 "expired".
19. `[cli]` Usage: `follows ''`, `follows <uuid>`, `--title X`, `--password hunter2`, `--password=hunter2`, `follows out hunter2`, `--api-base https://example.org`, `--password-stdin` with empty stdin → exit 2, empty stdout, no request. **`hunter2` appears nowhere on stderr.**
20. `[cli]` `-v` prints `logged in to MangaDex as fixture-user (…)`, `follows: n titles`, `follows:1: <uuid>`. `-vv` traces show the token URL and the follows URLs. **Neither stdout nor stderr contains the fixture access token, refresh token, secret or password** (grep for each).
21. `[cli]` A simulated interrupt during the follows fetch → no JSON, nothing in DIR, the session as last written. An interrupt at the password prompt → the interrupted path, the prompter's echo-restore hook was called, no session written.
22. `[cli]` Session save fails (state dir is a regular file) → warning, the run still exits 0. `login` in the same situation → exit 1.
23. `[cli]` Two sessions in one test: a fixture login never touches `session.json` (only `fixture-session.json`).
24. `[cli]` `login` with the fake prompter → 0, empty stdout, the "logged in … saved in" line. `logout` → the revoke POST sent, the file removed, 0. `logout` again → "not logged in", 0. A failing revoke → warning, file removed, 0.
25. `[cli]` A world-writable state dir or a symlinked `session.json` → warning, the session is not used and not written through the link.
26. `[cli]` A lock held by another fd → waits, then exits 1 after the (test-shortened) timeout. Released → proceeds.
27. `[e2e]` Help goldens: new `follows.help`, `login.help`, `logout.help`; re-bless requested for `download.help`, `batch.help`, `mdlist.help`, `edge.help` and root help (exit rows 7–9, the commands list). No `--api-base` in any help.
28. `[e2e]` No session, stdin `/dev/null` → exit 8, exact message, empty stdout, `out` absent.
29. `[e2e]` A committed 0644 credentials file via `--credentials {{INPUTS}}/follows/credentials-0644` → exit 1 with the chmod hint, value-free.
30. `[e2e]` Positive path (needs the driver feature from VERIFY C9, or a preceding `login` case seeding the session):
    - F1 → 0, the paths, sound EPUBs, no `.part-`;
    - `--json` has `source: "follows"` and the `follows` object;
    - `--dir-per-title` puts files under `out/Clean Title/`.
31. `[e2e]` F2 → 4 with `follows:2: 6666…: … not found`. F7 → 7. F3 → 0 with the "follow no titles" warning and `titles: []`. F4 → the duplicate warning.
32. `[e2e]` `--password-stdin` via `STDIN follows/password.txt` with a fixture credentials setup → 0. `STDIN NULL` → 2.
33. `[e2e]` `--dry-run` → planned paths, `out` absent. Goldens `follows.dry-run` and `follows.json-dry-run` requested.
34. `[edge]` `STDIN CLOSED` with a seeded session → 0; without one → 8; never 70, never a hang.
35. `[edge]` `--dry-run > /dev/full` → 1 plus `mangadex-dl: write error`.
36. `[edge]` F6 `--dry-run --layout chapters | head -1` → 141, also with the parent ignoring SIGPIPE.
37. `[edge]` `--json` with `CLICOLOR_FORCE=1` → no ANSI. Piped stderr with a failing item → no ANSI.
38. `[edge]` Invalid-UTF-8 DIR under `LC_ALL=C` → verbatim; `--json --dry-run` → valid JSON. Invalid-UTF-8 extra arg after `follows` → exit 2, not echoed, not 70.
39. `[tty-pty]` (cli-tester, `script` with piped keystrokes) Under a pty:
    - the client hint and four prompts appear on the terminal;
    - the secret and password are **not echoed**;
    - after the run `stty -a` shows `echo` restored;
    - progress `(1/2) …` appears after the prompts; none with `--json`/`-q`/`TERM=dumb`.
40. `[tty-pty]` Ctrl-C (SIGINT via the pty) at the password prompt → 130, echo restored, no session file.
41. `[human]` Real login with the user's own personal client: `login`, then `follows ~/Manga --dry-run`, then a real run. The count of follows matches the website, and the order is noted (closes B1, B2, B5).
42. `[human]` One deliberate wrong password (only once: brute-force lockout) → exit 9 with sensible wording. Record the raw `error`/`error_description` with `-v` (closes A5).
43. `[human]` If available, a not-yet-approved client → record what MangaDex answers and check the message mentions approval (closes A5's pending case).
44. `[human]` Run `follows` again after more than 15 minutes, then after several days: silent refresh, a rotated token saved (compare `refresh_expires_at`). Note `refresh_expires_in` (closes A3, A4).
45. `[human]` `logout`, then `follows` from cron (no TTY) → exit 8 with the login hint. Then `login` in a terminal and cron works again.
46. `[human]` A large follow count (100+), possibly more than 500: pagination complete, no 429 storm on the auth host, an authenticated request only at the start (check `-vv`).
47. `[human]` Typing at the real prompts in the user's terminal emulator: no echo, paste works, Ctrl-C and Ctrl-D behave. Ctrl-Z then `fg` at the password prompt: note whether echo comes back (a known termios caveat, documented if not).
48. `[human]` Q4 check: compare `follows` with the website's Library/Follows views; a titles-with-status-but-not-followed case decides whether `--status` is wanted.
49. `[human]` `ps -ef`/`/proc/<pid>/cmdline` during a run shows no secret; a deliberate crash (if one can be provoked) leaves no core with secrets (closes C7 in practice).

## Out of scope

- Env-var credentials (Q1-D, additive later).
- `whoami`/`status`; `--status` reading-status filters (Q4-C later).
- Authenticated `mdlist` for private lists (additive and opt-in later; the anonymous private-list 404 → exit 4 is now confirmed live and stays).
- Public clients, authorization-code and device-code flows (device_code appears in the realm's grants; unknown for personal clients).
- MFA. Personal clients bypass it, per MangaDex; the README says so.
- Following/unfollowing, marking chapters read, `/user/follows/manga/feed`.
- Several accounts per run; storing secrets in a system keyring; parallel titles; man page and completions.

## Open questions

The gate questions are Q1–Q6 at the top.

**[VERIFY] items, clustered for fact-checkers.** The fact-checker must not POST to `auth.mangadex.org` or send any credential, real or fake. Live shapes are closed by the human ACs.

**A. MangaDex auth (Keycloak):**
- **A1:** Token endpoint path. CONFIRMED by the discovery document and the docs. Note that mangadex-full-api comments that "the documented refresh URL" didn't work for it; both grants use `/realms/mangadex/protocol/openid-connect/token`.
- **A2:** `client_secret_post` with form fields for both grants. CONFIRMED (docs and discovery `token_endpoint_auth_methods_supported`).
- **A3:** Response fields `expires_in`, `refresh_expires_in`, `token_type`, and whether the refresh grant rotates `refresh_token`. Third-party code only; AC44.
- **A4:** Lifetimes: access 15 min (docs). The refresh-token lifetime is unknown (third-party says about 1 month); whether `refresh_expires_in: 0` occurs. AC44.
- **A5:** Error shapes and statuses for: a wrong password (expected 401 or 400 `invalid_grant` "Invalid user credentials"), a wrong secret (401 `invalid_client`), a pending or unapproved client, a disabled account or brute-force lockout, an expired or revoked refresh token ("Token is not active"/"Session not active"). AC42–43.
- **A6:** Rate limits and 429 headers on `auth.mangadex.org`. Only the legacy `/auth/login` 30/h and `/auth/refresh` 60/h are documented.
- **A7:** The revocation endpoint accepts `token`+`token_type_hint=refresh_token`+client credentials and answers 200. The endpoint is CONFIRMED in discovery; the behaviour is not.
- **A8:** The token charset is JWT/base64url and the size is far below 16 KiB.

**B. MangaDex API, follows:**
- **B1:** `/user/follows/manga` response: `MangaList` with `total`, `limit` ≤ 100 (CONFIRMED by the spec); the 401 body shape for missing or expired tokens.
- **B2:** The follows order (follow date? stable across pages?); whether content-rating filters apply (are pornographic follows returned without `contentRating[]`?).
- **B3:** Whether the 10 000 offset window applies to this endpoint.
- **B4:** Every followed title is downloadable anonymously (no title needs auth for `/manga/{id}`, the feed or at-home).
- **B5:** The semantics of follows vs `/manga/status` vs the website Library (gate Q4).
- **B6:** A 403 on the follows endpoint: ban vs permission body.

**C. Repo and toolchain:**
- **C1:** In the installed CLI11 2.7.x, `ExtrasError::what()` lists the extra arguments verbatim, and `remaining(true)` shows them once `follows`/`login`/`logout` is reached (so the value-free extras policy is needed and implementable).
- **C2:** The shared option binders register on a fourth subcommand without `ConstructionError`; help ordering matches `mdlist`'s with the three new flags last.
- **C3:** The curl transport can attach an `Authorization` header to GETs (new slist path); `CURLOPT_FOLLOWLOCATION 0` stays; libcurl never logs headers without `CURLOPT_VERBOSE`.
- **C4:** Fixture transport: POST-form routing; a thread-safe per-token counter.
- **C5:** `ScopedInterruptHandlers` installs with `SA_RESTART`, so the prompt reader must poll; confirm the pending-interrupt query API in `interrupt.hpp`.
- **C6:** `explicit_bzero`, `flock`, `mkostemp`, `renameat`, `openat` in the conda sysroot (glibc 2.28 headers).
- **C7:** `prctl(PR_SET_DUMPABLE, 0)` under the dev preset's ASan/LSan (LSan's StopTheWorld uses ptrace; probe with `cxx-probe` in the real context). If it breaks LSan: `RLIMIT_CORE=0`, and check systemd-coredump honours it.
- **C8:** termios `~ECHO` with `TCSAFLUSH` on fd 0 and restore on every path, including exceptions.
- **C9:** The E2E driver (`tests/e2e/CliE2E.cmake`, shared) can't create a 0600 file from a committed input; decide between a driver option (serialization point) and the CLI-tier-only positive path.
- **C10:** `EnvSnapshot`/`read_env` gains three strings (no longer trivially `noexcept`); `io.hpp` is shared.
- **C11:** `-Wswitch` finds every `switch` over `Errc` and `BatchSource`.
- **C12:** `with_remedy` must not append the network remedy to auth-rejected, login-required or content-failure messages.

**Contract changes outside `follows`/`login`/`logout`** (everything else in `download`, `batch` and `mdlist` stays byte-identical):
- the shared help footer: row 7 reworded, rows 8 and 9 added (every help golden);
- the root `unknown command` list;
- the root help command list.

**Relevant paths:**
- `/home/user/Mangadex-dl/docs/design/list-spec.md`
- `/home/user/Mangadex-dl/src/app/cmd_mdlist.cpp`
- `/home/user/Mangadex-dl/src/app/cmd_batch_jobs.hpp`
- `/home/user/Mangadex-dl/src/app/cli.cpp`
- `/home/user/Mangadex-dl/src/app/exit_help.hpp`
- `/home/user/Mangadex-dl/src/app/io.hpp`, `/home/user/Mangadex-dl/src/app/io.cpp`
- `/home/user/Mangadex-dl/include/mangadex_dl/exit_codes.hpp`, `/home/user/Mangadex-dl/include/mangadex_dl/error.hpp`
- `/home/user/Mangadex-dl/include/mangadex_dl/api_client.hpp`, `/home/user/Mangadex-dl/src/core/api_client.cpp`
- `/home/user/Mangadex-dl/include/mangadex_dl/net/transport.hpp`, `/home/user/Mangadex-dl/src/core/net/curl_transport.cpp`, `/home/user/Mangadex-dl/src/core/net/fixture_transport.cpp`
- `/home/user/Mangadex-dl/include/mangadex_dl/mdlist.hpp`, `/home/user/Mangadex-dl/include/mangadex_dl/batch.hpp`
- `/home/user/Mangadex-dl/include/mangadex_dl/fs/bounded_read.hpp`
- `/home/user/Mangadex-dl/src/core/interrupt.cpp`
- `/home/user/Mangadex-dl/tests/e2e/run_case.cmake`, `/home/user/Mangadex-dl/tests/e2e/CliE2E.cmake`
- `/home/user/Mangadex-dl/tests/support/mdx_fixture.hpp`, `/home/user/Mangadex-dl/tests/support/fake_transport.hpp`

**Sources** (anonymous reads only):
- [MangaDex docs: Authentication](https://api.mangadex.org/docs/02-authentication/)
- [MangaDex docs: Personal clients](https://api.mangadex.org/docs/02-authentication/personal-clients/)
- [MangaDex docs: Rate limits](https://api.mangadex.org/docs/2-limitations/)
- [MangaDex OpenAPI spec](https://api.mangadex.org/docs/static/api.yaml)
- [auth.mangadex.org OpenID discovery](https://auth.mangadex.org/realms/mangadex/.well-known/openid-configuration)
- [mangadex-full-api PersonalAuthClient.ts (third-party)](https://cdn.jsdelivr.net/npm/mangadex-full-api@6.2.0/src/shared/PersonalAuthClient.ts)
- [AsyncDex authentication docs (third-party, refresh-lifetime claim)](https://asyncdex.readthedocs.io/en/latest/authentication.html)