# Feature spec: `mangadex-dl follows`, `login`, `logout`: downloading the titles you follow

**Status:**
- **rev1** (2026-10-28): gate decisions recorded.
  - **Q1 = C:** TTY prompt, an optional private credentials file, and `--password-stdin`. Env vars are deferred.
  - **Q2 = B:** the session is persisted (never the password).
  - **Q3 = B:** top-level `follows`, `login` and `logout`.
  - **Q4 = "both now":** follows by default, plus a reading-status option in this change. That option is amendment **A1**, `--status`.
  - **Q5:** two exit codes, 8 `login_required` and 9 `auth_rejected`.
  - **Q6:** flat by default; `--dir-per-title` opts in, as in `mdlist`.
- **rev2** (2026-10-28): **QA1 resolved as (b).** `--status S` narrows the follows to the titles whose reading status is in S. A1 was reworked to match: the follows order is filtered, the label stays `follows:`, `all` is dropped, and the selection cap is removed. Changes are marked **rev2**.
- **rev3** (2026-10-30): plan gate (user approved `follows-plan.md` rev1) and fact-check (`follows-factcheck.md`). The amendments in "rev3 amendments" below **supersede** any conflicting text later in this spec.
- **Fact from the user's live test:** an anonymous `GET /list/{id}` for a private list answers **404** (mdlist exited 4). This **closes list-spec V1**. It matters only for the deferred "auth on `mdlist`" item.
- Previous drafts: DRAFT 2026-10-28 (cli-designer). This is step 2 of "download the works I follow"; step 1 (`mdlist`) is merged ([`list-spec.md`](list-spec.md)).

This spec designs three commands, in separate subsections:
- **`follows`**, the feature. With **A1** `--status`, the follows are narrowed by reading status.
- **`login`** and **`logout`**, small helpers.

**Intent:**
- Log in to MangaDex with the user's personal API client.
- Page through `GET /user/follows/manga`. With **A1** `--status`, also read `GET /manga/status` and keep only the follows whose status is requested.
- Run every remaining title through the unchanged batch engine. This is exactly `mdlist`'s path, with an authenticated collection in place of `GET /list/{id}`.

**Who runs it:**
- A person at a terminal. The first run asks for the login details once and saves the login.
- After that, mostly cron or a script that runs the same command unattended to pick up new chapters and new follows.

**Assumptions:**
- One account per run and one output root, DIR. The run-level options are `mdlist`'s, byte for byte.
- **Only the collection fetch is authenticated**: the follows pages and, with A1, the status map. Every per-title request stays anonymous, exactly as today: `/manga/{id}`, the feed, covers, at-home, images and MD@H reports. The reasons:
  - MangaDex's auth page says to send auth headers only when a call needs them, because authenticated requests can't be cached.
  - The same page says never to send them to any host other than `api.mangadex.org` or `auth.mangadex.org`.
  - The 15-minute access-token lifetime then never meets the long download phase (see "Token lifetime").
- Titles run one after another, and `--jobs` parallelises images within a title. There are no new threads.

**What was confirmed without credentials** (anonymous reads only; nothing was POSTed and no credentials were sent):
- **Personal-clients docs:**
  - The token endpoint is `POST https://auth.mangadex.org/realms/mangadex/protocol/openid-connect/token`, with `application/x-www-form-urlencoded` (a form, "NOT a Json payload").
  - Password grant: `grant_type=password&username&password&client_id&client_secret`.
  - Refresh grant: `grant_type=refresh_token&refresh_token&client_id&client_secret`. **The refresh needs the client secret too.**
  - Access tokens last 15 minutes.
  - A client may be "pending … requiring manual approval", and then "cannot be used yet".
- **Realm discovery document** (`/.well-known/openid-configuration`, a public GET):
  - issuer `https://auth.mangadex.org/realms/mangadex`;
  - `revocation_endpoint …/protocol/openid-connect/revoke` and `end_session_endpoint …/logout`;
  - `client_secret_post` is among the auth methods, and `password` and `refresh_token` are among the grants.
- **API spec:**
  - `GET /user/follows/manga`: `limit` 1–100 (default 10), `offset`, `includes[]`; it returns a `MangaList`.
  - `GET /manga/status`: an optional `status` query ("Used to filter the list by given status"). It returns `{result, statuses: {<manga id>: <status>}}` with the values `reading`, `on_hold`, `plan_to_read`, `dropped`, `re_reading` and `completed`, and has no pagination parameters.
- **Rate-limit page:**
  - About 5 req/s per IP globally; past that, 429, then a temporary 403 IP ban.
  - It lists only the *legacy* `POST /auth/login` (30 per 60 min) and `/auth/refresh` (60 per 60 min). **The Keycloak endpoint's limits are undocumented.**
- **Not confirmed (third-party code only):**
  - the `expires_in` and `refresh_expires_in` fields, and refresh-token rotation (mangadex-full-api stores the new `refresh_token` from each refresh);
  - "refresh token lasts about one month" (asyncdex docs).


## rev3 amendments (supersede conflicting text below)

Gate answers at the plan gate (2026-10-30): echo policy = hide every value on `follows`/`login`/`logout` (S7); no guard against a mistyped password becoming DIR (it is never echoed in an error). Each row names the section it amends.

| # | Spec section | Change |
|---|---|---|
| S1 | Status filter → Parsing (`parse_status_map`), "Plus a separator cap of 1 000 000" | Reword to "**two** per entry at the entry cap (200 000), plus headroom; 1 000 000". (V24) |
| S2 | Status filter → Keys, "A duplicate JSON key is malformed, if the parser surfaces it (VERIFY D8)" | Replace with "A duplicate JSON key is not detected: the parser keeps the **last** value (V23). Two keys that canonicalise to the same id are kept once, with the first value." Close D8 in the VERIFY list. |
| S3 | Summary ("at most 10 000"), Behaviour phase 7.1, Large input, the All-outcomes row | Cap wording: "at most 10 000 follows, the window MangaDex documents for collection endpoints; more are refused, never truncated". Message: `you follow more than 10000 titles on MangaDex; refusing to truncate the list`. (V5) |
| S4 | `logout` behaviour, step 2 | The revocation URL is the fixed `<issuer>/protocol/openid-connect/revoke`, with no discovery request. **Success is any HTTP 200, whatever the body**: Keycloak answers 200, even with a JSON error, for an invalid token. Only a 4xx, a 5xx or a transport error is a failed revoke and gets the warning. (V1, V3) |
| S5 | Exit codes (rows 8, 9, the texts) and Auth requests → Token responses | Add: "**Classification uses the `error` code only, never the HTTP status** (Keycloak answers a wrong password with 401 up to 26.5.0 and 400 on main): `invalid_grant` on a password grant → 9 (password text); `invalid_grant` on a refresh grant → session ended (password fallback, else 8 "expired"); `invalid_client` → 9 (client text); `unauthorized_client` → 9 (approval text); any other `error` → 9 with `MangaDex refused the login (<code>)`; a 4xx without a parsable `error` → 3 `<url>: HTTP <n>`." (V2) |
| S6 | Auth requests → Retries; AC6 | "At most **3 logical token requests** (password or refresh grants) per run; each may retry transport errors, 5xx and 429; the revoke is not counted." AC6: "never more than 3 token requests". (Spec issue 3) |
| S7 | Semantic usage checks → `--title`, Secret-shaped extras | Full `--title` text (Spec issue 4). Replace the extras bullets with the T19 echo policy: the secret-shaped prefixes `--password*`, `--client-secret*`, `-p*` → the fixed message; single-dash words longer than 2 characters → counted, never echoed; other option-looking extras echoed cut at `=` (terminal_safe); bare words counted; any flag given `=value` → `<flag> takes no value`; a ConversionError on a value option → `<option>: invalid value (not shown)`; ValidationErrors of `--jobs`/`--layout`/`--color` etc. keep CLI11's text. The extras message is `<cmd> does not take these arguments: <names> (and N other words, not shown in case they hold a password)`, or `<cmd> does not take N more argument(s) (not shown in case they hold a password)`. |
| S8 | Command surface → `--credentials FILE`, `--status` | `--credentials ''` → exit 2 `--credentials needs a file name`. The value is taken verbatim, so `--credentials --no-input` reads a file named `--no-input` (exit 1 if missing, the stock CLI11 behaviour). `--status` is bound one value per occurrence and split by the tool, not with `->delimiter(',')` (D7 closed). |
| S9 | `mdlist` (list-spec "help" text, and any follows-spec mention) | The mdlist footer line becomes `To download the titles you follow, see 'mangadex-dl follows --help'.` |
| S10 | Input & output → Prompts | An answer over 4096 bytes → exit 8 `the <field> you typed is longer than 4096 bytes`. Note: a canonical-mode terminal caps a line at 4095 bytes. |
| S11 | `logout` | The malformed-session and insecure-directory rules (Spec issue 9). The help lists the hidden `--api-base` (not shown) **plus the inherited global flags** (V9c). |
| S12 | Hermetic test story #2, #5; AC10, AC29, AC32 | Fixture statuses: a wrong password answers 401 `invalid_grant` by default, plus a 400 variant; refresh failures and `unauthorized_client` answer 400; a revoke of an unknown token answers 200 `{"error":"invalid_token"}`; a scripted status (for example 503) fails a revoke. AC29 and AC32 use generated `{{FIXTURES}}/<tree>/creds/{credentials-0644,password.txt}`. C9 is resolved: no driver change; positive paths use `--credentials` and `PRE_RUN login --credentials …`. |
| S13 | Engine reuse | `Endpoints` unchanged; `auth_endpoints_for`; `ApiClient::get_authorized` instead of reusing `paginate`; `Prompter` is a core interface (`prompt.hpp`); `secret.hpp`, `session_store.hpp` are new headers. |
| S14 | Secret hygiene, memory row | Adopted: `CURLOPT_POSTFIELDS` (libcurl keeps no copy of the body); `CURLOPT_VERBOSE` is never enabled (it would print the Bearer header); `PR_SET_DUMPABLE 0` plus `RLIMIT_CORE 0` confirmed compatible with ASan/LSan/UBSan/TSan and with `/proc/self/fd` publishing (V20, V21). |
| S15 | Open questions → VERIFY list | Mark resolved: A1, A2 (partial: status varies, see S5), A7 (+ the S4 caveat), D1 (both `{}` and `[]` accepted), D2 (not sent), D7, D8, C1–C12. A3–A6, B1–B6, D3–D6 stay for the human ACs. |
| S16 | login/logout | The help footers per Spec issue 12, worded by cli-ux. |

