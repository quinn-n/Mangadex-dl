# Feature spec: `mangadex-dl follows`, `login`, `logout`: downloading the titles you follow or have in your Library

**Status:** rev1, 2026-10-28.
- **Draft:** DRAFT 2026-10-28 by cli-designer. Step 2 of "download the works I follow". Step 1 (`mdlist`) is merged; see [`list-spec.md`](list-spec.md).
- **rev1:** gate decisions recorded (user, 2026-10-28):
  - **Q1 = C:** TTY prompt, plus an optional private credentials file, plus `--password-stdin`. Env vars are deferred.
  - **Q2 = B:** persist a session, never the password.
  - **Q3 = B:** top-level `follows`, `login` and `logout`.
  - **Q4 = "both now":** follows by default, plus a reading-status filter in this change, specified as amendment **A1** (`--status`).
  - **Q5:** two exit codes, 8 `login_required` and 9 `auth_rejected`.
  - **Q6:** flat by default; `--dir-per-title` opts in, as in `mdlist`.
  - Changes A1 forces elsewhere are marked **A1**.
- **Fact folded in (coordinator, from the user's live test):** an anonymous `GET /list/{id}` for a private list answers **404**, and `mdlist` exited 4. This **closes list-spec V1**. It matters only for the deferred "auth on `mdlist`" item.

Three commands are designed here, in separate subsections:
- **`follows`** is the feature. With **A1** `--status`, it reads the Library by reading status instead of the follows.
- **`login`** and **`logout`** are small helpers.

**Intent:**
- Log in to MangaDex with the user's personal API client.
- Collect the titles: by default page through `GET /user/follows/manga`; with **A1** `--status`, read `GET /manga/status` and keep the titles with the requested statuses.
- Run every title through the unchanged batch engine. This is exactly `mdlist`'s path, with an authenticated collection in place of `GET /list/{id}`.

**Who runs it:**
- A person at a terminal. The first run asks for the login details once and saves the login.
- After that, mostly cron or a script running the same command unattended, to pick up new chapters and new titles.

**Assumptions:**
- One account per run and one output root, DIR. The run-level options are `mdlist`'s, byte for byte.
- **Only the collection fetch is authenticated:** the follows pages, or with A1 the status map. Every per-title request stays anonymous, exactly as today: `/manga/{id}`, the feed, covers, at-home, images and MD@H reports. The reasons:
  - MangaDex's auth page says to send auth headers only when a call needs them, because authenticated requests can't be cached.
  - It also says never to send them to any host other than `api.mangadex.org` or `auth.mangadex.org`.
  - The 15-minute access-token lifetime then never meets the long download phase (see "Token lifetime").
- Titles run one after another, and `--jobs` parallelises images within a title. No new threads.

**What was confirmed without credentials.** These were anonymous reads only: nothing was POSTed and no credentials were sent.
- **Personal-clients docs:**
  - The token endpoint is `POST https://auth.mangadex.org/realms/mangadex/protocol/openid-connect/token`, sent as `application/x-www-form-urlencoded`. The docs say it is a form, "NOT a Json payload".
  - Password grant: `grant_type=password&username&password&client_id&client_secret`.
  - Refresh grant: `grant_type=refresh_token&refresh_token&client_id&client_secret`. **The refresh needs the client secret too.**
  - The access token lasts 15 minutes.
  - A client may be "pending … requiring manual approval", and then it "cannot be used yet".
- **Realm discovery document** (`/.well-known/openid-configuration`, a public GET):
  - issuer `https://auth.mangadex.org/realms/mangadex`;
  - `revocation_endpoint …/protocol/openid-connect/revoke` and `end_session_endpoint …/logout`;
  - `client_secret_post` is among the supported auth methods, and `password` and `refresh_token` are among the grants.
- **API spec:**
  - `GET /user/follows/manga`: `limit` 1–100 (default 10), `offset`, `includes[]`; it returns a `MangaList`.
  - `GET /manga/status`: an optional `status` query, "Used to filter the list by given status". It returns `{result, statuses: {<manga id>: <status>}}`, with the values `reading`, `on_hold`, `plan_to_read`, `dropped`, `re_reading` and `completed`. It has no pagination parameters.
- **Rate-limit page:**
  - About 5 req/s per IP globally; past that, 429s, then a temporary 403 IP ban.
  - It lists only the *legacy* `POST /auth/login` (30 per 60 min) and `/auth/refresh` (60 per 60 min). **The Keycloak endpoint's limits are undocumented.**
- **Not confirmed (third-party code only):**
  - the `expires_in` and `refresh_expires_in` fields, and refresh-token rotation (mangadex-full-api stores the new `refresh_token` it gets from each refresh);
  - "refresh token lasts about one month" (asyncdex docs).

## Decisions at the design gate (user, 2026-10-28)

- **Q1, credential sources: C.**
  - Credentials come from a TTY prompt, an optional private credentials file, or `--password-stdin`. None of them ever goes on argv.
  - Env vars are deferred. If they are added later, they rank above the credentials file, and the tool applies that order itself.
- **Q2, persistence: B.**
  - The session lives in `$XDG_STATE_HOME/mangadex-dl/session.json`: mode 0600, inside a 0700 directory, written atomically.
  - It holds the client_id, client_secret, username, refresh token, access token and expiry times. **It never holds the password.**
  - It is rewritten after every token response, so a rotated refresh token is never lost.
- **Q3, command surface: B.**
  - Top-level `follows`, `login` and `logout`. `whoami`/`status` are deferred.
  - Auth on `mdlist` (reading your own private lists) is deferred. It is additive later and must be opt-in; anonymous private list → 404 → exit 4 is now confirmed live and stays.
- **Q4, what is downloaded: "both now".**
  - By default, `GET /user/follows/manga`.
  - With **A1** `--status`, the Library titles whose reading status is in the given set (`GET /manga/status`).
- **Q5, exit codes:** two codes, **8 `login_required`** and **9 `auth_rejected`**.
- **Q6, `--dir-per-title`:** off by default, opt-in, with the same meaning as on `mdlist`.

---

## Summary

`mangadex-dl follows [DIR]` logs in to MangaDex as the owner of a personal API client and collects titles:
- **by default**, the account's followed titles (paginated, at most 10 000);
- **with `--status reading,on_hold,…`** (A1), every title in the account's Library whose reading status is one of those given, whether or not it is followed.

It then downloads each title into DIR through the same engine as `mdlist`: the same options, stop rules, stderr wording ("item"), exit aggregation and `--json` document.
- Only the collection request carries the access token. Downloads are anonymous.
- The first interactive run asks for the client id, client secret, username and password, with a hint on where to create a client. It logs in and saves a session; the password is never saved.
- Later runs, cron included, refresh that session silently.
- `mangadex-dl login` sets up the session without downloading.
- `mangadex-dl logout` revokes the session at MangaDex (best effort) and deletes it.

Users: a person at a terminal first, then cron and scripts.

**Friction audit.** The primary task, `mangadex-dl follows ~/Manga`, takes one positional and no flags.
- The first time on a TTY: four prompts, once.
- Afterwards: nothing to type until MangaDex ends the session.
- Cron: nothing, after one `login`.
- The Library variant adds one flag: `--status reading`.

## Command surface (CLI11)

**Files:**
- App: `src/app/cmd_follows.{hpp,cpp}` (`register_follows`), and `src/app/cmd_login.{hpp,cpp}` (`register_login`, `register_logout`).
- Three registration lines in `cli.cpp`, which is a **serialization point**.
- Core: `include/mangadex_dl/auth.hpp`, `include/mangadex_dl/follows.hpp`, `src/core/auth*.cpp` and `src/core/follows*.cpp`. **A1:** the status-map parser and selection live in `follows.hpp`.

### `follows`

- **Invocation:** `mangadex-dl follows [OPTIONS] [DIR]`.
- **Positional:** `DIR` (optional, default `.`), "Output directory (created if missing)". Byte-identical to `mdlist`.

**Options.** Exactly `mdlist`'s set, bound by the same helpers so the help strings are byte-identical: `bind_selection_options`, `bind_run_options` (including the hidden `--api-base`), `--fail-fast` and `--dir-per-title`.

New options; the help texts fit the 44-column right column:
- `--credentials FILE`: "Read login details from FILE".
  - It is a path, not a secret.
  - No `CLI::ExistingFile`: a missing file is reported when the file is opened (TOCTOU), with exit 1.
  - No `capture_default_str`: the default depends on the environment, and the help must not. No `envname`.
- `--password-stdin` (flag): "Read the password from stdin". It is read once, after parsing and before any network use.
- `--no-input` (flag): "Never prompt; fail if a login is needed".
- **A1:** `--status WORD,...`: "Library titles with these reading statuses" (42 columns).
  - **Values:** a `std::vector<std::string>`, repeatable, and comma-separated inside one value (`->delimiter(',')`, VERIFY D7). `--status reading,on_hold` is the same as `--status reading --status on_hold`.
  - It is validated after parsing, not with `IsMember`, because of the comma split and the aliases. The help column shows `WORD,...`, never a default.
  - **Words:** the six API values `reading`, `on_hold`, `plan_to_read`, `dropped`, `re_reading` and `completed`. The kebab spellings `on-hold`, `plan-to-read` and `re-reading` are accepted too and normalised to the API's underscore form, so a kebab-case CLI doesn't trip users. Exact lower case only.
  - **Plus `all`:** every Library title, whatever its status. Unknown future status values count too.
    - **Why offer `all`:** "download my whole Library" is the most likely Library request. Spelling out six words is friction, and `all` also covers a status MangaDex adds later.
    - `all` together with other words is accepted, and `all` absorbs them (no warning). Repeated words are de-duplicated, keeping the first occurrence.
  - **Absent** means the follows source (the default). There is no `--status follows` value: follows are not a reading status.
  - Name considered and rejected: `--reading-status`. It is precise but long; the help line, the footer and the README all say "reading status", so the short form is unambiguous in context.
  - **Not offered on `login` or `logout`.**

**Not offered, on purpose:** `--password`, `--client-secret`, `--username`, `--client-id` and `--title`. See "Semantic usage checks".

**Global flags** (`--json`, `-q`, `-v`, `--color`) work anywhere, through the existing `fallthrough()`. `--version` has no `-v` alias.

**Prompts** happen only when all of these hold:
- stdin and stderr are both TTYs;
- `--no-input` is absent;
- `--password-stdin` is absent (stdin is spoken for).

Otherwise a missing value is **exit 8** with the remedy. This deliberately deviates from cli-design's "a needed prompt with `--no-input` is exit 2": the invocation is valid; what is missing is state.

**Semantic usage checks.** Each one gives exit 2, prints nothing on stdout, opens no file descriptor and sends no request. Each ends with `Run 'mangadex-dl follows --help' for more information.`
- `DIR must not be empty`.
- `DIR looks like a MangaDex id; follows takes no title or list (use 'mangadex-dl mdlist <list>' or 'download <title>')`.
- Every run option is validated as in `mdlist` (`detail::validate_common`).
- **A1:** `--status: unknown reading status '<word>'; use reading, on_hold, plan_to_read, dropped, re_reading, completed or all`. The word is passed through `terminal_safe` and cut to 32 bytes; it comes from argv and is not a secret.
- **A1:** an empty word (`--status ''`, `--status reading,`, `--status ,`) gives `--status needs a reading status (reading, on_hold, …, or all)`.
- An `--api-base` that is neither the official API nor a `file://` fixture: `follows logs in only to MangaDex itself; --api-base must be the official API`. This keeps credentials from ever reaching a custom host.
- `--password-stdin` with an empty stdin (EOF before any byte, `/dev/null`, or closed), more than 4096 bytes, a NUL byte, or an empty first line: `--password-stdin was given, but stdin held no password`.
- `--title` or `--title=X`: `follows has no --title option; …`. This generalises `title_option_owner`.
- **Secret-shaped extras:**
  - `--password`, `--password=…`, `--client-secret…` and `-p` give `follows has no --password option: passwords and secrets never go on the command line (other users can see it); use the prompt, --password-stdin or the credentials file`.
  - **For `follows`, `login` and `logout`, no ExtrasError ever echoes an argument's value or a bare word.** Only option names are echoed, cut at `=`. CLI11's default ExtrasError text lists the extras verbatim (VERIFY C1), and `follows out hunter2` must not print `hunter2`.

**Root:**
- The unknown-command line becomes `unknown command 'x' (commands: download, batch, mdlist, follows, login, logout)`.
- Root `--help` lists the three new one-liners.

**Help draft for `follows --help`** (for cli-ux; lines ≤ 80):
```
Download every title you follow on MangaDex (needs a login)


mangadex-dl follows [OPTIONS] [DIR]


POSITIONALS:
  DIR [.]                           Output directory (created if missing)

OPTIONS:
  ... exactly as mdlist, including --fail-fast and --dir-per-title ...
      --status WORD,...             Library titles with these reading statuses
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

READING STATUS:
  Without --status, the titles you follow are downloaded. With --status,
  the titles in your Library with one of the given reading statuses are
  downloaded instead, whether or not you follow them: reading, on_hold,
  plan_to_read, dropped, re_reading, completed, or all (the whole Library).
  Give several separated by commas, or repeat --status. They run in the
  order given, each status's titles in a fixed order.

  Only the list of titles is fetched with your login; the titles
  themselves are downloaded anonymously, one after another, as with mdlist.

Examples:
  Log in once, then download everything you follow into ~/Manga:
    mangadex-dl login
    mangadex-dl follows ~/Manga
  Everything you are reading or plan to read, one folder per title:
    mangadex-dl follows ~/Manga --status reading,plan_to_read --dir-per-title
  In a script, with the password from a password manager:
    pass show mangadex | mangadex-dl follows ~/Manga --password-stdin
  Show what would be written, without downloading anything:
    mangadex-dl follows ~/Manga --dry-run

Running the same command again skips files that already hold every chapter,
rebuilds files MangaDex has added chapters to, and downloads titles you have
added since the last run.

Symlinks inside DIR are followed: don't point DIR at a directory that others
can write to. Each title costs several API requests: many titles make a long
run.

Exit status:
  ... exit_status_footer() ...
```

### `login`

- **Invocation:** `mangadex-dl login [OPTIONS]`. One-liner: "Log in to MangaDex and save the login for follows".
- **Options:** `--credentials FILE`, `--password-stdin`, `--no-input` and the hidden `--api-base`, with the same rules and help strings as on `follows`. No positionals and no `--status`.
- **Behaviour:** always a **password grant**, even when a session exists. This is how the user renews or switches the login.
  - Values are gathered in the Q1 order.
  - The password prompt shows the username: `MangaDex password for <username>: `.
- **Output:**
  - stdout is always empty: there is no data, also with `--json`, and this is documented.
  - stderr, not with `-q`: `mangadex-dl: logged in to MangaDex as <username>; the login is saved in <path>`.

### `logout`

- **Invocation:** `mangadex-dl logout`. One-liner: "Remove the saved MangaDex login".
- **Options:** the hidden `--api-base` only, for fixture tests. No prompts. stdout is always empty.
- **Behaviour:**
  1. Take the session lock and read the session.
  2. **Best effort:** POST the RFC 7009 revocation (`token=<refresh>&token_type_hint=refresh_token&client_id&client_secret`) to the discovery document's `revocation_endpoint`. The request gets 5 s connect and stall limits and no retries; the answer is ignored except for success or failure.
  3. Then `unlinkat` the session file.
- **Messages:**
  - `logged out; removed <path>`.
  - `not logged in (no saved login at <path>)`: exit 0 (idempotent).
  - A failed revoke gives the warning `could not end the session at MangaDex (<reason>); the saved login was removed anyway`, and exit 0.
- `logout` never reads or touches the credentials file. The README says "delete it yourself if it holds your password".

## Input & output

**Sources and precedence.**
- There is no CLI11 config file and no `envname` anywhere. CLI11's order (CLI > config > env > default) has nothing to arbitrate, and the CLI-level options (A1's `--status` included) keep "command line > built-in default".
- No credential value passes through CLI11. The tool applies its own order, per field:
  - client_id, client_secret, username: **credentials file > saved session > TTY prompt > error (exit 8)**;
  - password: **`--password-stdin` > credentials file > TTY prompt > error (exit 8)**. It is needed only when there is no usable session.
- If env vars are added later, they rank **above** the credentials file. The tool applies that itself; it is the opposite of CLI11's config-beats-env order.

**Credentials file.**
- **Path:** `--credentials FILE`, else `$XDG_CONFIG_HOME/mangadex-dl/credentials`. `XDG_CONFIG_HOME` is used only if it is absolute; otherwise `$HOME/.config/…`. Without a usable `HOME` there is no default file.
- An absent default file is not an error. An absent `--credentials` file is exit 1.
- **Opening and checks:** open with `O_RDONLY|O_CLOEXEC|O_NOCTTY|O_NONBLOCK`, following symlinks (for dotfile managers). Then `fstat` the descriptor:
  - it must be a regular file;
  - `st_uid` must equal `geteuid()`;
  - `(st_mode & 077)` must be 0.
  - Failures are exit 1: `credentials file '<path>' is readable or writable by others (mode 0644); run 'chmod 600 <path>'`, `… is not owned by you`, or `… is not a regular file`.
- **Bounded read:** at most 16 KiB, with `read_bounded_fd` semantics.
- **Format:** lines of `key = value`.
  - Keys are exactly `client_id`, `client_secret`, `username` and `password`.
  - `#` at the start of a line (after optional whitespace) begins a comment. Blank lines are allowed. A final `\r` is stripped.
  - The value is the rest of the line after the first `=`, trimmed of spaces and tabs. An empty value means absent.
  - Errors are exit 1. They name the line and **never the value**:
    - an unknown key (the key is echoed only if it matches `[a-z_]{1,32}`);
    - a duplicate key;
    - a line without `=`;
    - a control byte or NUL in a value;
    - invalid UTF-8.
  - A password with leading or trailing spaces can't be written in the file. The README points to `--password-stdin` for that case.

**Prompts** (written to stderr, read from the stdin TTY):
- Before the first client prompt, one line: `mangadex-dl: reading your follows needs a MangaDex personal API client; create one at https://mangadex.org/settings (API Clients)`.
- The prompts, in order:
  - `MangaDex API client id: ` (echo on);
  - `MangaDex API client secret: ` (**echo off**);
  - `MangaDex username: ` (echo on);
  - `MangaDex password for <username>: ` (**echo off**).
- "Echo off" means termios `~ECHO` with `TCSAFLUSH`, restored by RAII on every path, then a newline on stderr.
- Each answer is read up to `\n`, at most 4096 bytes. An empty answer or EOF (Ctrl-D) is exit 8: `no <field> given`.
- Ctrl-C at a prompt restores echo, saves nothing, and exits 130. The existing handlers use `SA_RESTART`, so the reader must poll and check the pending-interrupt flag (VERIFY C5).
- In-process tests replace the real prompt through an `Io` seam (a prompter interface).