## Decisions at the design gate (user, 2026-10-28)

- **Q1, where credentials come from: C.**
  - A TTY prompt, an optional private credentials file, and `--password-stdin`. Credentials never go on argv.
  - Env vars are deferred. If they are added later, they rank above the credentials file, and the tool applies that order itself.
- **Q2, persistence: B.**
  - The session lives at `$XDG_STATE_HOME/mangadex-dl/session.json`, mode 0600, in a 0700 directory, written atomically.
  - It holds client_id, client_secret, username, the refresh token, the access token and expiry times. It **never holds the password**.
  - It is rewritten after every token response, so a rotated refresh token is never lost.
- **Q3, command surface: B.**
  - Top-level `follows`, `login` and `logout`. `whoami`/`status` are deferred.
  - Auth on `mdlist` (reading your private lists) is deferred. It would be additive and must be opt-in: the anonymous private list → 404 → exit 4 path is confirmed live and stays.
- **Q4, what gets downloaded: both now.** The follows by default; with **A1** `--status`, the follows narrowed by reading status.
- **QA1 (rev2), the meaning of `--status`: (b).** It is follows ∩ status. The Library-by-status set is out of scope.
- **Q5, exit codes:** two codes, **8 `login_required`** and **9 `auth_rejected`**.
- **Q6, `--dir-per-title`:** off by default, opt-in, with the same meaning as in `mdlist`.

---

## Summary

`mangadex-dl follows [DIR]` logs in to MangaDex as the owner of a personal API client and collects the account's followed titles (paginated, at most 10 000).
- With `--status reading,on_hold,…` (**A1**), only the followed titles whose reading status is one of those given are kept.
- Each title is then downloaded into DIR through the same engine as `mdlist`: the same options, stop rules, stderr wording ("item"), exit aggregation and `--json` document.
- Only the collection requests carry the access token. Downloads are anonymous.

Login:
- The first interactive run asks for the client id, client secret, username and password, with a hint on where to create a client. It logs in and saves a session, never the password.
- Later runs, cron included, refresh that session silently.
- `mangadex-dl login` sets the session up without downloading anything.
- `mangadex-dl logout` revokes the session at MangaDex (best effort) and deletes it.

Users: a person at a terminal first, then cron and scripts.

**Friction audit.** The primary task, `mangadex-dl follows ~/Manga`, takes one positional and zero flags.
- The first time on a TTY: four prompts, once.
- Afterwards: nothing to type until MangaDex ends the session.
- Cron: nothing, after one `login`.
- Narrowing by status adds one flag: `--status reading`.

## Command surface (CLI11)

**Files:**
- App: `src/app/cmd_follows.{hpp,cpp}` (`register_follows`) and `src/app/cmd_login.{hpp,cpp}` (`register_login`, `register_logout`). They add three registration lines to `cli.cpp`, which is a **serialization point**.
- Core: `include/mangadex_dl/auth.hpp`, `include/mangadex_dl/follows.hpp`, `src/core/auth*.cpp` and `src/core/follows*.cpp`.
- **A1:** the status-map parser and the filter live in `follows.hpp`.

### `follows`

- **Invocation:** `mangadex-dl follows [OPTIONS] [DIR]`.
- **Positional:** `DIR`, optional, default `.`: "Output directory (created if missing)". Byte-identical to `mdlist`.

**Options.** Exactly `mdlist`'s set, bound by the same helpers so the help strings are byte-identical: `bind_selection_options`, `bind_run_options` (including the hidden `--api-base`), `--fail-fast` and `--dir-per-title`.

New options. Their help texts fit the 44-column right column.
- `--credentials FILE`: "Read login details from FILE".
  - A path, not a secret.
  - No `CLI::ExistingFile`: a missing file is reported when it is opened (TOCTOU), with exit 1.
  - No `capture_default_str`: the default depends on the environment, and the help must not. No `envname`.
- `--password-stdin` (flag): "Read the password from stdin". Read once, after parsing and before any network use.
- `--no-input` (flag): "Never prompt; fail if a login is needed".
- **A1:** `--status WORD,...`: "Only follows with these reading statuses" (**rev2** wording, 39 columns).
  - **Parsing:** a `std::vector<std::string>` that is repeatable and comma-separated inside one value (`->delimiter(',')`, VERIFY D7). `--status reading,on_hold` equals `--status reading --status on_hold`.
  - **Validation** happens after parsing (comma splitting and aliases rule out `IsMember`). The help column shows `WORD,...` and never a default.
  - **Words:** the six API values `reading`, `on_hold`, `plan_to_read`, `dropped`, `re_reading` and `completed`. The kebab forms `on-hold`, `plan-to-read` and `re-reading` are accepted and normalised to the API's underscore form, so a kebab-case CLI doesn't trip users. Exact lower case only. Repeated words are de-duplicated, keeping the first occurrence.
  - **No `all` value (rev2).** Running without `--status` already means "every title you follow". An `all` that meant "followed titles with any status" would differ from that default only by silently dropping follows with no status, a distinction that invites confusion. Users who want exactly that can list the six words. It is additive later if anyone asks.
  - **When absent:** every follow (the default).
  - **Name** considered and rejected: `--reading-status`. It is precise but long. The help line, the footer and the README all say "reading status", so the short form is unambiguous in context.
  - **Not offered** on `login` or `logout`.

**Not offered, on purpose:** `--password`, `--client-secret`, `--username`, `--client-id` and `--title`. See "Semantic usage checks".

**Global flags:** `--json`, `-q`, `-v` and `--color` work anywhere, through the existing `fallthrough()`. `--version` has no `-v` alias.

**Prompts** happen only when all of these hold:
- stdin and stderr are both TTYs;
- `--no-input` is absent;
- `--password-stdin` is absent (stdin is spoken for).

Otherwise a missing value is **exit 8** with the remedy. This deliberately deviates from cli-design's "a needed prompt with `--no-input` is exit 2": the invocation is valid, and what is missing is state.

**Semantic usage checks.** Each exits 2, prints nothing on stdout, opens no file descriptor, sends no request, and ends with `Run 'mangadex-dl follows --help' for more information.`
- `DIR must not be empty`.
- `DIR looks like a MangaDex id; follows takes no title or list (use 'mangadex-dl mdlist <list>' or 'download <title>')`.
- Every run option is validated as in `mdlist` (`detail::validate_common`).
- **A1:** `--status: unknown reading status '<word>'; use reading, on_hold, plan_to_read, dropped, re_reading or completed`. The word is passed through `terminal_safe` and cut to 32 bytes; it is argv, not a secret. **rev2:** `all` is now an unknown word and gets this message.
- **A1:** an empty word (`--status ''`, `--status reading,`, `--status ,`): `--status needs a reading status (reading, on_hold, plan_to_read, dropped, re_reading or completed)`.
- An `--api-base` that is neither the official API nor a `file://` fixture: `follows logs in only to MangaDex itself; --api-base must be the official API`. This keeps credentials from ever reaching a custom host.
- `--password-stdin` with an empty stdin (EOF before any byte, `/dev/null` or closed), more than 4096 bytes, a NUL byte, or an empty first line: `--password-stdin was given, but stdin held no password`.
- `--title` or `--title=X`: `follows has no --title option; …` (this generalises `title_option_owner`).
- **Secret-shaped extras:**
  - `--password`, `--password=…`, `--client-secret…` and `-p` get `follows has no --password option: passwords and secrets never go on the command line (other users can see it); use the prompt, --password-stdin or the credentials file`.
  - **For `follows`, `login` and `logout`, no ExtrasError ever echoes an argument's value or a bare word.** Only option names are echoed, cut at `=`. CLI11's default ExtrasError text lists the extras verbatim (VERIFY C1), and `follows out hunter2` must not print `hunter2`.

**Root:**
- `unknown command 'x' (commands: download, batch, mdlist, follows, login, logout)`.
- Root `--help` lists the three new one-liners.