**`--password-stdin`:**
- `read_bounded_fd(io.stdin_fd, "stdin", 4096)`, read to EOF.
- The password is the first line, without its `\n` or `\r\n`.
- It is read once, and always when the flag is given, even if a session makes it unnecessary. That keeps the pipe contract predictable.

**stdin otherwise** is read only for prompts or `--password-stdin`. A closed stdin or `/dev/null` changes nothing without them; a needed prompt is then exit 8.

**stdout** is data only, exactly `mdlist`'s: one path per written or planned file, in item order. `login` and `logout` print nothing on stdout.

**stderr for `follows`:**
- **Row prefix**, after `error:` or `warning:`:
  - `follows:<n>: <manga-uuid>: `, where `<n>` is the 1-based position in the API's follows order;
  - **A1:** with `--status`, `library:<n>: <manga-uuid>: `, where `<n>` is the 1-based position in the deterministic Library order (see "Library selection"). The label differs so that a log line says which collection the position refers to.
- **Wording** is `mdlist`'s "item" wording: the closing line, duplicate, collision and dir-per-title lines. This is `BatchSource::follows`, which shares `mdlist`'s strings. The label comes from `BatchSettings::label` (`follows` or `library`).
- **Run-level lines** (no prefix):
  - Empty, follows: `warning: you follow no titles on MangaDex`.
  - Empty, **A1**: `warning: no titles in your MangaDex Library have the reading status reading, on_hold`, listing the statuses as given and normalised. For `all`: `warning: your MangaDex Library is empty`.
  - Either empty case: not shown with `-q`, exit 0, DIR not created.
  - Session not saved: `warning: could not save the MangaDex login to <path>: <reason>; the next run will need the password again`. The run continues.
  - Session ignored: `warning: ignoring the saved login at <path>: <why>` (malformed; wrong owner or mode; a different account or client than the credentials file). A fresh login follows if possible.
- **`-v`:**
  - `mangadex-dl: logged in to MangaDex as <username> (saved login|refreshed|password)`.
  - `mangadex-dl: follows: <n> titles`.
  - **A1:** `mangadex-dl: library: <k> of <m> titles have the reading status reading, on_hold`.
  - **A1:** `mangadex-dl: library: <j> titles have a reading status mangadex-dl does not know ('<value>'); only --status all includes them`. This is printed once per distinct unknown value; the value is the API's string, already validated to `[a-z_]{1,32}`.
  - Per item: `follows:<n>: <uuid>` or `library:<n>: <uuid>`, then the engine's lines.
  - The username is display-only: cut to 64 bytes on a UTF-8 boundary and passed through `terminal_safe`.
- **`-vv`:** the request trace. POST lines show only the method, URL, status and time, never the body or headers: `POST https://auth.mangadex.org/realms/mangadex/protocol/openid-connect/token -> 200 (210 ms)`. The collection trace shows `GET …/user/follows/manga?limit=100&offset=0 -> 200 (… ms)`, or for **A1** `GET …/manga/status -> 200 (… ms)`. The token travels in a header, not the URL.
- **Progress:** as in `mdlist` (TTY stderr only, never with `-q` or `--json`): `(2/57) [1/3] Vol. 1: 3/5 pages`. Prompts come before any progress line.

**`--json`** (`follows` only):
- One document at the end, printed under `mdlist`'s rule: only when the items ran and were not interrupted. **It is never printed for usage errors, auth failures (8/9), collection-fetch failures (3) or interrupts.**
- It is `ordered_json`, indent 2, with `error_handler_t::replace`, and never contains ANSI, not even with `CLICOLOR_FORCE=1`.
- **It is `batch`'s document plus one additive key, `follows`, after `source`.** `format_version` stays 1. `batch` and `mdlist` documents are byte-identical to today's.
  ```
  format_version: 1
  dry_run: bool
  source: "follows"                  the literal command word, in both modes
  follows: {                         follows only
      source: string                 A1: "follows" (default) or "library" (--status)
      statuses: [string] | null      A1: null for "follows"; for "library" the requested statuses,
                                     normalised (underscores), de-duplicated, in the order given;
                                     --status all is exactly ["all"]
      username: string               as configured (file/session/prompt), never from the API
      titles: int                    follows: the number of follows the API returned (duplicates
                                     included); library: the number of titles selected
  }
  titles: [ { line: int (position, as <n> above), manga_id, status, exit_code, error,
              duplicate_of (a position), report, dir } ]          exactly as mdlist
  summary: { titles: {...}, files: {...} }                        as batch
  ```
- **A1 key order inside `follows`:** `source`, `statuses`, `username`, `titles`. Both modes always carry all four keys, so the shape never depends on the mode; only `statuses` is null in one of them.
- Note that `titles[].status` is the item's download outcome, as in `batch`. It is unrelated to the reading status; the README says so.
- **No token, secret, password or client id ever appears in the document.**
- An empty collection prints `titles: []` with the `follows` object filled in.

**Color:** only the stderr prefixes, through `resolve_color`. Prompts are never colored.

**Locale:** positions and counts use `std::format` without `L`. Expiry times are integers (`from_chars`/`to_chars`). **A1:** the UUID ordering is a byte-wise comparison of the canonical lowercase form, never a locale collation.

## Library selection (A1: `--status`)

**Semantics.** `--status S` means **(a): every title in the account's Library whose reading status is in S, whether or not the account follows it.**
- **Why (a):** the user wants this because the Library-by-status set may differ from the follows. Option (b), filtering the follows down to those statuses, can never reach a title that has a status but isn't followed, and reaching those titles is the point.
- **The other choice:** (b) stays a reasonable reading ("my follows, but only the ones I'm reading"), so it is listed as **QA1** under Open questions for the user.
- **No union with follows:** without `--status` the follows are used; with it, the Library is used. Combining both sets is out of scope.

**Request.**
- One `GET <api>/manga/status` with **no `status` query**, carrying `Authorization: Bearer <access>`. Filtering happens locally.
- This costs one request whatever S is. It also lets `-v` report `<k> of <m>`, and it lets `all` include values the tool doesn't know.
- Rejected: one `?status=<s>` request per status. That is up to six requests, it can't express `all`, and the query semantics are unverified (VERIFY D2).
- The endpoint has no pagination parameters, so the whole map arrives in one response (VERIFY D3).

**Parsing (`parse_status_map`, pure, untrusted input):**
- `json_util::parse_untrusted` with the API defaults: 16 MiB body cap (`max_api_json_bytes`), depth 64, BOM and NUL rejected. **Plus a separator cap of 1 000 000**: three per entry at the entry cap, with headroom (the planner may tune it).
- The top level must be an object. `result` must equal `"ok"`. `statuses` must be an object.
  - An **empty JSON array `[]`** is accepted as an empty map, because PHP backends often serialise an empty map that way (VERIFY D1).
  - Anything else is malformed.
- **Entry cap:** more than **100 000** entries is refused with Errc::network: `your MangaDex Library has more than 100000 titles; refusing to truncate it`. A Library can be larger than the 10 000 titles one run may download, so this cap is separate.
- **Keys:** each must pass `is_uuid` and becomes a `MangaId` (canonical lowercase) **before** it reaches any URL or path. Otherwise the whole document is malformed (exit 3), consistent with `parse_custom_list`.
  - Two keys that canonicalise to the same id are kept as one, with the first value. A duplicate JSON key, if the parser surfaces one, is malformed (VERIFY D8).
- **Values:** each must be a string matching `[a-z_]{1,32}`; anything else (a number, null, an object, an over-long or odd string) makes the document malformed.
  - **A well-formed but unknown value is not an error:** the entry is kept with that value. It is never matched by a named status; it is matched only by `all`, and it gets the `-v` note.
  - **Why skip rather than fail:** MangaDex adding a status must not break every cron run, and an explicit filter can only name known statuses anyway.
- Messages are fixed text naming fields, never response bytes or ids.

**Selection and order (deterministic):**
1. Normalise S: map the kebab aliases, de-duplicate keeping the first occurrence. If `all` is present, S becomes "everything".
2. For a named S, emit the titles **grouped by status, in the order the statuses were given**, and within a status **in ascending byte order of the canonical UUID**.
3. For `all`, use the API's canonical status order (`reading`, `on_hold`, `plan_to_read`, `dropped`, `re_reading`, `completed`), then the unknown values in ascending byte order, each group sorted by UUID.

`<n>` in `library:<n>:` and `titles[].line` is the 1-based position in this sequence.
- **Stable across runs** for an unchanged Library, so a re-run's positions match, and the order is the one the user asked for (`--status reading,plan_to_read` downloads what they're reading first).
- **Rejected:** map order. It is the server's object order, nlohmann's `ordered_json` would preserve it, but nothing documents or guarantees it.
- **Rejected:** title order. The names are known only after each title's own request.

**Cap on the selection:** more than **10 000** selected titles (`max_list_titles`, shared with `batch` and `mdlist`) is refused with Errc::network: `<k> titles in your MangaDex Library have the reading status …; refusing to download more than 10000 in one run; pick fewer statuses with --status`. Exit 3 follows the refuse-to-truncate precedent; `with_remedy` adds nothing.

**Dedupe:** a map can't repeat an id after canonicalisation, but `mark_duplicates` still runs (with `duplicate_of`), as in `mdlist`.

**Auth:** exactly the follows rules.
- Proactive refresh if the token expires within 30 s.
- One refresh (or the password fallback) on a 401, then the request is retried once. A second 401 is exit 9.
- The 3-POST cap applies to the whole run.
- A 403 keeps the existing mapping (exit 3, possibly a ban).

## Exit codes

New codes go in `exit_codes.hpp` (a serialization point), each with a doc comment. `error.hpp` gains `Errc::login_required` and `Errc::auth_rejected`; every `switch (Errc)` must cover them, including the batch runner's `is_fatal` (VERIFY C11). **A1 adds no code.**

| Code | Name | Meaning / when |
|---|---|---|
| **8** | `login_required` | `follows`, `login`: there is no usable saved login and a missing value can't be asked for (no TTY, `--no-input`, `--password-stdin` already used stdin, an empty answer or EOF at a prompt); or no API client is configured at all; or the saved session has expired or been revoked (`invalid_grant` on refresh) and there is no password source |
| **9** | `auth_rejected` | `follows`, `login`: the auth server refused the details. That covers a wrong username or password (`invalid_grant` on the password grant), a wrong client id or secret (`invalid_client`), a client that is pending, disabled or not allowed this grant (`unauthorized_client`/`invalid_client`), and a locked account. Also: the API answered 401 to a freshly obtained access token on the follows or **A1** status endpoint |

**Messages.**
- All are fixed text and never contain a secret.
- The server's `error` code is appended only if it matches `[a-z_]{1,64}`.
- Its `error_description` is shown only with `-v`, passed through `terminal_safe` and cut to 200 bytes.

The texts:
- 8, no client: `no MangaDex API client is set up; run 'mangadex-dl login' in a terminal, or put client_id and client_secret in <credentials path>`.
- 8, login needed: `you are not logged in to MangaDex; run 'mangadex-dl login' in a terminal (or pass --password-stdin)`.
- 8, session ended: `your saved MangaDex login has expired or was ended; run 'mangadex-dl login' in a terminal`.
- 9, password: `MangaDex rejected the username or password for <username> (invalid_grant); check them, or log in on the website to see whether the account is locked`.
- 9, client: `MangaDex rejected the API client (invalid_client); check client_id and client_secret, and whether the client is approved at https://mangadex.org/settings`.
- 9, fresh token refused: `MangaDex rejected a new access token for your follows (HTTP 401); check that the API client is approved and allowed to read your account`. **A1:** with `--status`, "your follows" becomes "your Library".

**Every outcome:**

| Outcome | Code |
|---|---|
| all items ok or duplicate; an empty collection; `login` succeeded; `logout` (also when not logged in, or when the revoke failed) | 0 |
| credentials file unreadable, insecure, malformed or (explicit path) missing; session lock not acquired within 60 s; `login` could not save the session; `logout` could not remove it; backend unusable (no libcurl); DIR unusable or another global local error (the rest become `not_run`); stdout write error | 1 |
| every `ParseError`; the DIR checks; bad option values, **A1** including bad `--status` words; a non-official, non-fixture `--api-base`; an empty `--password-stdin`; `--title` or `--password`-style extras | 2 |
| auth, follows or **A1** status endpoint: connect, DNS, TLS, timeout, 5xx or 429 retries exhausted, 403 (possibly a ban; existing message) | 3 |
| a malformed token response (missing `access_token`, bad token charset or size) | 3 |
| a malformed follows page (wrong type, a non-UUID id, over 16 MiB), or more than 10 000 follows (refused, not truncated) | 3 |
| **A1:** a malformed status map (not an object, a non-UUID key, a malformed value, over 16 MiB, over the separator cap); over 100 000 entries; more than 10 000 selected titles | 3 |
| more than 3 token requests in one run | 3 |
| items: a single failure class gives that code (1, 3, 4, 5 or 6); several give 7 | as `mdlist` |
| SIGINT or SIGTERM (also at a prompt or during auth) | 130/143 |

- **Network remedy:** auth and collection request failures get `with_remedy`'s network remedy. Content failures (malformed, over a cap) don't. This is the same split as `mdlist`'s `ListFailureKind`.
- **Help footer.** It is shared, so every help golden changes. Row 7 is reworded to fit 80 columns now that it would name three commands, and two rows are added:
  - `7   several titles failed in different ways (see per-title results)`
  - `8   follows/login: not logged in, or the saved login expired`
  - `9   follows/login: MangaDex rejected the login details`
- Also update the README's EXIT STATUS section and the `mixed_failures` doc comment.

## Behaviour & edge cases

**`follows` phases:**
1. **Validate** (pure; exit 2). **A1:** this includes normalising `--status`.
2. **Set up:** install the interrupt handlers, optionally harden against core dumps (see "Secret hygiene"), then `make_backend`.
3. **Read `--password-stdin`** if given (exit 2 if it is empty).
4. **Credentials:** read and check the credentials file.
5. **Session:**
   - Take the exclusive lock: `<state dir>/session.lock`, `flock` with `LOCK_NB`, polled every 250 ms while checking for interrupts, for at most 60 s. A timeout is exit 1: `another mangadex-dl is using the saved login; try again`.
   - Read the session: bounded to 64 KiB, `parse_untrusted`, every field validated.
   - Ignore it, with a warning, if it is malformed, has the wrong owner or mode, has another issuer, or its client_id or username differ from the credentials file.
6. **Get an access token**, with at most 3 token POSTs per run:
   1. the saved access token, if it stays valid for at least 60 s more (wall clock);
   2. else a refresh grant, if there is a refresh token;
   3. on `invalid_grant` from the refresh, or with no session at all, a password grant. The password comes from the Q1 sources, prompting for missing fields if allowed; otherwise exit 8.

   **After every successful token response, the session is written atomically before anything else happens, even before a pending interrupt is honoured,** so a rotated refresh token is never lost. A failed write is only a warning; the run continues.
7. **Fetch the collection:**
   - **Follows** (no `--status`):
     - Request `GET <api>/user/follows/manga?limit=100&offset=<k>` with `Authorization: Bearer <access>`, through the existing `paginate`: step by the number of items received, stop on an empty page or once `offset >= total`, refuse past the 10 000 window.
     - Parse each page as a `MangaList`: `data[i].type == "manga"`, `data[i].id` passes `is_uuid` and becomes a `MangaId`, attributes are ignored.
     - **Before each page**, refresh first if the token expires within 30 s.
     - **On a 401 for a page**, do one refresh (or the password fallback), then retry that page once. A second 401 is exit 9.
   - **A1, Library** (`--status`): one `GET <api>/manga/status` with the same token handling (proactive refresh, one refresh-and-retry on a 401, a second 401 is exit 9). Then parse, select and order as in "Library selection".
   - Then release the lock, and wipe and destroy every token and secret in memory.
8. **Plan:** `plan_list`-equivalent jobs (position, DIR, options, `dir_per_title`, duplicates marked). An empty collection gives the warning, the JSON if asked, and exit 0.
9. **Run** through `run_batch_jobs` with `BatchSource::follows`, the label (`follows` or `library`) and the follows summary, over the same anonymous `Backend`.

**Token lifetime during a long run:**
- The token is used only in phase 7, which is bounded:
  - follows: at most 100 pages at about 250 ms each, plus any 429 back-off;
  - **A1** Library: one request.
- Expiry is handled proactively before each request, plus one reactive retry on a 401.
- Phase 9 can take hours: titles run one after another, with `--jobs` image workers. It sends **no Authorization header at all**, so the 15-minute expiry can't bite there. Nothing refreshes during downloads, and no token is shared with worker threads.
- **Rejected:** an authenticated client per title with a shared refresher. It would need a thread-safe token cache across `FetchPool` workers, it would defeat the CDN cache for every title, and it goes against MangaDex's "send auth only when needed".

**Auth requests:**
- **Form body:** POST, percent-encoded by a pure core function. RFC 3986 unreserved characters are kept; everything else is `%XX`.
- **Token responses:**
  - The body cap is 64 KiB.
  - `access_token` and `refresh_token` must match `[A-Za-z0-9._~+/=-]{1,16384}`. This is checked before use and prevents header injection; anything else is exit 3.
  - `expires_in` must be an integer from 1 to 86400; otherwise 900 is assumed. A `refresh_expires_in` of 0, or none at all, means unknown (stored as null).
  - When the refresh response carries a new `refresh_token`, it replaces the old one (rotation).
- **Retries:**
  - Transport errors, 5xx and 429 use the existing `decide_retry` policy; 429 honours `Retry-After` and `X-RateLimit-Retry-After`.
  - **Any other 4xx is never retried**, because of Keycloak's brute-force protection.
  - The auth host gets its own limiter of 1 request per second.
- **Where the Authorization header goes:**
  - It is attached only to an **allowlist of exactly two API paths**: `/user/follows/manga` and (**A1**) `/manga/status`. Both must be on the host `api.mangadex.org` (or the fixture).
  - It is never sent anywhere else.
  - Redirects are already off (`FOLLOWLOCATION 0`), so the header can't be forwarded.

**The five things:**
1. **Success:** paths on stdout, empty stderr.
2. **Empty:** the empty-collection warning, exit 0.
3. **Usage errors:** exit 2.
4. **Runtime failures:** 1, 3, 8 or 9, as tabled.
5. **Interrupt**, always 130/143:
   - at a prompt: echo restored, nothing saved;
   - during auth: the session is either unchanged or fully rewritten (the write is atomic);
   - during the collection fetch: nothing printed, nothing created, no JSON;
   - inside the items: exactly as `mdlist`.