**Help draft for `follows --help`** (for cli-ux; lines ≤ 80):
```
Download every title you follow on MangaDex (needs a login)


mangadex-dl follows [OPTIONS] [DIR]


POSITIONALS:
  DIR [.]                           Output directory (created if missing)

OPTIONS:
  ... exactly as mdlist, including --fail-fast and --dir-per-title ...
      --status WORD,...             Only follows with these reading statuses
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
  --status narrows the titles you follow to those whose reading status is
  one of: reading, on_hold, plan_to_read, dropped, re_reading, completed.
  Give several separated by commas, or repeat --status. Followed titles
  without a reading status are left out. The titles keep the order of your
  follows.

  Only the lists of titles are fetched with your login; the titles
  themselves are downloaded anonymously, one after another, as with mdlist.

Examples:
  Log in once, then download everything you follow into ~/Manga:
    mangadex-dl login
    mangadex-dl follows ~/Manga
  Only what you are reading or plan to read, one folder per title:
    mangadex-dl follows ~/Manga --status reading,plan_to_read --dir-per-title
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
- **Options:** `--credentials FILE`, `--password-stdin`, `--no-input` and the hidden `--api-base`, with the same rules and help strings as on `follows`. No positionals and no `--status`.
- **Behaviour:** always a **password grant**, even when a session exists. This is how the user renews or switches the login.
  - Values are gathered in the Q1 order.
  - The password prompt shows the username: `MangaDex password for <username>: `.
- **Output:**
  - stdout is always empty: there is no data, also with `--json`, and this is documented.
  - stderr, not with `-q`: `mangadex-dl: logged in to MangaDex as <username>; the login is saved in <path>`.

### `logout`

- **Invocation:** `mangadex-dl logout`. One-liner: "Remove the saved MangaDex login".
- **Options:** the hidden `--api-base` only (for fixture tests). No prompts. stdout is always empty.
- **Behaviour:**
  1. Take the session lock and read the session.
  2. **Best effort:** POST the RFC 7009 revocation (`token=<refresh>&token_type_hint=refresh_token&client_id&client_secret`) to the discovery document's `revocation_endpoint`, with 5 s connect and stall limits and no retries. The answer is ignored apart from success or failure.
  3. `unlinkat` the session file.
- **Messages:**
  - `logged out; removed <path>`.
  - `not logged in (no saved login at <path>)`, exit 0 (idempotent).
  - A failed revoke: the warning `could not end the session at MangaDex (<reason>); the saved login was removed anyway`, exit 0.
- `logout` never reads or touches the credentials file. The README says "delete it yourself if it holds your password".

## Input & output

**Sources and precedence:**
- There is no CLI11 config file and no `envname` anywhere. CLI11's CLI > config > env > default has nothing to arbitrate. The CLI-level options, A1's `--status` included, keep "command line > built-in default".
- No credential value passes through CLI11. The tool applies its own order, per field:
  - client_id, client_secret, username: **credentials file > saved session > TTY prompt > error (exit 8)**;
  - password: **`--password-stdin` > credentials file > TTY prompt > error (exit 8)**. It is needed only when there is no usable session.
- If env vars are added later, they rank **above** the credentials file. The tool applies that itself, the opposite of CLI11's config-beats-env.

**Credentials file:**
- **Path:** `--credentials FILE`, else `$XDG_CONFIG_HOME/mangadex-dl/credentials`. `XDG_CONFIG_HOME` is used only if it is absolute; otherwise `$HOME/.config/…`. Without a usable `HOME` there is no default file.
- An absent default file is not an error. An absent `--credentials` file is exit 1.
- **Opening and checks:** open with `O_RDONLY|O_CLOEXEC|O_NOCTTY|O_NONBLOCK` (symlinks followed, for dotfile managers), then `fstat` the descriptor. It must be:
  - a regular file;
  - owned by the user: `st_uid == geteuid()`;
  - private: `(st_mode & 077) == 0`.

  Otherwise exit 1, with one of:
  - `credentials file '<path>' is readable or writable by others (mode 0644); run 'chmod 600 <path>'`;
  - `… is not owned by you`;
  - `… is not a regular file`.
- **Bounded read:** at most 16 KiB (`read_bounded_fd` semantics).
- **Format:** lines of `key = value`.
  - Keys are exactly `client_id`, `client_secret`, `username` and `password`.
  - `#` at the start of a line (after optional whitespace) is a comment. Blank lines are allowed. A final `\r` is stripped.
  - The value is the rest of the line after the first `=`, trimmed of spaces and tabs. An empty value means absent.
  - Errors exit 1, naming the line and **never the value**:
    - an unknown key (echoed only if it matches `[a-z_]{1,32}`);
    - a duplicate key;
    - a line without `=`;
    - a control byte or NUL in a value;
    - invalid UTF-8.
  - A password with leading or trailing spaces can't be written in the file; the README points to `--password-stdin`.

**Prompts** (written to stderr, read from the stdin TTY):
- Before the first client prompt, one line: `mangadex-dl: reading your follows needs a MangaDex personal API client; create one at https://mangadex.org/settings (API Clients)`.
- The prompts, in order:
  1. `MangaDex API client id: ` (echo on);
  2. `MangaDex API client secret: ` (**echo off**);
  3. `MangaDex username: ` (echo on);
  4. `MangaDex password for <username>: ` (**echo off**).
- **Echo off** is termios `~ECHO` with `TCSAFLUSH`, restored by RAII on every path, then a newline on stderr.
- Each answer is at most 4096 bytes, up to `\n`. An empty answer or EOF (Ctrl-D) is exit 8: `no <field> given`.
- Ctrl-C at a prompt restores echo, saves nothing, and gives 130. The existing handlers use `SA_RESTART`, so the reader must poll and check the pending-interrupt flag (VERIFY C5).
- In-process tests replace the real prompt through an `Io` seam (a prompter interface).

**`--password-stdin`:**
- `read_bounded_fd(io.stdin_fd, "stdin", 4096)`, read to EOF.
- The password is the first line, without its `\n` or `\r\n`.
- It is read once, and always when the flag is given, even if a session makes it unnecessary: a predictable pipe contract.

**stdin otherwise:** read only for prompts or `--password-stdin`. A closed stdin or `/dev/null` changes nothing without them; a needed prompt is then exit 8.

**stdout:** data only, exactly `mdlist`'s: one path per written or planned file, in item order. `login` and `logout` print nothing on stdout.

**stderr for `follows`:**
- **Row prefix:** `follows:<n>: <manga-uuid>: `, after `error:` or `warning:`.
  - `<n>` is the 1-based position in the run's sequence of titles: the follows in API order, or **A1/rev2** with `--status`, the follows that passed the filter, still in follows order and numbered 1..k without gaps.
  - The label is always `follows` (**rev2:** no `library:` label).
- **Wording:** `mdlist`'s "item" wording, for the closing line, duplicate, collision and dir-per-title lines. This is `BatchSource::follows`, which shares `mdlist`'s strings.
- **Run-level lines** (no prefix):
  - Empty: `warning: you follow no titles on MangaDex`.
  - **A1, rev2:** follows exist but none match: `warning: none of the titles you follow have the reading status reading, on_hold`, with the statuses listed as given and normalised.
  - Both empty cases: not shown with `-q`, exit 0, DIR not created.
  - Session not saved: `warning: could not save the MangaDex login to <path>: <reason>; the next run will need the password again`. The run continues.
  - Session ignored: `warning: ignoring the saved login at <path>: <why>` (malformed; wrong owner or mode; another account or client than the credentials file). A fresh login follows if possible.
- **`-v`:**
  - `mangadex-dl: logged in to MangaDex as <username> (saved login|refreshed|password)`.
  - `mangadex-dl: follows: <n> titles`.
  - **A1, rev2:** `mangadex-dl: follows: <k> of <n> have the reading status reading, on_hold`.
  - **A1:** `mangadex-dl: follows: <j> titles have a reading status mangadex-dl does not know ('<value>') and are left out`. Printed once per distinct unknown value, counting only followed titles. The value is the API's, validated to `[a-z_]{1,32}`.
  - Per item: `follows:<n>: <uuid>`, then the engine's lines.
  - The username is display-only: cut to 64 bytes on a UTF-8 boundary and passed through `terminal_safe`.
- **`-vv`:** the request trace.
  - POSTs show method, URL, status and time only, never the body or headers: `POST https://auth.mangadex.org/realms/mangadex/protocol/openid-connect/token -> 200 (210 ms)`.
  - The collection appears as `GET …/user/follows/manga?limit=100&offset=0 -> 200 (… ms)` and, with **A1**, `GET …/manga/status -> 200 (… ms)`. The token travels in a header, not the URL.
- **Progress:** as in `mdlist`, TTY stderr only and never with `-q` or `--json`: `(2/57) [1/3] Vol. 1: 3/5 pages`. Prompts come before any progress line.

**`--json`** (`follows` only):
- **When it is printed:** one document at the end, under `mdlist`'s rule: when the items ran and were not interrupted. **Never for usage errors, auth failures (8/9), collection-fetch failures (3) or interrupts.**
- **Format:** `ordered_json`, indent 2, `error_handler_t::replace`, never ANSI (even with `CLICOLOR_FORCE=1`).
- **It is `batch`'s document plus one additive key, `follows`, after `source`.** `format_version` stays 1. `batch` and `mdlist` documents are byte-identical to today's.
  ```
  format_version: 1
  dry_run: bool
  source: "follows"                  the literal command word (there is no positional to echo)
  follows: {                         follows only
      username: string               as configured (file/session/prompt), never from the API
      titles: int                    number of follows the API returned, duplicates included
      statuses: [string] | null      A1: null without --status; otherwise the requested statuses,
                                     normalised (underscores), de-duplicated, in the order given
      selected: int                  A1 (rev2): number of follows that passed the filter (the
                                     number of items); equal to `titles` without --status
  }
  titles: [ { line: int (position, as <n> above), manga_id, status, exit_code, error,
              duplicate_of (a position), report, dir } ]          exactly as mdlist
  summary: { titles: {...}, files: {...} }                        as batch
  ```
- **rev2 key order inside `follows`:** `username`, `titles`, `statuses`, `selected`. All four are always present, so the shape never depends on `--status`. rev1's `source: "library"` is gone.
- `titles[].status` is the item's download outcome, as in `batch`. It is unrelated to the reading status, and the README says so.
- **No token, secret, password or client id ever appears in the document.**
- Empty, or nothing matched: `titles: []` with the `follows` object filled in.

**Color:** stderr prefixes only, through `resolve_color`. Prompts are never colored.

**Locale:** positions and counts use `std::format` without `L`. Expiry times are integers (`from_chars`/`to_chars`).

## Status filter (A1: `--status`, rev2)

**Semantics (QA1 = b):** `--status S` keeps the titles that the account **follows** and whose reading status is in S.
- A followed title **without** a reading status (absent from the map) is never kept.
- A title that **has** a status but is **not** followed is never downloaded. Downloading the Library-by-status set is out of scope, additive later as a separate option.

**Requests:**
- First the follows pages, exactly as without `--status`.
- Then, if there is at least one follow, **one** `GET <api>/manga/status` with **no `status` query**, with `Authorization: Bearer <access>`. The filtering happens locally.
- If there are no follows, the status request is skipped.
- **Why one unfiltered request rather than `?status=<s>` per status:**
  - it is one request whatever S is, instead of up to six;
  - it lets `-v` report `<k> of <n>` and the unknown-value note;
  - the query parameter's semantics are unverified (VERIFY D2).
- The endpoint has no pagination parameters, so the whole map arrives in one response (VERIFY D3).
- Both requests run under the same lock and token handling, within the 3-POST cap.

**Parsing (`parse_status_map`, pure, untrusted input):**
- `json_util::parse_untrusted` with the API defaults: a 16 MiB body cap (`max_api_json_bytes`), depth 64, BOM and NUL rejected.
- **Plus a separator cap of 1 000 000.** That is three per entry at the entry cap, plus headroom; the planner may tune it.
- **Structure:** the top level is an object, `result` equals `"ok"`, and `statuses` is an object.
  - An **empty JSON array `[]`** is accepted as an empty map. PHP backends often serialise an empty map that way (VERIFY D1).
  - Anything else is malformed.
- **Entry cap:** more than **100 000** entries is refused with Errc::network: `your MangaDex reading statuses hold more than 100000 titles; refusing to truncate them`. The map covers every title with a status, followed or not, so it can be larger than the follows.
- **Keys:** each must pass `is_uuid` and becomes a `MangaId` (canonical lowercase) **before** it is compared or used. Otherwise the whole document is malformed (exit 3), consistent with `parse_custom_list`.
  - Two keys that canonicalise to the same id are kept once, with the first value.
  - A duplicate JSON key is malformed, if the parser surfaces it (VERIFY D8).
- **Values:** each must be a string matching `[a-z_]{1,32}`. Anything else (a number, null, an object, an over-long or odd string) makes the document malformed.
  - **A well-formed but unknown value is not an error.** That title just never matches, because no `--status` word can name it, and it counts toward the `-v` note.
  - Why skip rather than fail: MangaDex adding a status must not break every cron run.
- Messages are fixed text that names fields, never response bytes or ids.

**Filter and order:**
- Normalise S: aliases, de-duplication.
- Keep each follow, in **follows (API) order**, whose canonical id maps to a status in S.
  - The order of the words in S does not reorder anything: S is a set.
  - **rev2:** the follows order is kept because the run is "my follows, narrowed". A re-run with or without `--status` lists shared titles in the same relative order. Grouping by status would reorder the user's follows for no stated need.
- Duplicates in the follows (possible if pages shift) survive the filter and are marked by `mark_duplicates` afterwards, as without `--status`.
- **`<n>` and `titles[].line`:** 1-based positions **in the filtered sequence**, without gaps. The closing line ("items 4, 9"), "N of M titles" and `duplicate_of` count the run's own items.
  - Rejected: the original follows position. Its gaps would read as missing items, and `line` would stop agreeing with `summary.titles`.

**Caps (rev2):** the follows cap (10 000, refused rather than truncated) bounds the selection, so there is **no separate selection cap**. The status map keeps the entry cap and the parsing rules above.

**Auth:** exactly the follows rules.
- Refresh proactively if the token expires within 30 s.
- On a 401, one refresh (or the password fallback), then retry the request once. A second 401 is exit 9.
- The 3-POST cap applies to the whole run.
- A 403 keeps the existing mapping (exit 3, possibly a ban).

## Exit codes

New codes go in `exit_codes.hpp` (a serialization point), each with a doc comment. `error.hpp` gains `Errc::login_required` and `Errc::auth_rejected`, and every `switch (Errc)` gets them, the batch runner's `is_fatal` included (VERIFY C11). **A1 adds no code.**

| Code | Name | Meaning / when |
|---|---|---|
| **8** | `login_required` | follows, login: no usable saved login, and the missing value can't be asked for (no TTY, `--no-input`, `--password-stdin` already used stdin, an empty answer or EOF at a prompt); or no API client is configured at all; or the saved session has expired or been revoked (`invalid_grant` on refresh) and no password source exists |
| **9** | `auth_rejected` | follows, login: the auth server refused the details. That covers a wrong username or password (`invalid_grant` on the password grant), a wrong client id or secret (`invalid_client`), a client that is pending, disabled or not allowed this grant (`unauthorized_client`/`invalid_client`), and a locked account. Also: the API answered 401 to a freshly obtained access token on the follows or **A1** status endpoint |

**Messages.** All are fixed text and never a secret.
- The server's `error` code is appended only if it matches `[a-z_]{1,64}`.
- Its `error_description` appears only with `-v`, passed through `terminal_safe` and cut to 200 bytes.

The texts:
- 8, no client: `no MangaDex API client is set up; run 'mangadex-dl login' in a terminal, or put client_id and client_secret in <credentials path>`.
- 8, login needed: `you are not logged in to MangaDex; run 'mangadex-dl login' in a terminal (or pass --password-stdin)`.
- 8, session ended: `your saved MangaDex login has expired or was ended; run 'mangadex-dl login' in a terminal`.
- 9, password: `MangaDex rejected the username or password for <username> (invalid_grant); check them, or log in on the website to see whether the account is locked`.
- 9, client: `MangaDex rejected the API client (invalid_client); check client_id and client_secret, and whether the client is approved at https://mangadex.org/settings`.
- 9, fresh token refused: `MangaDex rejected a new access token for your follows (HTTP 401); check that the API client is approved and allowed to read your account`. **A1 (rev2):** on the status request, "your follows" becomes "your reading statuses".

**All outcomes:**

| Outcome | Code |
|---|---|
| All items ok or duplicate; no follows; **A1** no follow matched; `login` succeeded; `logout` (also when not logged in, or the revoke failed) | 0 |
| Credentials file unreadable, insecure, malformed or (explicit path) missing; session lock not acquired within 60 s; `login` could not save the session; `logout` could not remove it; backend unusable (no libcurl); DIR unusable and other global local errors (the rest become `not_run`); stdout write error | 1 |
| Every `ParseError`; the DIR checks; bad option values, **A1** including bad `--status` words (`all` too, rev2); a non-official, non-fixture `--api-base`; an empty `--password-stdin`; `--title` or `--password`-style extras | 2 |
| Auth, follows or **A1** status endpoint: connect, DNS, TLS, timeout, 5xx or 429 retries exhausted, 403 (possibly a ban, existing message) | 3 |
| A malformed token response (missing `access_token`, bad token charset or size) | 3 |
| A malformed follows page (wrong type, a non-UUID id, over 16 MiB); more than 10 000 follows (refused, not truncated) | 3 |
| **A1:** a malformed status map (not an object, a non-UUID key, a malformed value, over 16 MiB, over the separator cap); more than 100 000 entries | 3 |
| More than 3 token requests in one run | 3 |
| Items: one failure class gives that code (1, 3, 4, 5, 6); several give 7 | as `mdlist` |
| SIGINT or SIGTERM (also at a prompt or during auth) | 130/143 |

- Auth and collection request failures get `with_remedy`'s network remedy. Content failures (malformed, over a cap) don't, the same split as `mdlist`'s `ListFailureKind`.
- **Help footer.** It is shared, so every help golden changes. Row 7 is reworded to fit 80 columns now that it would name three commands, and two rows are added:
  - `7   several titles failed in different ways (see per-title results)`
  - `8   follows/login: not logged in, or the saved login expired`
  - `9   follows/login: MangaDex rejected the login details`
- Also update the README EXIT STATUS and the `mixed_failures` doc comment.

## Behaviour & edge cases

**`follows` phases:**
1. **Validate** (pure, exit 2). **A1:** this includes normalising `--status`.
2. **Set up:** install the interrupt handlers, optionally harden against core dumps (see "Secret hygiene"), then `make_backend`.
3. **Read `--password-stdin`** if given (exit 2 if empty).
4. **Credentials:** read and check the credentials file.
5. **Session:**
   - Take the exclusive lock, `<state dir>/session.lock`, with `flock` and `LOCK_NB`, polled every 250 ms while checking for interrupts, for at most 60 s. A timeout is exit 1: `another mangadex-dl is using the saved login; try again`.
   - Read the session: bounded to 64 KiB, `parse_untrusted`, every field validated.
   - Ignore it, with a warning, if it is malformed, has the wrong owner or mode, has another issuer, or its client_id or username differ from the credentials file.