**Edge suite:**
- stdin `/dev/null` or closed: exit 0 with a session; exit 8 without one, never a hang; exit 2 with `--password-stdin`.
- `--dry-run > /dev/full`: exit 1 with `mangadex-dl: write error`.
- `| head -1`: exit 141, also when the parent ignores SIGPIPE.
- No ANSI when piped, or with `--json` under `CLICOLOR_FORCE=1`.
- An invalid-UTF-8 DIR under `LC_ALL=C` is used verbatim and gives valid JSON.
- An invalid-UTF-8 username in the credentials file is exit 1 (the invalid UTF-8 line), never echoed.
- An invalid-UTF-8 `--status` word under `LC_ALL=C` is exit 2, echoed only through `terminal_safe` (**A1**).
- `argc == 0` behaves as today.

**Idempotency:**
- Re-runs pick up new follows, new Library entries and new chapters.
- Files of titles that left the collection are left alone.
- **A1:** a title whose status changes moves position, and in or out of the selection. Its files stay where they are: flat, or in its `--dir-per-title` directory, whose name doesn't depend on the status.
- `login` twice replaces the session. `logout` twice exits 0 both times.
- **Concurrent runs** are serialised on the session lock, so two cron runs can't burn each other's rotated refresh tokens.

**`--dry-run`:** it still logs in and fetches the collection; it is a dry run of the downloads, not of auth. It may refresh and save the session. It writes nothing in DIR.

**Large input:**
- follows: at most 100 pages of at most 16 MiB each, processed one page at a time;
- **A1** Library: one response of at most 16 MiB and at most 100 000 entries;
- at most 10 000 ids are kept.

## Secret hygiene (`cpp-security` §9)

| Channel | Rule |
|---|---|
| argv, `ps` | No secret options exist. Secret-shaped extras get a fixed message that contains no value. ExtrasError never echoes values for these three commands |
| env | Never read for credentials (Q1 = C). `EnvSnapshot` gains only `HOME`, `XDG_CONFIG_HOME` and `XDG_STATE_HOME`, read once at startup. `read_env` is `noexcept` today and would now allocate |
| stderr, `-v`, `-vv` | Never a token, secret or password. POST traces show only method, URL and status. The username is shown (it is not a secret). Server error text is restricted as described under "Exit codes" |
| `--json` | Only `follows.username`, the source and statuses (**A1**), and the count |
| files | The session is 0600 inside a 0700 directory. The credentials file is checked for mode 0600 and ownership |
| core dumps, ptrace | **Recommended:** `prctl(PR_SET_DUMPABLE, 0)` at the start of `follows`, `login` and `logout`, before any secret is read. That gives no core file and no same-user `/proc/<pid>/mem`. Its interplay with LSan in the dev preset must be probed (VERIFY C7); the fallback is `setrlimit(RLIMIT_CORE, 0)` |
| memory | **Recommended, best effort, not a gate:** a move-only `Secret` type with no formatter and no `operator<<`, `explicit_bzero` in its destructor and on reassignment, and capacity reserved up front so no reallocation leaves copies behind. Copies inside libcurl (`COPYPOSTFIELDS`) and the TLS stack are out of reach; the planner may switch secret bodies to `CURLOPT_POSTFIELDS` over the caller-owned body |
| child processes | None are spawned |

## Data & state

**Reads:**
- the credentials file, if any;
- the session file;
- the follows pages, or **A1** the status map (`api.mangadex.org`);
- the token, refresh and revoke answers (`auth.mangadex.org`);
- existing EPUBs in DIR, as `mdlist` does.

**Writes:**
- DIR outputs, exactly as `mdlist` writes them (the atomic `OutputDir` publish).
- **Session state** under `$XDG_STATE_HOME/mangadex-dl/`. `XDG_STATE_HOME` is used only if it is absolute; otherwise `$HOME/.local/state/mangadex-dl/`.
  - Missing directories are created with mode 0700.
  - The final directory is opened with `O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC` and `fstat`ed: it must be owned by the effective uid with `(mode & 077) == 0`. Otherwise the session is neither used nor written, with a warning.
  - Files in it are reached with `openat` from that descriptor and `O_NOFOLLOW`. A symlinked session file is refused with a warning.
  - Each write:
    1. creates a temp file `mkostemp`-style (`.session.json.XXXXXX`, `O_CLOEXEC`);
    2. sets mode 0600 with `fchmod`;
    3. writes everything, then `fsync`s;
    4. checks the result of `close`;
    5. moves the file into place with `renameat`, then `fsync`s the directory.

    The temp file is unlinked on any failure.
  - `session.lock` has mode 0600 and is created with `O_NOFOLLOW`.
  - Without a usable `HOME` there is no persistence: a warning for `follows`, exit 1 for `login`.
- **Fixture mode** uses `fixture-session.json` in the same directory and stores the fixture issuer. A test run can never clobber or read a real login.

**Session file** (JSON, `format_version` 1, at most 64 KiB):
- Fields: `issuer`, `client_id`, `client_secret`, `username`, `refresh_token`, `refresh_expires_at` (UNIX seconds or null), `access_token`, `access_expires_at`.
- An unknown `format_version` is ignored with a warning. An older binary never rewrites it without a fresh login.

**Nothing else:** no cache, no TMPDIR use. The tool never creates or edits the credentials file.

**Threat model:**
- The follows pages and the **A1** status map are untrusted. Ids and status values are validated before use; names are not used.
- Token responses are untrusted; their charset and size are checked.
- The credentials file and session belong to the user but are still checked: a world-writable session could plant someone else's token.
- `--api-base` is restricted, as above.
- `security-reviewer` must review this feature: it handles secrets, files, env and untrusted input.

## Engine reuse (for the planner; names are suggestions)

**Core, new:**
- `auth.hpp`:
  - types `ClientCredentials`, `Secret`, and `Session` (a validated factory returning `Result`);
  - parsers and serialisers `parse_credentials_file`, `parse_session`/`session_to_json`, `form_encode` and `parse_token_response`;
  - `classify_token_error`, which maps to login_required, auth_rejected or network;
  - an `AuthClient` over `Transport` + `Clock` that owns the token POST, its retries and its limiter.
- `follows.hpp`:
  - for the follows source: `parse_follows_page`, `follows_request_url` and `fetch_follows` (paginate, with a token-provider callback for refresh-before-request and the 401 retry);
  - **A1** for the Library source: `ReadingStatus` (the six values, plus the raw unknown string), `StatusFilter` (a validated factory from the raw words: aliases, `all`, de-duplication, Errc::usage messages), `parse_status_map`, `select_library(map, filter) -> std::vector<MangaId>` (the order specified above, the 10 000 cap), and `fetch_status_map` (the same token-provider seam);
  - `plan_follows`.

**Core, changed** (serialization points):
- `net/transport.hpp`: `HttpRequest` gains an additive `authorization` field (the Bearer value), validated against CR, LF and NUL like `content_type`. The curl transport needs a header slist on GETs too.
- `api_client.hpp`:
  - `Endpoints` gains `auth`: officially `https://auth.mangadex.org/realms/mangadex`; in fixture mode `file://<root>/auth/realms/mangadex`.
  - `ApiClient` gains an authenticated GET restricted to the two allowlisted paths.
- `batch.hpp`: `BatchSource::follows`, and a follows summary for `batch_to_json` with `source`, `statuses`, `username` and `titles`.
- `error.hpp` and `exit_codes.hpp`.

**App:**
- `cmd_follows.cpp`, in `cmd_mdlist.cpp`'s shape, with **A1** `--status` bound to a member `std::vector<std::string>`.
- `cmd_login.cpp`, for `login` and `logout`.
- `io.hpp`: the prompter seam and the three env paths.
- `cli.cpp`: the registrations, the commands list and the extras policy.
- `exit_help.hpp`: rows 7–9.
- `cmd_batch_jobs.cpp`: the row label comes from the settings (`follows` or `library`).

## Hermetic test story

**No test ever contacts MangaDex.**
- Fixture mode refuses the network.
- A non-official `--api-base` is refused for these commands.
- The official API is never used in CI.

**1. Unit tests** (FakeTransport + FakeClock):
- Scripted exchanges for the token, refresh and revoke POSTs, the follows pages, and the **A1** status map.
- Assert exact form bodies, including passwords containing `& = + % space é`.
- Assert the Authorization header appears only on the two allowlisted URLs, and that **no recorded request to any other URL carries it**.
- Cover rotation, expiry before a request, a 401 then a retry, `invalid_grant`/`invalid_client`, a 429 with Retry-After, 5xx, malformed answers and the 3-POST cap.

**2. Fixture transport extension** (`file://<root>/…`):
- **Token POSTs** to `<root>/auth/realms/mangadex/protocol/openid-connect/token`:
  - The form is parsed, bounded.
  - `grant_type=password` serves `<root>/auth/password/<username>.json`; `refresh_token` serves `<root>/auth/refresh/<token>.json`; `/revoke` serves `<root>/auth/revoke/<token>.json`. A missing file answers 400 `invalid_grant`.
  - Each file has the shape `{"expect": {client_id, client_secret, password?}, "status": 200, "body": {...}}`. A mismatched `expect` field answers 401 `invalid_client` or 400 `invalid_grant`.
  - Name components must match `[A-Za-z0-9._-]{1,128}` and must not be `.` or `..`.