6. **Get an access token**, with at most 3 token POSTs per run:
   1. the saved access token, if it is still valid for at least 60 s (wall clock);
   2. else a refresh grant, if there is a refresh token;
   3. on `invalid_grant` there, or with no session, a password grant. The password comes from the Q1 sources, prompting for missing fields if allowed; otherwise exit 8.

   **After every successful token response, the session is written atomically before anything else happens,** including before a pending interrupt is honoured, so a rotated refresh token is never lost. A failed write is a warning, and the run continues.
7. **Fetch the collection:**
   1. Follows: `GET <api>/user/follows/manga?limit=100&offset=<k>` with `Authorization: Bearer <access>`, through the existing `paginate`.
      - It steps by the items received, stops on an empty page or once `offset >= total`, and refuses anything beyond the 10 000 window.
      - Each page is parsed as a `MangaList`: `data[i].type == "manga"`, and `data[i].id` must pass `is_uuid` and becomes a `MangaId`. Attributes are ignored.
      - **Before each page:** if the token expires within 30 s, refresh first.
      - **On a 401 for a page:** one refresh (or the password fallback), then retry that page once. A second 401 is exit 9.
   2. **A1:** if `--status` is given and there is at least one follow, one `GET <api>/manga/status`, with the same token handling (proactive refresh, one refresh-and-retry on a 401, a second 401 is exit 9). Then parse it and filter, as in "Status filter".
   3. Release the lock, and wipe and destroy every token and secret in memory.
8. **Plan:** `plan_list`-equivalent jobs (position, DIR, options, `dir_per_title`, duplicates marked). An empty result gives the matching warning, the JSON if asked, and exit 0.
9. **Run:** `run_batch_jobs` with `BatchSource::follows`, the label `follows` and the follows summary, over the same anonymous `Backend`.

**Token lifetime during a long run:**
- The token is used only in phase 7, which is bounded: at most 100 pages at about 250 ms each, plus **A1**'s one status request, plus any 429 back-off.
- Expiry is handled proactively before each request, with one reactive 401 retry.
- Phase 9 can take hours: titles run one after another, with `--jobs` image workers. It sends **no Authorization header at all**, so the 15-minute expiry can't bite there. Nothing refreshes during downloads, and no token is shared with worker threads.
- **Rejected:** an authenticated per-title client with a shared refresher. It would need a thread-safe token cache across `FetchPool` workers, it would defeat the CDN cache for every title, and it goes against MangaDex's "send auth only when needed".

**Auth requests:**
- **Form body:** a POST, percent-encoded by a pure core function. RFC 3986 unreserved characters are kept; everything else becomes `%XX`.
- **Token responses:**
  - The body cap is 64 KiB.
  - `access_token` and `refresh_token` must match `[A-Za-z0-9._~+/=-]{1,16384}`. This prevents header injection and is checked before use; anything else is exit 3.
  - `expires_in` must be an integer from 1 to 86400, else 900 is assumed.
  - `refresh_expires_in` of 0, or absent, means unknown (stored as null).
  - When the refresh response carries a new `refresh_token`, it replaces the old one (rotation).
- **Retries:**
  - Transport errors, 5xx and 429 use the existing `decide_retry` policy; 429 honours `Retry-After` and `X-RateLimit-Retry-After`.
  - **Any other 4xx is never retried**, because of Keycloak's brute-force protection.
  - The auth host gets its own limiter of 1 request per second.
- **The Authorization header** is attached only to an **allowlist of exactly two API paths**, `/user/follows/manga` and (**A1**) `/manga/status`. Both must be on the host `api.mangadex.org` (or the fixture). The header is never sent anywhere else. Redirects are already off (`FOLLOWLOCATION 0`), so no header can be forwarded.

**The five things:**
1. **Success:** paths on stdout, empty stderr.
2. **Empty:** no follows, or **A1** none matched: a warning, exit 0.
3. **Usage errors:** exit 2.
4. **Runtime failures:** 1, 3, 8 or 9, as tabled.
5. **Interrupts,** all ending in 130/143:
   - at a prompt: echo restored, nothing saved;
   - during auth: the session is unchanged, or fully rewritten (atomically);
   - during the collection fetch: nothing printed or created, no JSON;
   - inside the items: exactly as `mdlist`.

**Edge suite:**
- stdin `/dev/null` or closed: exit 0 with a session; 8 without one, never a hang; 2 with `--password-stdin`.
- `--dry-run > /dev/full`: exit 1 plus `mangadex-dl: write error`.
- `| head -1`: 141, also when the parent ignores SIGPIPE.
- No ANSI when piped, or with `--json` under `CLICOLOR_FORCE=1`.
- An invalid-UTF-8 DIR under `LC_ALL=C` is kept verbatim, and the JSON stays valid.
- An invalid-UTF-8 username in the credentials file: exit 1 (invalid UTF-8), never echoed.
- **A1:** an invalid-UTF-8 `--status` word under `LC_ALL=C`: exit 2, echoed only through `terminal_safe`.
- `argc == 0` is unchanged.

**Idempotency:**
- Re-runs pick up new follows and new chapters.
- Unfollowed titles' files are left alone.
- **A1:** a title whose status changes moves in or out of the filtered run, and its position may shift. Its files stay where they are, either flat or in its `--dir-per-title` directory, whose name doesn't depend on the status.
- `login` twice replaces the session. `logout` twice gives 0 and 0.
- **Concurrent runs** are serialised on the session lock, so two cron runs can't burn each other's rotated refresh tokens.

**`--dry-run`:** it still logs in and fetches the collection; it is a dry run of the downloads, not of auth. It may refresh and save the session, and it writes nothing in DIR.

**Large input:**
- At most 100 follows pages of at most 16 MiB each, processed one page at a time.
- **A1:** one status map of at most 16 MiB and 100 000 entries.
- At most 10 000 ids are kept.

## Secret hygiene (`cpp-security` §9)

| Channel | Rule |
|---|---|
| argv / `ps` | No secret options exist. Secret-shaped extras get a fixed message with no value in it. ExtrasError never echoes values for these three commands |
| env | Never read for credentials (Q1 = C). `EnvSnapshot` gains only `HOME`, `XDG_CONFIG_HOME` and `XDG_STATE_HOME`, read once at startup. `read_env` is `noexcept` today and would now allocate |
| stderr / `-v` / `-vv` | Never a token, secret or password. POST traces show method, URL and status only. The username is shown (it is not a secret). Server error text is restricted as described under "Exit codes" |
| `--json` | Only `follows.username`, the counts and (**A1**) the statuses |
| files | The session is 0600 in a 0700 directory. The credentials file is checked for 0600 and ownership |
| core dumps / ptrace | **Recommended:** `prctl(PR_SET_DUMPABLE, 0)` at the start of `follows`, `login` and `logout`, before any secret is read (no core file, no same-user `/proc/<pid>/mem`). Its interplay with LSan in the dev preset must be probed (VERIFY C7); the fallback is `setrlimit(RLIMIT_CORE, 0)` |
| memory | **Recommended, best-effort, not a gate:** a move-only `Secret` type (no formatter, no `operator<<`, `explicit_bzero` in its destructor and on reassignment, capacity reserved up front so a reallocation leaves no copies). Copies inside libcurl (`COPYPOSTFIELDS`) and the TLS stack are out of reach; the planner may switch secret bodies to `CURLOPT_POSTFIELDS` over the caller-owned body |
| child processes | None are spawned |

## Data & state

**Reads:**
- the credentials file, if any;
- the session file;
- the follows pages and, with **A1**, the status map (`api.mangadex.org`);
- token, refresh and revoke answers (`auth.mangadex.org`);
- existing EPUBs in DIR, as `mdlist` does.

**Writes:**
- DIR outputs, exactly as `mdlist` (atomic `OutputDir` publish).
- **Session state** under `$XDG_STATE_HOME/mangadex-dl/`. `XDG_STATE_HOME` is used only if absolute; otherwise `$HOME/.local/state/mangadex-dl/`.
  - Missing directories are created with mode 0700.
  - The final directory is opened with `O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC` and `fstat`ed. It must be owned by the euid with `(mode & 077) == 0`; otherwise the session is neither used nor written (a warning).
  - Files in it are reached with `openat` from that descriptor and `O_NOFOLLOW`. A symlinked session file is refused with a warning.
  - Writing a session:
    1. create a temp file `mkostemp`-style (`.session.json.XXXXXX`, `O_CLOEXEC`);
    2. `fchmod` it to 0600;
    3. write all of it, then `fsync`;
    4. `close`, checking the result;
    5. `renameat` into place, then `fsync` the directory.

    On failure the temp file is `unlink`ed.
  - `session.lock` is 0600, created with `O_NOFOLLOW`.
  - No usable `HOME` means no persistence: a warning for `follows`, exit 1 for `login`.
- **Fixture mode** uses `fixture-session.json` in the same directory and stores the fixture issuer. A test run can never clobber or read a real login.

**Session file** (JSON, `format_version` 1, at most 64 KiB):
- Fields: `issuer`, `client_id`, `client_secret`, `username`, `refresh_token`, `refresh_expires_at` (UNIX seconds or null), `access_token`, `access_expires_at`.
- An unknown `format_version` is ignored with a warning. An older binary never rewrites it without a fresh login.

**Nothing else:** no cache and no TMPDIR use. The tool never creates or edits the credentials file.

**Threat model:**
- **Collection responses are untrusted:** the follows pages and the **A1** status map. Ids and status values are validated before use, and names are not used.
- **Token responses are untrusted:** their charset and size are checked.
- **The credentials file and session** belong to the user but are checked anyway: a world-writable session could plant someone else's token.
- **`--api-base`** is restricted, as above.

`security-reviewer` must review this feature: it touches secrets, files, env and untrusted input.

## Engine reuse (for the planner; names are suggestions)

**Core, new:**
- `auth.hpp`:
  - `ClientCredentials`, `Secret`, and `Session` (built by a validated factory returning `Result`);
  - `parse_credentials_file`, `parse_session` / `session_to_json`, `form_encode`, `parse_token_response`;
  - `classify_token_error`, mapping to login_required, auth_rejected or network;
  - an `AuthClient` over `Transport` + `Clock`, owning the token POST, its retries and its limiter.
- `follows.hpp`:
  - `parse_follows_page`, `follows_request_url`, and `fetch_follows` (paginate, with a token-provider callback for refresh-before-request and the 401 retry);
  - **A1:** `ReadingStatus` (the six values, plus the raw unknown string) and `StatusFilter` (a validated factory from the raw words: aliases and de-duplication; `all` and empty words are Errc::usage);
  - **A1:** `parse_status_map`, `fetch_status_map` (the same token-provider seam), and `filter_follows(follows, map, filter) -> std::vector<MangaId>` (follows order kept, plus counts for `-v` and JSON);
  - `plan_follows`.

**Core, changed** (serialization points):
- `net/transport.hpp`: `HttpRequest` gains an additive `authorization` field (the Bearer value), validated against CR, LF and NUL like `content_type`. The curl transport needs a header slist on GETs too.
- `api_client.hpp`:
  - `Endpoints` gains `auth`: officially `https://auth.mangadex.org/realms/mangadex`; for the fixture `file://<root>/auth/realms/mangadex`.
  - `ApiClient` gains an authenticated GET restricted to the two allowlisted paths.
- `batch.hpp`: `BatchSource::follows`, and a follows summary for `batch_to_json` with `username`, `titles`, `statuses` and `selected`.
- `error.hpp` and `exit_codes.hpp`.

**App:**
- `cmd_follows.cpp`: in `cmd_mdlist.cpp`'s shape; **A1** `--status` is bound to a member `std::vector<std::string>`.
- `cmd_login.cpp`: `login` and `logout`.
- `io.hpp`: the prompter seam and the three env paths.
- `cli.cpp`: the registrations, the commands list and the extras policy.
- `exit_help.hpp`: rows 7–9.
- `cmd_batch_jobs.cpp`: the `follows` row label (**rev2:** a single label).

## Hermetic test story

**No test ever contacts MangaDex.** Fixture mode refuses the network, a non-official `--api-base` is refused for these commands, and the official API is never used in CI.

1. **Unit (FakeTransport + FakeClock).**
   - Scripted token, refresh, revoke, follows and **A1** status-map exchanges.
   - Assert exact form bodies, including passwords with `& = + % space é`.
   - Assert the Authorization header appears only on the two allowlisted URLs, and that **no recorded request to any other URL carries it**.
   - Cover rotation, expiry before a request, a 401 then a retry, `invalid_grant` / `invalid_client`, 429 with Retry-After, 5xx, malformed answers and the 3-POST cap.
2. **Fixture transport extension** (`file://<root>/…`):
   - **Token POSTs** to `<root>/auth/realms/mangadex/protocol/openid-connect/token`. The form is parsed, bounded.
     - `grant_type=password` serves `<root>/auth/password/<username>.json`.
     - `refresh_token` serves `<root>/auth/refresh/<token>.json`.
     - `/revoke` serves `<root>/auth/revoke/<token>.json`.
     - A missing file answers 400 `invalid_grant`.
   - **Fixture file shape:** `{"expect": {client_id, client_secret, password?}, "status": 200, "body": {...}}`. A mismatched `expect` field answers 401 `invalid_client` or 400 `invalid_grant`.
   - **Name components** must match `[A-Za-z0-9._-]{1,128}` and must not be `.` or `..`.
   - **Other POSTs** (MD@H) keep today's 404.
   - **Authenticated GETs:** an **exact allowlist**, `api/user/follows/manga` (with its `offset` pages) and `api/manga/status`. Any other `api/user/…` URL always needs a token.
     - These URLs require `authorization` to be a token T for which `<root>/auth/access/<T>.json` exists.
     - That file may say `{"valid_requests": n}`. The count is kept in memory under a mutex; past it, the fixture answers 401 with a JSON error body.
   - **Mapping:** `file://<root>/api/manga/status` maps to `<root>/api/manga/status.json` through the existing `.json` rule, and `status` can't collide with a manga UUID. Pagination maps `offset=` to `offset-<n>.json`, as today.
   - **Leak tripwire:** any request with an authorization value to a URL outside the allowlist fails with `TransportErrc::local "fixture: Authorization sent to <url>"`, so a leak fails the test loudly.
3. **Fixture data**, generated by `tests/support/mdx_fixture.hpp` (`FixtureAuth`, `FixtureFollows`, **A1** `FixtureStatuses`).
   - Values are obviously fake (`fixture-user`, `fixture-password-not-a-secret`), and nothing secret-looking is committed (`diff-secret-scan`).
   - **Follows sets**, reusing the existing title trees:
     - F1: clean;
     - F2: clean plus an unknown title;
     - F3: empty;
     - F4: a duplicate;
     - F5: a bad id;
     - F6: a 2-page `bulk` set with page size 1;
     - F7: `nochapters`.
   - **A1 (rev2)** status sets, each paired with a follows set:

     | Set | Tree | Follows (API order) | Status map |
     |---|---|---|---|
     | FS1 | `clean` | `6666…`, `1111…`, `7777…`, `8888…` | `{1111…: reading, 6666…: completed, 7777…: paused, 9999…: reading}`. `paused` is unknown; `8888…` is followed without a status; `9999…` has a status but is not followed |
     | FS2 | `clean` | `1111…` | `statuses: []` (empty, array form) |
     | FS3 | `clean` | `1111…` | one value is a number (FS3a) / one key is `../x` (FS3b) |

     The planner decides whether these are separate fixture roots or account variants.
4. **CLI (in-process `run()`):** prompts go through the injected prompter, using the TTY flags in `Io`. Credentials files are created 0600 in a temporary `XDG_CONFIG_HOME`, and sessions live in a temporary `XDG_STATE_HOME`.
5. **E2E:**
   - The driver already gives each case a fresh `HOME` and `XDG_*`, and scrubs `MANGADEX_DL_*`.
   - A 0600 credentials file can't be committed, because git keeps only 644 and 755. So positive E2E cases need a driver feature to create a private copy (VERIFY C9; `CliE2E.cmake` is shared).
   - Without that feature, positive auth paths stay at the `[cli]` tier, and E2E covers the negative and permission paths plus `--password-stdin` (a STDIN file) with a session seeded by a preceding `login` case.

## Acceptance criteria

1. `[unit]` `form_encode` keeps RFC 3986 unreserved characters and percent-encodes everything else. The password-grant body equals the exact expected string for the password `a&b=c+d%e f é`. The refresh and revoke bodies are exact too.
2. `[unit]` `parse_credentials_file`:
   - accepts comments, blank lines, CRLF, `key = value` with trimming, and `=` inside a value;
   - rejects an unknown key (named), a duplicate, a line without `=`, a control byte or NUL, invalid UTF-8, and more than 16 KiB;
   - no error message contains a value.
3. `[unit]` `parse_token_response`:
   - accepts the access, refresh, `expires_in` and `refresh_expires_in` shape;
   - an absent `expires_in` gives 900, and `refresh_expires_in` 0 gives null;
   - rejects a missing `access_token`, a token containing CR, LF, a space or `"`, a 16 385-byte token, non-JSON, and more than 64 KiB.
4. `[unit]` `classify_token_error`:
   - password grant: `400 invalid_grant` → auth_rejected; `401 invalid_client` → auth_rejected with the client wording; `unauthorized_client` → auth_rejected with the approval wording;
   - refresh grant: `400 invalid_grant` → session_expired, which leads to the password fallback or login_required;
   - 429 and 5xx are retried, then network. Any other 4xx is never retried: FakeTransport sees exactly one POST.
5. `[unit]` Session round trip: `session_to_json` then `parse_session` gives an equal value. An unknown `format_version`, a wrong issuer, a bad token charset or more than 64 KiB is rejected, with a reason that contains no token.
6. `[unit]` Token acquisition, order and cap:
   - a valid saved access token → 0 POSTs;
   - an expired access token → 1 refresh;
   - a refresh answered `invalid_grant`, with a password available → the refresh plus a password grant (2 POSTs);
   - the same without a password → login_required;
   - a rotated refresh token is the one stored;
   - never more than 3 POSTs.
7. `[unit]` `fetch_follows` over FakeTransport:
   - URLs are `<api>/user/follows/manga?limit=100&offset=<k>` with `Authorization: Bearer <t>`;
   - pagination ends on `total` or an empty page;
   - a FakeClock advanced past expiry before page 2 → a refresh POST comes before page 2;
   - a 401 on page 2 → refresh, then page 2 is retried once; a second 401 → auth_rejected;
   - total 10 001 → network with the cap message; a non-UUID id → malformed;
   - a stop → interrupted.