- **Other POSTs** (MD@H reports) keep today's 404.
- **Authenticated GETs.** **A1 changes the draft here:** the draft's rule was "anything under `api/user/`". It becomes an **exact allowlist**, `api/user/follows/manga` (with its `offset` pages) and `api/manga/status`, plus any other `api/user/…` URL, which always needs a token.
  - These URLs require an `authorization` value equal to a token T for which `<root>/auth/access/<T>.json` exists.
  - That file may say `{"valid_requests": n}`: a counter kept in memory under a mutex, after which the fixture answers 401 with a JSON error body.
  - `file://<root>/api/manga/status` maps to `<root>/api/manga/status.json` through the existing `.json` rule. The name `status` can't collide with a manga UUID.
  - Pagination maps `offset=` to `offset-<n>.json`, as today.
- **Leak tripwire:** a request with an authorization value to any URL outside the allowlist fails with `TransportErrc::local "fixture: Authorization sent to <url>"`. A leak fails the test loudly.

**3. Fixture data**, generated by `tests/support/mdx_fixture.hpp` (`FixtureAuth`, `FixtureFollows`, **A1** `FixtureLibrary`):
- The values are obviously fake (`fixture-user`, `fixture-password-not-a-secret`). Nothing secret-looking is committed, which keeps `diff-secret-scan` quiet.
- The follows sets reuse the existing title trees:
  - F1: clean;
  - F2: clean plus an unknown title;
  - F3: empty;
  - F4: a duplicate;
  - F5: a bad id;
  - F6: a 2-page `bulk` set with page size 1;
  - F7: `nochapters`.
- **A1** status maps:

  | Set | Tree | Content |
  |---|---|---|
  | S1 | `clean` | `{1111…: reading, 6666…: completed, 7777…: paused}` (an unknown value) |
  | S2 | `clean` | `statuses: []` (empty, the array form) |
  | S3 | `clean` | a value that is a number |
  | S4 | `clean` | a key `../x` |
  | S5 | `bulk` | 10 001 entries all `reading`, generated (the selection cap) |

  One tree serves one fixture account, so the S sets are separate fixture roots or account variants; the planner decides.

**4. CLI tests** (in-process through `run()`):
- Prompts go through the injected prompter, driven by the TTY flags in `Io`.
- Credentials files are created at mode 0600 in a temporary `XDG_CONFIG_HOME`. Sessions live in a temporary `XDG_STATE_HOME`.

**5. E2E tests:**
- The driver already gives each case a fresh `HOME` and `XDG_*`, and scrubs `MANGADEX_DL_*`.
- A 0600 credentials file can't be committed, because git keeps only modes 644 and 755. Positive E2E cases therefore need one of two things (VERIFY C9; `CliE2E.cmake` is shared):
  - a driver feature that creates a private copy;
  - or positive auth paths stay at the `[cli]` tier, and E2E covers the negative and permission paths plus `--password-stdin` (a STDIN file) with a session seeded by a preceding `login` case.

## Acceptance criteria

1. `[unit]` `form_encode` keeps the RFC 3986 unreserved characters and percent-encodes everything else. The password-grant body equals the exact expected string for the password `a&b=c+d%e f é`. The refresh and revoke bodies are exact too.
2. `[unit]` `parse_credentials_file`:
   - accepts comments, blank lines, CRLF, `key = value` with trimming, and `=` inside a value;
   - rejects an unknown key (named), a duplicate key, a line without `=`, a control byte or NUL, invalid UTF-8 and a file over 16 KiB;
   - no error message contains a value.
3. `[unit]` `parse_token_response`:
   - accepts the access, refresh, `expires_in` and `refresh_expires_in` shape;
   - with `expires_in` absent it assumes 900; a `refresh_expires_in` of 0 gives null;
   - rejects a missing `access_token`, a token containing CR, LF, a space or `"`, a 16 385-byte token, a body that isn't JSON, and a body over 64 KiB.
4. `[unit]` `classify_token_error`:
   - password grant: `400 invalid_grant` is auth_rejected; `401 invalid_client` is auth_rejected with the client wording; `unauthorized_client` is auth_rejected with the approval wording;
   - refresh grant: `400 invalid_grant` is session_expired, which leads to the password fallback or login_required;
   - 429 and 5xx are retried, then network; any other 4xx is never retried (FakeTransport sees exactly one POST).
5. `[unit]` Session round trip: `session_to_json` then `parse_session` gives an equal value. An unknown `format_version`, a wrong issuer, a bad token charset or a file over 64 KiB is rejected, with a reason that contains no token.
6. `[unit]` Token acquisition order and cap:
   - a valid saved access token: 0 POSTs;
   - an expired access token: 1 refresh;
   - a refresh answered `invalid_grant`, with a password available: a refresh plus a password grant (2 POSTs);
   - the same without a password: login_required;
   - a rotated refresh token is the one stored;
   - never more than 3 POSTs.
7. `[unit]` `fetch_follows` over FakeTransport:
   - URLs are `<api>/user/follows/manga?limit=100&offset=<k>`, each with `Authorization: Bearer <t>`;
   - pagination ends on `total` or on an empty page;
   - with the FakeClock advanced past expiry before page 2, a refresh POST comes before page 2;
   - a 401 on page 2 gives a refresh, then page 2 is retried once; a second 401 is auth_rejected;
   - a total of 10 001 is network with the cap message; a non-UUID id is malformed;
   - a stop is interrupted.
8. `[unit]` **No leak.** A full FakeTransport `follows` run covers auth, the collection and one title's manga, feed, covers, at-home and images. It runs once without `--status` and once with `--status reading` (**A1**). In both, every recorded request whose URL is not on the two-path allowlist has an empty `authorization`, and only auth-host requests carry a form body with secrets.
9. `[unit]` An `HttpRequest::authorization` containing CR, LF or NUL is `TransportErrc::local` before any I/O. With an empty `authorization`, the curl configuration is unchanged: GETs carry no `Authorization` header.
10. `[unit]` Fixture transport:
    - routes the password, refresh and revoke POSTs as specified, and rejects `..` or `/` in names;
    - `valid_requests` counts down, then answers 401;
    - authorization sent to a URL off the allowlist (for example `api/manga/<uuid>`) is a local error, while `api/manga/status` with a valid token is served (**A1**);
    - MD@H POSTs still answer 404.
11. `[unit]` `BatchSource::follows` uses mdlist's "item" wording, with the label-driven prefixes `follows:<n>: <uuid>: ` and `library:<n>: <uuid>: `.
    - The JSON has batch's keys plus `follows{source,statuses,username,titles}` after `source` (**A1** shape).
    - `statuses` is null for follows and `["reading","on_hold"]` for library.
    - `batch` and `mdlist` documents are byte-identical to before.
12. `[unit]` `Errc::login_required` maps to 8 and `auth_rejected` to 9. `is_fatal` and every switch cover them.
13. `[cli]` First run with prompts: the stdin and stderr TTY flags are set, and a fake prompter answers the client id, secret, user and password, with fixture F1.
    - exit 0, `clean`'s paths on stdout;
    - the prompts come in order, with the client hint first;
    - the session file exists at 0600 inside a 0700 directory and contains no password;
    - a second run asks nothing and sends 0 or 1 token POSTs.
14. `[cli]` No TTY, no session and no credentials:
    - exit 8 with the login message;
    - empty stdout, no JSON (also with `--json`);
    - DIR not created, no request sent.
15. `[cli]` `--no-input` with the TTY flags set and no session: exit 8, and the prompter is never called.
16. `[cli]` Credentials file permissions:
    - a file at 0644: exit 1 with the `chmod 600` hint;
    - a 0600 file with all four keys: exit 0 with no prompts;
    - an explicit `--credentials` path that is missing: exit 1.
17. `[cli]` Rejected details:
    - a wrong password (a fixture `expect` mismatch): exit 9 with the password message;
    - a wrong secret: exit 9 with the client/approval message;
    - either way, the session is unchanged.
18. `[cli]` A session whose refresh is answered `invalid_grant`:
    - with a password in the credentials file: a silent password login, exit 0, the session rewritten;
    - without one: exit 8 with the "expired" message.
19. `[cli]` Usage errors, all exit 2 with empty stdout and no request:
    - `follows ''` and `follows <uuid>`;
    - `--title X`;
    - `--password hunter2`, `--password=hunter2` and `follows out hunter2`;
    - `--api-base https://example.org`;
    - `--password-stdin` with an empty stdin.

    **`hunter2` appears nowhere on stderr.**
20. `[cli]` `-v` prints `logged in to MangaDex as fixture-user (…)`, `follows: n titles` and `follows:1: <uuid>`. `-vv` traces show the token URL and the follows URLs. **Neither stdout nor stderr contains the fixture access token, refresh token, secret or password** (grep for each).
21. `[cli]` Interrupts:
    - during the collection fetch: no JSON, nothing in DIR, the session as last written;
    - at the password prompt: the interrupted path, the prompter's echo-restore hook was called, no session written.
22. `[cli]` A failed session save (the state directory is a regular file): a warning, and the run still exits 0. `login` in the same situation exits 1.
23. `[cli]` Both session files in one test: a fixture login never touches `session.json`, only `fixture-session.json`.
24. `[cli]` `login` and `logout`:
    - `login` with the fake prompter: exit 0, empty stdout, the "logged in … saved in" line;
    - `logout`: the revoke POST is sent, the file removed, exit 0;
    - `logout` again: "not logged in", exit 0;
    - a failing revoke: a warning, the file removed, exit 0.
25. `[cli]` A world-writable state directory, or a symlinked `session.json`: a warning; the session is not used, and nothing is written through the link.
26. `[cli]` A lock held by another file descriptor: the run waits, then exits 1 after the timeout (shortened in the test). Once the lock is released, the run proceeds.
27. `[e2e]` Help goldens:
    - new `follows.help` (it includes `--status` and the READING STATUS section, **A1**), `login.help` and `logout.help`;
    - re-bless requested for `download.help`, `batch.help`, `mdlist.help`, `edge.help` and the root help (exit rows 7–9, the commands list);
    - no `--api-base` in any help.
28. `[e2e]` No session, stdin `/dev/null`: exit 8 with the exact message, empty stdout, `out` absent.
29. `[e2e]` A committed 0644 credentials file via `--credentials {{INPUTS}}/follows/credentials-0644`: exit 1 with the chmod hint, containing no value.
30. `[e2e]` The positive path (it needs the driver feature from VERIFY C9, or a preceding `login` case that seeds the session):
    - F1: exit 0, the paths, sound EPUBs, no `.part-`;
    - `--json` has `source: "follows"` and the `follows` object;
    - `--dir-per-title` puts the files under `out/Clean Title/`.
31. `[e2e]` Aggregates:
    - F2: exit 4, with `follows:2: 6666…: … not found`;
    - F7: exit 7;
    - F3: exit 0, the "follow no titles" warning and `titles: []`;
    - F4: the duplicate warning.
32. `[e2e]` `--password-stdin` via `STDIN follows/password.txt`, with a fixture credentials setup: exit 0. `STDIN NULL`: exit 2.
33. `[e2e]` `--dry-run`: the planned paths, `out` absent. Goldens `follows.dry-run` and `follows.json-dry-run` are requested.
34. `[edge]` `STDIN CLOSED`: exit 0 with a seeded session, exit 8 without one. Never 70, never a hang.
35. `[edge]` `--dry-run > /dev/full`: exit 1 with `mangadex-dl: write error`.
36. `[edge]` F6 `--dry-run --layout chapters | head -1`: exit 141, also with the parent ignoring SIGPIPE.
37. `[edge]` `--json` with `CLICOLOR_FORCE=1`: no ANSI. Piped stderr with a failing item: no ANSI.
38. `[edge]` An invalid-UTF-8 DIR under `LC_ALL=C` is used verbatim, and `--json --dry-run` gives valid JSON. An invalid-UTF-8 extra argument after `follows` is exit 2, not echoed, not 70.
39. `[tty-pty]` (cli-tester, `script` with piped keystrokes) Under a pty:
    - the client hint and the four prompts appear on the terminal;
    - the secret and the password are **not echoed**;
    - after the run, `stty -a` shows `echo` restored;
    - the progress line `(1/2) …` comes after the prompts, and there is none with `--json`, `-q` or `TERM=dumb`.