8. `[unit]` **No leak.** A full FakeTransport `follows` run (auth, the collection, and one title's manga, feed, covers, at-home and images) runs once without `--status` and once with `--status reading` (**A1**). In both, every recorded request whose URL is not on the two-path allowlist has an empty `authorization`, and only auth-host requests carry a form body with secrets.
9. `[unit]` An `HttpRequest::authorization` containing CR, LF or NUL → `TransportErrc::local` before any I/O. With an empty `authorization`, the curl configuration is unchanged (GETs carry no `Authorization` header).
10. `[unit]` Fixture transport:
    - routes password, refresh and revoke POSTs as specified, and rejects `..` or `/` in names;
    - `valid_requests` counts down, then answers 401;
    - authorization sent to a URL off the allowlist (e.g. `api/manga/<uuid>`) → local error, while `api/manga/status` with a valid token is served (**A1**);
    - MD@H POSTs still get 404.
11. `[unit]` `BatchSource::follows` uses mdlist's "item" wording and the `follows:<n>: <uuid>: ` prefix.
    - The JSON has batch's keys plus `follows{username,titles,statuses,selected}` after `source` (**rev2** shape).
    - Without `--status`: `statuses` null and `selected == titles`.
    - `batch` and `mdlist` documents are byte-identical to before.
12. `[unit]` `Errc::login_required` → 8 and `auth_rejected` → 9. `is_fatal` and every switch cover them.
13. `[cli]` First run with prompts. The stdin and stderr TTY flags are set, and a fake prompter answers the client id, secret, user and password; fixture F1.
    - exit 0, `clean`'s paths on stdout;
    - the prompts come in order, with the client hint first;
    - the session file exists at 0600 in a 0700 directory and contains no password;
    - a second run asks nothing and sends 0 or 1 token POSTs.
14. `[cli]` No TTY, no session, no credentials → exit 8 with the login message. Empty stdout, no JSON (also with `--json`), DIR not created, no request sent.
15. `[cli]` `--no-input` with the TTY flags set and no session → exit 8, and the prompter is never called.
16. `[cli]` Credentials file permissions:
    - a file at 0644 → exit 1 with the `chmod 600` hint;
    - a 0600 file with all four keys → exit 0, no prompts;
    - an explicit `--credentials` path that is missing → exit 1.
17. `[cli]` A wrong password (a fixture `expect` mismatch) → exit 9 with the password message. A wrong secret → 9 with the client/approval message. The session is unchanged.
18. `[cli]` A session whose refresh is answered `invalid_grant`:
    - with a password in the credentials file → a silent password login, exit 0, the session rewritten;
    - without one → exit 8 with the "expired" message.
19. `[cli]` Usage errors, all exit 2 with empty stdout and no request:
    - `follows ''`, `follows <uuid>`, `--title X`;
    - `--password hunter2`, `--password=hunter2`, `follows out hunter2`;
    - `--api-base https://example.org`;
    - `--password-stdin` with an empty stdin.

    **`hunter2` appears nowhere on stderr.**
20. `[cli]` `-v` prints `logged in to MangaDex as fixture-user (…)`, `follows: n titles` and `follows:1: <uuid>`. `-vv` traces show the token URL and the follows URLs. **Neither stdout nor stderr contains the fixture access token, refresh token, secret or password** (grep for each).
21. `[cli]` A simulated interrupt during the collection fetch → no JSON, nothing in DIR, the session as last written. An interrupt at the password prompt → the interrupted path, the prompter's echo-restore hook called, no session written.
22. `[cli]` The session save fails (the state directory is a regular file) → a warning, and the run still exits 0. `login` in the same situation exits 1.
23. `[cli]` Two session files in one test: a fixture login never touches `session.json`, only `fixture-session.json`.
24. `[cli]` `login` and `logout`:
    - `login` with the fake prompter → 0, empty stdout, the "logged in … saved in" line;
    - `logout` → the revoke POST sent, the file removed, 0;
    - `logout` again → "not logged in", 0;
    - a failing revoke → a warning, the file removed, 0.
25. `[cli]` A world-writable state directory or a symlinked `session.json` → a warning. The session is not used and nothing is written through the link.
26. `[cli]` A lock held by another file descriptor → the run waits, then exits 1 after the timeout (shortened for the test). Once the lock is released, it proceeds.
27. `[e2e]` Help goldens:
    - new `follows.help` (including `--status` and the **rev2** READING STATUS section), `login.help` and `logout.help`;
    - a re-bless is requested for `download.help`, `batch.help`, `mdlist.help`, `edge.help` and the root help (exit rows 7–9, the commands list);
    - no `--api-base` in any help.
28. `[e2e]` No session, stdin `/dev/null` → exit 8, the exact message, empty stdout, `out` absent.
29. `[e2e]` A committed 0644 credentials file via `--credentials {{INPUTS}}/follows/credentials-0644` → exit 1 with the chmod hint, with no value in it.
30. `[e2e]` The positive path (it needs the driver feature from VERIFY C9, or a preceding `login` case that seeds the session):
    - F1 → 0, the paths, sound EPUBs, no `.part-`;
    - `--json` has `source: "follows"` and the `follows` object;
    - `--dir-per-title` puts files under `out/Clean Title/`.
31. `[e2e]` Aggregates and empties:
    - F2 → 4, with `follows:2: 6666…: … not found`;
    - F7 → 7;
    - F3 → 0, the "follow no titles" warning, `titles: []`;
    - F4 → the duplicate warning.
32. `[e2e]` `--password-stdin` via `STDIN follows/password.txt`, with a fixture credentials setup → 0. With `STDIN NULL` → 2.
33. `[e2e]` `--dry-run` → the planned paths, `out` absent. Goldens `follows.dry-run` and `follows.json-dry-run` are requested.
34. `[edge]` `STDIN CLOSED` with a seeded session → 0; without one → 8. Never 70, never a hang.
35. `[edge]` `--dry-run > /dev/full` → 1 plus `mangadex-dl: write error`.
36. `[edge]` F6 `--dry-run --layout chapters | head -1` → 141, also with the parent ignoring SIGPIPE.
37. `[edge]` `--json` with `CLICOLOR_FORCE=1` → no ANSI. Piped stderr with a failing item → no ANSI.
38. `[edge]` An invalid-UTF-8 DIR under `LC_ALL=C` is used verbatim; with `--json --dry-run` the JSON is valid. An invalid-UTF-8 extra argument after `follows` → exit 2, not echoed, not 70.
39. `[tty-pty]` (cli-tester, `script` with piped keystrokes) Under a pty:
    - the client hint and the four prompts appear;
    - the secret and the password are **not echoed**;
    - after the run, `stty -a` shows `echo` restored;
    - the progress line `(1/2) …` comes after the prompts, and there is none with `--json`, `-q` or `TERM=dumb`.
40. `[tty-pty]` Ctrl-C (SIGINT through the pty) at the password prompt → 130, echo restored, no session file.
41. `[human]` A real login with the user's own personal client: `login`, then `follows ~/Manga --dry-run`, then a real run. The follows count matches the website; note the order (closes B1 and B2).
42. `[human]` One deliberately wrong password, only once because of brute-force lockout → exit 9 with sensible wording. Record the raw `error` and `error_description` with `-v` (closes A5).
43. `[human]` If one is available, a not-yet-approved client: record what MangaDex answers, and check that the message mentions approval (closes A5's pending case).
44. `[human]` Run `follows` again after more than 15 minutes, then after several days: a silent refresh, and a rotated token saved (compare `refresh_expires_at`). Note `refresh_expires_in` (closes A3 and A4).
45. `[human]` `logout`, then `follows` from cron (no TTY) → exit 8 with the login hint. Then `login` in a terminal, and cron works again.
46. `[human]` A large follow count (100 or more, more than 500 if available): pagination is complete, there is no 429 storm on the auth host, and authenticated requests happen only at the start (check `-vv`).
47. `[human]` Typing at the real prompts in the user's terminal emulator: no echo, pasting works, Ctrl-C and Ctrl-D behave. Ctrl-Z then `fg` at the password prompt: note whether echo comes back (a known termios caveat; documented if not).
48. `[human]` **A1 (rev2):** run `follows --status reading --dry-run` on the real account. Its titles are exactly the followed titles shown with status Reading on the website. A title that has a status but isn't followed does not appear. A followed title without a status does not appear (closes B5, D4 and D5 in practice).
49. `[human]` `ps -ef` and `/proc/<pid>/cmdline` during a run show no secret. A deliberate crash, if one can be provoked, leaves no core file with secrets (closes C7 in practice).

**A1, `--status` (rev2):**

50. `[unit]` `StatusFilter`:
    - `reading`, `on-hold`, `plan-to-read`, `re-reading`, `on_hold,reading` and repeated flags are normalised, and duplicates are removed keeping the first occurrence;
    - `Reading`, `all`, `paused`, `''`, `reading,` and `,` get the exact exit-2 messages;
    - an unknown word is echoed through `terminal_safe`, cut to 32 bytes.
51. `[unit]` `parse_status_map`:
    - accepts the documented shape and the empty `[]` form;
    - canonicalises an upper-case key;
    - keeps a well-formed unknown value (`paused`);
    - rejects, with Errc::network and fixed text: non-JSON, a `result` other than `ok`, missing `statuses`, `statuses` as a non-empty array or a string, a key `../x`, a 35-character key, a numeric or null value, a 33-character value, a value with an upper-case letter or a dash, 100 001 entries, more than the separator cap, and more than 16 MiB.
52. `[unit]` `filter_follows` over FS1:
    - `reading` → `[1111…]`;
    - `reading,completed` and `completed,reading` → both `[6666…, 1111…]`, the follows order, independent of the word order;
    - `8888…` (no status), `7777…` (`paused`) and `9999…` (not followed) are never kept;
    - the counts are `titles = 4`, `selected` = the kept count, and unknown = `{paused: 1}`;
    - duplicate follows survive the filter and are then marked.
53. `[unit]` `fetch_status_map` over FakeTransport:
    - the URL is exactly `<api>/manga/status`, with no query, and `Authorization: Bearer <t>`;
    - an expiring token is refreshed before the request;
    - a 401 → refresh plus one retry; a second 401 → auth_rejected with the "reading statuses" wording;
    - 429 and 5xx are retried, then network with the remedy;
    - the 16 MiB cap is enforced;
    - a stop → interrupted.
54. `[unit]` With `--status`, the follows pages come first, then exactly one status request. With zero follows, no status request is sent.
55. `[cli]` FS1 `--status reading`:
    - exit 0, `clean`'s paths on stdout;
    - `-v` shows `follows: 4 titles`, `follows: 1 of 4 have the reading status reading`, the unknown-status note for `paused`, and `follows:1: 1111…`;
    - `--json` has `follows.statuses: ["reading"]`, `titles: 4`, `selected: 1`, and `titles[0].line == 1`.
56. `[cli]` FS1 `--status reading,completed` → exit 4. Item 1 is `follows:1: 6666…: … not found`, item 2 is `1111…`, and the closing line says `1 of 2 titles did not complete (item 1)`.
57. `[cli]` FS2 with `--status reading` → exit 0, the warning `none of the titles you follow have the reading status reading`, `titles: []` with `selected: 0`, and DIR not created. F3 (no follows) with `--status reading` → the "follow no titles" warning, and no status request.
58. `[cli]` FS3a or FS3b → exit 3 with the malformed message (no network remedy), no JSON, `out` absent.
59. `[cli]` Usage: `--status all`, `--status paused` and `--status ''` → exit 2 with no request (the fixture root is never opened). `login --status reading` → exit 2 (no such option).
60. `[e2e]` FS1 `--status reading`, using the seeding or driver feature from AC30 → 0, the paths, sound EPUBs. `--status reading,completed --json` → valid JSON with `statuses` and `selected`. `--dry-run --status reading` → the planned paths, `out` absent.
61. `[edge]` `--status` with an invalid-UTF-8 word under `LC_ALL=C` → exit 2, the word passed through `terminal_safe`, never 70. `--json --status reading` with `CLICOLOR_FORCE=1` → no ANSI.
62. `[human]` Change a followed title's status on the website and run again: the title moves in or out of the filtered run as expected, and its files stay where they are.

## Out of scope

- Credentials from env vars (additive later, ranked above the file).
- `whoami` / `status` commands.
- **rev2:** the Library-by-status set, i.e. titles with a reading status that are not followed. It is additive later as a separate option. So are an `all` value for `--status` and per-status output directories.
- The `status` query parameter of `/manga/status`.
- Authenticated `mdlist` for private lists. That is additive and opt-in later; the anonymous private-list 404 → exit 4 is confirmed live and stays.
- Public clients and the authorization-code and device-code flows. device_code is listed in the realm's grants, but whether personal clients can use it is unknown.
- MFA: personal clients bypass it, per MangaDex, and the README says so.
- Following or unfollowing, changing reading statuses, marking chapters read, `/user/follows/manga/feed`.
- Several accounts per run, storing secrets in a system keyring, parallel titles, the man page and completions.

## Open questions

QA1 is resolved (b) and there is no open gate question.

**[VERIFY] items, clustered for the fact-checkers.** The fact-checker must not POST to `auth.mangadex.org`, must not send any credential (real or fake), and must not call the authenticated endpoints. Live shapes are closed by the human ACs.

**A. MangaDex auth (Keycloak)**
- **A1:** The token endpoint path is CONFIRMED by the discovery document and the docs. mangadex-full-api comments that the documented refresh URL didn't work for it; both grants use `/realms/mangadex/protocol/openid-connect/token`.
- **A2:** `client_secret_post` with form fields for both grants is CONFIRMED (docs, plus the discovery document's `token_endpoint_auth_methods_supported`).
- **A3:** The response fields `expires_in`, `refresh_expires_in` and `token_type`, and whether the refresh grant rotates `refresh_token`. Third-party code only (AC44).
- **A4:** Lifetimes: access 15 min (docs). The refresh-token lifetime is unknown (third-party says about 1 month), and so is whether `refresh_expires_in: 0` occurs (AC44).
- **A5:** Error shapes and statuses (AC42–43) for:
  - a wrong password (expected 401 or 400 `invalid_grant` "Invalid user credentials");
  - a wrong secret (401 `invalid_client`);
  - a pending or unapproved client;
  - a disabled account or a brute-force lockout;
  - an expired or revoked refresh token ("Token is not active" / "Session not active").
- **A6:** Rate limits and 429 headers on `auth.mangadex.org`. Only the legacy `/auth/login` (30/h) and `/auth/refresh` (60/h) are documented.
- **A7:** The revocation endpoint accepts `token` + `token_type_hint=refresh_token` + client credentials and answers 200. The endpoint is CONFIRMED in discovery; the behaviour is not.
- **A8:** The token charset is JWT/base64url and the size is far below 16 KiB.

**B. MangaDex API, follows**
- **B1:** The `/user/follows/manga` response is a `MangaList` with `total`, and `limit` ≤ 100 (CONFIRMED by the spec). The 401 body shape for a missing or expired token is not confirmed.
- **B2:** The follows order (follow date? stable across pages?), and whether content-rating filters apply (are pornographic follows returned without `contentRating[]`?).
- **B3:** Whether the 10 000 offset window applies to this endpoint.
- **B4:** Every followed title can be downloaded anonymously: no title needs auth for `/manga/{id}`, the feed or at-home.
- **B5 (rev2):** Whether a followed title can lack a reading status in practice (does following set one?), which decides how often "left out" happens (AC48).
- **B6:** A 403 on the follows or status endpoint: a ban, or a permission body?

**D. MangaDex API, the status map (A1)**
- **D1:** The exact `GET /manga/status` response is `{"result":"ok","statuses":{"<uuid>":"<status>",…}}`. Is an empty map serialised as `{}` or `[]`? The design accepts both.
- **D2:** Without `status`, does the endpoint return every status? With it, only that status? The design never sends it.
- **D3:** Is it really unpaginated and uncapped? How large does it get for big accounts (behind the 16 MiB and 100 000-entry caps)? Are the ids lowercase?
- **D4:** Is the value set exactly the six spec values, with no others in practice?
- **D5:** Does the map hold the status of every followed title that has one, across content ratings, including titles that are no longer available?
- **D6:** The 401 shape for a missing or expired token on this endpoint.
- **D7 (installed CLI11 2.7.x):** `add_option` on a `std::vector<std::string>` with `->delimiter(',')` splits `reading,on_hold` and also accumulates a repeated `--status`. Check how an empty element (`reading,`) arrives, and that the help's type name is `WORD,...` with no default shown.
- **D8 (installed nlohmann):** What `parse_untrusted` does with duplicate object keys (does the last silently win, or can it be detected?), and whether the separator and depth guards cover a 100 000-entry object within the cap.

**C. Repo and toolchain**
- **C1:** In the installed CLI11 2.7.x, `ExtrasError::what()` lists the extra arguments verbatim, and `remaining(true)` shows them once `follows`, `login` or `logout` is reached. That makes the value-free extras policy both needed and implementable.
- **C2:** The shared option binders register on a fourth subcommand without `ConstructionError`. The help order matches `mdlist`'s, with the four new options last (**A1:** `--status` first of them).
- **C3:** The curl transport can attach an `Authorization` header to GETs (a new slist path). `CURLOPT_FOLLOWLOCATION 0` stays. libcurl never logs headers without `CURLOPT_VERBOSE`.
- **C4:** Fixture transport: the POST form routing, the thread-safe per-token counter, and the two-path allowlist mapping `api/manga/status` to `status.json`.
- **C5:** `ScopedInterruptHandlers` installs with `SA_RESTART`, so the prompt reader must poll. Confirm the pending-interrupt query API in `interrupt.hpp`.
- **C6:** `explicit_bzero`, `flock`, `mkostemp`, `renameat` and `openat` exist in the conda sysroot (glibc 2.28 headers).
- **C7:** `prctl(PR_SET_DUMPABLE, 0)` under the dev preset's ASan/LSan. LSan's StopTheWorld uses ptrace, so probe it with `cxx-probe` in the real context. If it breaks LSan, use `RLIMIT_CORE=0` and check that systemd-coredump honours it.
- **C8:** termios `~ECHO` with `TCSAFLUSH` on fd 0, restored on every path, exceptions included.
- **C9:** The E2E driver (`tests/e2e/CliE2E.cmake`, shared) can't create a 0600 file from a committed input. Decide between a driver option (a serialization point) and keeping the positive path at the CLI tier only.
- **C10:** `EnvSnapshot` / `read_env` gain three strings (no longer trivially `noexcept`); `io.hpp` is shared.
- **C11:** `-Wswitch` finds every `switch` over `Errc` and `BatchSource`.
- **C12:** `with_remedy` must not append the network remedy to the auth-rejected, login-required or content-failure messages.

**Contract changes outside `follows`, `login` and `logout`.** Everything else in `download`, `batch` and `mdlist` stays byte-identical. The changes are:
- the shared help footer: row 7 reworded, rows 8 and 9 added (every help golden changes);
- the root `unknown command` list;
- the root help's command list.

A1 adds no change outside `follows`.

**Relevant paths:**
- `/home/user/Mangadex-dl/docs/design/follows-spec.md` (rev1, which this revises)
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