40. `[tty-pty]` Ctrl-C (SIGINT through the pty) at the password prompt: exit 130, echo restored, no session file.
41. `[human]` A real login with the user's own personal client: `login`, then `follows ~/Manga --dry-run`, then a real run. The follows count matches the website; note the order (closes B1 and B2).
42. `[human]` One deliberately wrong password, only once because of the brute-force lockout: exit 9 with sensible wording. Record the raw `error` and `error_description` with `-v` (closes A5).
43. `[human]` If available, a not-yet-approved client: record what MangaDex answers, and check that the message mentions approval (closes A5's pending case).
44. `[human]` Run `follows` again after more than 15 minutes, then after several days: the refresh is silent and a rotated token is saved (compare `refresh_expires_at`). Note `refresh_expires_in` (closes A3 and A4).
45. `[human]` `logout`, then `follows` from cron (no TTY): exit 8 with the login hint. After `login` in a terminal, cron works again.
46. `[human]` A large collection (100 or more titles, more than 500 if possible): pagination is complete, there is no 429 storm on the auth host, and `-vv` shows authenticated requests only at the start.
47. `[human]` Typing at the real prompts in the user's terminal emulator: no echo, paste works, Ctrl-C and Ctrl-D behave. Ctrl-Z then `fg` at the password prompt: note whether echo comes back (a known termios caveat, documented if not).
48. `[human]` **A1:** compare `follows` with `follows --status all --dry-run` and with the website's Library tabs. Confirm that a title with a status but no follow appears only in the Library run (closes B5 and D5 in practice, and informs QA1).
49. `[human]` `ps -ef` and `/proc/<pid>/cmdline` during a run show no secret. A deliberate crash, if one can be provoked, leaves no core file with secrets (closes C7 in practice).

**A1, `--status`:**

50. `[unit]` `StatusFilter`:
    - `reading`, `on-hold`, `plan-to-read`, `re-reading`, `on_hold,reading` and repeated flags are normalised;
    - duplicates are removed, keeping the first occurrence;
    - `all` absorbs any other words;
    - `Reading`, `paused`, `''`, `reading,` and `,` give the exact exit-2 messages;
    - an unknown word is echoed through `terminal_safe` and cut to 32 bytes.
51. `[unit]` `parse_status_map`:
    - accepts the documented shape and the empty `[]` form;
    - canonicalises an upper-case key;
    - keeps a well-formed unknown value (`paused`);
    - rejects, with Errc::network and fixed text: a body that isn't JSON, a `result` other than `ok`, missing `statuses`, `statuses` as a non-empty array or as a string, a key `../x` or a 35-character key, a numeric or null value, a 33-character value, a value with an upper-case letter or a dash, 100 001 entries, more than the separator cap, and more than 16 MiB.
52. `[unit]` `select_library` order:
    - `reading,completed` over S1 gives `[1111…(reading), 6666…(completed)]`;
    - `completed,reading` gives the reverse;
    - within a status, UUIDs are in ascending byte order;
    - `all` gives the canonical status order, then `paused`, last;
    - a named filter never selects `paused`;
    - 10 001 selected titles is network with the "pick fewer statuses" message;
    - 10 000 selected titles are accepted.
53. `[unit]` `fetch_status_map` over FakeTransport:
    - the URL is exactly `<api>/manga/status`, with no query, carrying `Authorization: Bearer <t>`;
    - an expiring token is refreshed before the request;
    - a 401 gives a refresh and one retry; a second 401 is auth_rejected with the "Library" wording;
    - 429 and 5xx are retried, then network with the remedy;
    - the 16 MiB body cap is enforced;
    - a stop is interrupted.
54. `[cli]` `--status reading` on S1:
    - exit 0, `clean`'s paths on stdout;
    - `-v` shows `library: 1 of 3 titles have the reading status reading`, the unknown-status note for `paused`, and `library:1: 1111…`;
    - `--json` has `follows.source: "library"`, `statuses: ["reading"]` and `titles: 1`;
    - no follows URL is requested.
55. `[cli]` `--status reading,completed` on S1: exit 4. Item 2 is `library:2: 6666…: … not found`, and the closing line says `1 of 2 titles did not complete (item 2)`.
56. `[cli]` `--status all` on S1: `statuses: ["all"]`, three items, with `7777…` (`paused`) last.
57. `[cli]` S2 (empty) with `--status reading`: exit 0, the warning `no titles in your MangaDex Library have the reading status reading`, `titles: []`, DIR not created. With `--status all`: `your MangaDex Library is empty`.
58. `[cli]` S3 or S4: exit 3 with the malformed message (no network remedy), no JSON, `out` absent. S5 with `--status reading`: exit 3 with the selection-cap message.
59. `[cli]` Usage: `--status paused` and `--status ''` give exit 2 with no request (the fixture root is never opened). `login --status reading` gives exit 2, because `login` has no such option.
60. `[e2e]` `--status reading` on S1 (with the seeding or driver feature from AC30): exit 0, the paths, sound EPUBs. `--status reading,completed --json`: valid JSON with the `library` source. `--dry-run --status all`: the planned paths, `out` absent.
61. `[edge]` `--status` with an invalid-UTF-8 word under `LC_ALL=C`: exit 2, the word passed through `terminal_safe`, never 70. `--json --status reading` with `CLICOLOR_FORCE=1`: no ANSI.
62. `[human]` On the user's real account: `--status reading --dry-run` lists the titles the website shows under Library → Reading (the count matches), and `--status all` matches the whole Library (closes D3 and D5).
63. `[human]` Change a title's status on the website, then run again: the title moves in or out of the selection as expected, and its files stay where they are.

## Out of scope

- Credentials from env vars (additive later, ranked above the file).
- `whoami`/`status` subcommands.
- **Combining follows and the Library** in one run (a union or intersection); see QA1.
- Per-status output directories.
- The `status` query parameter of `/manga/status`.
- Authenticated `mdlist` for private lists. It is additive and opt-in later; the anonymous private-list 404 → exit 4 is now confirmed live and stays.
- Public clients, and the authorization-code and device-code flows. The realm lists device_code among its grants, but whether personal clients may use it is unknown.
- MFA. Personal clients bypass it, per MangaDex; the README says so.
- Following or unfollowing titles, changing reading statuses, marking chapters read, and `/user/follows/manga/feed`.
- Several accounts per run; secrets in a system keyring; parallel titles; man page and completions.

## Open questions

**QA1 (for the user, from A1):** what should `--status` mean?
- **(a), as specified:** the Library titles with those statuses, whether or not they are followed.
- **(b):** the follows, filtered down to those statuses (follows ∩ status).

(b) can't reach titles that have a status but aren't followed, which is why (a) was chosen. (b) would suit someone who curates follows and uses statuses only as tags. If the user wants both, (b) could later be an additive flag. Confirm (a), or switch before planning.

**[VERIFY] items, clustered for the fact-checkers.** The fact-checker must not POST to `auth.mangadex.org` or send any credential, real or fake, and must not call the authenticated endpoints. Live shapes are closed by the human acceptance criteria.

**A. MangaDex auth (Keycloak):**
- **A1:** the token endpoint path is CONFIRMED by the discovery document and the docs. Note that mangadex-full-api says the refresh URL given in the docs didn't work for it; both grants use `/realms/mangadex/protocol/openid-connect/token`.
- **A2:** `client_secret_post` with form fields for both grants is CONFIRMED (docs, and the discovery document's `token_endpoint_auth_methods_supported`).
- **A3:** the response fields `expires_in`, `refresh_expires_in` and `token_type`, and whether the refresh grant rotates `refresh_token`. Only third-party code says so (AC44).
- **A4:** lifetimes. The access token lasts 15 minutes (docs). The refresh token's lifetime is unknown (third-party: about a month), as is whether `refresh_expires_in: 0` occurs (AC44).
- **A5:** error shapes and statuses for:
  - a wrong password (expected 401 or 400 `invalid_grant` "Invalid user credentials");
  - a wrong secret (401 `invalid_client`);
  - a pending or unapproved client;
  - a disabled account, or a brute-force lockout;
  - an expired or revoked refresh token ("Token is not active", "Session not active").

  Closed by AC42 and AC43.
- **A6:** rate limits and 429 headers on `auth.mangadex.org`. Only the legacy `/auth/login` (30 per hour) and `/auth/refresh` (60 per hour) are documented.
- **A7:** the revocation endpoint accepts `token` plus `token_type_hint=refresh_token` plus the client credentials and answers 200. The endpoint is CONFIRMED in the discovery document; its behaviour is not.
- **A8:** tokens use the JWT/base64url charset and stay far below 16 KiB.

**B. MangaDex API, follows:**
- **B1:** the `/user/follows/manga` response is a `MangaList` with `total`, and `limit` is at most 100 (CONFIRMED by the spec). The 401 body shape for a missing or expired token is not confirmed.
- **B2:** the order of follows (by follow date? stable across pages?), and whether content-rating filters apply: are pornographic follows returned without `contentRating[]`?
- **B3:** whether the 10 000 offset window applies to this endpoint.
- **B4:** every followed or Library title can be downloaded anonymously; no title needs auth for `/manga/{id}`, the feed or at-home.
- **B5:** how follows, `/manga/status` and the website's Library relate (informs QA1; AC48).
- **B6:** a 403 on the follows or status endpoint: a ban or a permission body?

**D. MangaDex API, the status map (A1):**
- **D1:** the exact `GET /manga/status` response is `{"result":"ok","statuses":{"<uuid>":"<status>",…}}`. Is an empty Library serialised as `{}` or `[]`? The design accepts both.
- **D2:** without `status`, does the endpoint return every status? With it, does it return only that status? (The design never sends it.)
- **D3:** is it really unpaginated and uncapped? How large does it get for big Libraries (the sizes behind the 16 MiB and 100 000-entry caps)? Are the ids lowercase?
- **D4:** is the status value set exactly the six spec values, with no others in practice?
- **D5:** does the map include titles of every content rating, and every title in the Library, including ones that are no longer available?
- **D6:** the 401 shape for a missing or expired token on this endpoint.
- **D7 (CLI11, installed 2.7.x):** `add_option` on a `std::vector<std::string>` with `->delimiter(',')` splits `reading,on_hold` and also accumulates repeated `--status`; how an empty element (`reading,`) arrives; and the help's type name `WORD,...` with no default shown.
- **D8 (nlohmann, installed):** what `parse_untrusted` does with duplicate object keys (last wins silently, or detectable?), and whether the parser's separator and depth guards cover a 100 000-entry object within the cap.

**C. Repo and toolchain:**
- **C1:** in the installed CLI11 2.7.x, `ExtrasError::what()` lists the extra arguments verbatim, and `remaining(true)` shows them once `follows`, `login` or `logout` was reached. That makes the value-free extras policy both needed and implementable.
- **C2:** the shared option binders register on a fourth subcommand without a `ConstructionError`. The help order matches `mdlist`'s, with the four new options last (**A1:** `--status` first among them).
- **C3:** the curl transport can attach an `Authorization` header to GETs (a new slist path). `CURLOPT_FOLLOWLOCATION 0` stays. libcurl never logs headers without `CURLOPT_VERBOSE`.
- **C4:** the fixture transport's POST form routing, the thread-safe per-token counter, and the **A1** allowlist (`api/user/follows/manga`, `api/manga/status`) mapping `api/manga/status` to `status.json`.
- **C5:** `ScopedInterruptHandlers` installs with `SA_RESTART`, so the prompt reader must poll. Confirm the API for querying a pending interrupt in `interrupt.hpp`.
- **C6:** `explicit_bzero`, `flock`, `mkostemp`, `renameat` and `openat` are available in the conda sysroot (glibc 2.28 headers).
- **C7:** `prctl(PR_SET_DUMPABLE, 0)` under the dev preset's ASan and LSan. LSan's StopTheWorld uses ptrace, so probe it with `cxx-probe` in the real context. If it breaks LSan, use `RLIMIT_CORE=0`, and check that systemd-coredump honours it.
- **C8:** termios `~ECHO` with `TCSAFLUSH` on fd 0, restored on every path, exceptions included.
- **C9:** the E2E driver (`tests/e2e/CliE2E.cmake`, shared) can't create a 0600 file from a committed input. Decide between a driver option (a serialization point) and keeping the positive path at the CLI tier only.
- **C10:** `EnvSnapshot` and `read_env` gain three strings, so `read_env` is no longer trivially `noexcept`. `io.hpp` is shared.
- **C11:** `-Wswitch` finds every `switch` over `Errc` and `BatchSource`.
- **C12:** `with_remedy` must not append the network remedy to the auth-rejected, login-required, content-failure or **A1** selection-cap messages.

**Contract changes outside `follows`, `login` and `logout`.** Everything else in `download`, `batch` and `mdlist` stays byte-identical. The changes are:
- the shared help footer: row 7 reworded, rows 8 and 9 added (every help golden changes);
- the root `unknown command` list;
- the root help's command list.

**A1 adds no change outside `follows`.**

**Relevant paths:**
- `/home/user/Mangadex-dl/docs/design/follows-spec.md` (the draft this revises)
- `/home/user/Mangadex-dl/docs/design/list-spec.md`
- `/home/user/Mangadex-dl/src/app/cmd_mdlist.cpp`
- `/home/user/Mangadex-dl/src/app/cmd_batch_jobs.hpp`
- `/home/user/Mangadex-dl/src/app/cli.cpp`
- `/home/user/Mangadex-dl/src/app/exit_help.hpp`
- `/home/user/Mangadex-dl/src/app/io.hpp`
- `/home/user/Mangadex-dl/src/app/io.cpp`
- `/home/user/Mangadex-dl/include/mangadex_dl/exit_codes.hpp`
- `/home/user/Mangadex-dl/include/mangadex_dl/error.hpp`
- `/home/user/Mangadex-dl/include/mangadex_dl/api_client.hpp`
- `/home/user/Mangadex-dl/src/core/api_client.cpp`
- `/home/user/Mangadex-dl/include/mangadex_dl/net/transport.hpp`
- `/home/user/Mangadex-dl/src/core/net/curl_transport.cpp`
- `/home/user/Mangadex-dl/src/core/net/fixture_transport.cpp`
- `/home/user/Mangadex-dl/include/mangadex_dl/mdlist.hpp`
- `/home/user/Mangadex-dl/include/mangadex_dl/batch.hpp`
- `/home/user/Mangadex-dl/include/mangadex_dl/mangadex_api.hpp`
- `/home/user/Mangadex-dl/include/mangadex_dl/fs/bounded_read.hpp`
- `/home/user/Mangadex-dl/src/core/interrupt.cpp`
- `/home/user/Mangadex-dl/tests/e2e/run_case.cmake`
- `/home/user/Mangadex-dl/tests/e2e/CliE2E.cmake`
- `/home/user/Mangadex-dl/tests/support/mdx_fixture.hpp`
- `/home/user/Mangadex-dl/tests/support/fake_transport.hpp`

**Sources** (anonymous reads only):
- [MangaDex docs: Authentication](https://api.mangadex.org/docs/02-authentication/)
- [MangaDex docs: Personal clients](https://api.mangadex.org/docs/02-authentication/personal-clients/)
- [MangaDex docs: Rate limits](https://api.mangadex.org/docs/2-limitations/)
- [MangaDex OpenAPI spec (`/user/follows/manga`, `/manga/status`)](https://api.mangadex.org/docs/static/api.yaml)
- [auth.mangadex.org OpenID discovery](https://auth.mangadex.org/realms/mangadex/.well-known/openid-configuration)
- [mangadex-full-api PersonalAuthClient.ts (third-party)](https://cdn.jsdelivr.net/npm/mangadex-full-api@6.2.0/src/shared/PersonalAuthClient.ts)
- [AsyncDex authentication docs (third-party, the refresh-lifetime claim)](https://asyncdex.readthedocs.io/en/latest/authentication.html)