# FloServer: handoff (2026-10-29, ~13:10 Baghdad)

Start a new chat with: "Read floserver-handoff.md and continue FloServer."
(Repo copy: `docs/handoff.md`; the master is `floserver-handoff.md` in the claude.ai project "Flo Server". A session without that project reads this copy and `docs/design.md`, and `CLAUDE.md` for the rules.)

## Status
- Design note approved, all of D1–D16; Q1–Q8 decided 27 Oct and recorded in the note: https://claude.ai/artifact/7vJVpQSFLnty5yz8nUM4Hg
- Mockups approved 27 Oct: https://claude.ai/artifact/5hKStiT4gyDZKjXnj8twnP (sizes and memory figures are placeholders).
- Repo: private `hostkurd/floserver`.
  - Merged into `main`: PR #1 skeleton, #2 signed manifest, #3 built-in key + CDN address, #4 `floserver-manifest build` + CDN check script, #5 daily live-manifest CI check, #6 step 2 (service + private channel; tested by the user on Windows 28 Oct), #7 CLI short name `flo` (merged 28 Oct), #8 step 3 downloader and runtime installs (merged 29 Oct; the user tried the Windows build and everything worked).
  - **PR #9 step 4a merged into `main` (28 Oct).** Project list, site plan, nginx config, `projects.*`, `flo projects/add/remove/use`.
  - **PR #10 step 4b** (local CA + certificates) merged into `step4-projects`, then **PR #11** carried it into `main` (merged 29 Oct, `main` 07d23f9).
  - **PR #12 step 4c merged 29 Oct** (`main` 7f985ef). Running projects: nginx + PHP pools with sleep when idle, `flo start/stop`, `web.*`.
  - **PR #13 step 4d MariaDB merged 29 Oct** (fdea06d).
  - **PR #14 step 5a window merged 29 Oct** (`main` c9f7bee). Review fixed; tried on the user's PC. AR/KU strings sent for a native read; open choice: Off = کوژاوە or وەستاوە. Follow-ups: screen reader support (UI Automation), relaunch during a slow Quit does nothing.
  - One unexplained CI failure: "Linux (clang, ASan + UBSan)" Test step failed once on 8d0192a (merge of main into step5-window) and passed on the next run; logs weren't readable then. Linux test failures now become annotations (`Show failed tests as annotations`), so next time read them with `gh api .../check-runs/<id>/annotations`.
  - Whether the user ran the 5 `flo trust`/`flo untrust` steps on Windows (in PR #10's description; build from CI run https://github.com/hostkurd/floserver/actions/runs/40785375559) is **not confirmed yet**; asked 29 Oct.
  - **PR #15 merged 29 Oct** (by the user): fixed the flaky `test_web_service.cpp:242` that failed the ASan job on `main` (nginx's old worker can take one more connection right after reload under ASan; the test now waits up to 3 s). Next PR number **#16**.
  - **Python (D17) written into the design note 29 Oct as "proposed, waiting for approval".** User's answers: FastAPI most important (Claude added Django + Flask), Python 3.13 + 3.12, pip (uv maybe later), in 1.0. Next: user approves D17 (and optionally sees mockup additions: Python chip, FastAPI template card), then build in step 4e with Node. After approval, also update the note's summary line (PHP, Node.js, Python…) and Q8 (five templates).
  - (Original request, 29 Oct: Python development support with venv.) Not in the approved design: needs a design-note addition (new D17) + mockup changes + the user's approval before building. Questions put to the user: frameworks (Django/Flask/FastAPI), Python versions, pip or uv, 1.0 or 1.x. Proposed shape: Python runtimes from python-build-standalone (relocatable, includes venv/pip) on the CDN; `"python": "3.13"` in floserver.json; FloServer makes `.venv` in the project and installs requirements; nginx proxies to the app's dev server on a FloServer-chosen port (same model as Node, D11), sleeping when idle; terminal shims use the project's venv.
  - **`flo trust` on Windows confirmed working** (29 Oct, on the PC).
  - Windows builds come from each CI run's Artifacts section (the sandbox can't download them: Actions blob storage is blocked by the proxy, so the user downloads from the run page).
- Signing key `floserver-2026` was made offline on 27 Oct (public `jwJF3yZAweyJCvFKbZvb/IjJRjlBbnDiEU3NloC38Cc=`). The secret file stays with the user.
- CDN live at https://cdn.floserver.org: bucket `floserver-cdn-origin` (eu-central-1), CloudFront only, direct S3 refused. File list and hashes are in `claude/floserver-cdn-files.md`.
- **Published runtime list:** sequence 1, 11 runtimes, expires 2027-01-26T12:27:07Z. The next list must use **sequence 2 or higher**. The daily CI job warns 14 days before expiry. Composer is listed with platform `any`.

## Step 4 plan (told to the user 29 Oct, no decision changed)
Four PRs, in order:
1. **#9 (step 4a)** project list and web config. Merged.
2. **#10 → #11 (4b)** local certificate authority + per-project certificates with Windows' own crypto (CNG, no OpenSSL), CA added to the *user* Root store (Windows asks once). Plan: give the CA a **name constraint** (permitted DNS `localhost`), so it can only sign `localhost`/`*.localhost`. Tell the user in the PR that this is within D7. CA key protected with DPAPI in the app folder; leaf keys as PEM for nginx. "Remove FloServer" removes the CA (D7).
3. **#12 (4c)** running things: nginx start/reload/stop, php-cgi pools with sleep when idle (Q1), `flo start/stop`, addresses with real ports. Add a **CI test on Windows** for the open Q1 point (php-cgi started with the listening socket as stdin, only that handle inherited via PROC_THREAD_ATTRIBUTE_HANDLE_LIST), using php from the CDN, before asking the user to test.
4. **#13 (4d)** MariaDB per version: first setup, a database and user per project, passwords in Credential Manager, "Copy .env lines".
Also still to place: Apache projects (nginx must proxy them, since only one server can hold 80/443), Node projects (proxy to the dev server), terminal shims (D10), Studio file (D14).

**Detail settled within D10/D11 (told to the user):** a PHP pool is one PHP install **plus one extension set** (php-cgi reads one php.ini). Projects with the same pair share it; a shared pool sleeps only when none of its projects has Keep running.

## Merge rule and parallel work (user's decisions, 29 Oct)
- Claude merges a PR itself when **all CI checks pass and the independent review has nothing open**, and tells the user each time.
- Step 5 (window) is built in parallel with step 4 on separate branches from `main`; no stacked PRs.

## Step 4d design (PR #13), implementation details within D11/D12
- `secret_store.h`: Windows Credential Manager (generic credential `FloServer:<name>`, CRED_PERSIST_LOCAL_MACHINE); POSIX `<data>/secrets/<name>` 0600. `generate_password()` 24 chars, no look-alikes.
- `mariadb.h`: files per series `<data>/data/mariadb-<series>/` (my.ini|my.cnf, `installed` marker written only after install-db succeeds, `port`, `root-password-set`, data/ with mariadbd.err, mariadbd.pid, mysqld.sock, init.sql briefly). Config: `datadir` full path (a relative one is taken from basedir), pid/log/socket/init-file relative to it (socket: Linux limit 107 bytes), no basedir on Windows, bind 127.0.0.1, utf8mb4, 64M buffer pool, skip-name-resolve. SQL: root password on root@localhost/127.0.0.1/::1, other root and anonymous accounts deleted from mysql.global_priv; per project `CREATE DATABASE/USER IF NOT EXISTS` + ALTER + GRANT on its db only. Names `-`→`_`; **reserved names refused** (root, mysql, sys, performance_schema, information_schema, test, mariadb_sys, public). `.env` FloCMS (DB_TYPE/DB_NAME) or Laravel (DB_CONNECTION/DB_DATABASE) keys; `update_env_text` keeps line endings, replaces the first line of a key (also `export KEY=`), drops duplicates, appends missing.
- `database_service.h`: one Server per series. The service holds the public port (saved per series; 3306 first, never another stack's) and relays to mariadbd on an internal port (33100+, unique across servers, re-picked at start when taken). First connection starts it; idle (no connection, no activity) → `mariadb-admin shutdown` (MYSQL_PWD only) on a stopper thread, never under locks. First start: install-db (half-set-up folder moved to `data.failed-<time>`), root's password through init-file, kept until a real root login works (MariaDB listens before reading it). Lost/wrong root secret → new password through the init file, once. Retired servers never start again; their port is waited for. Logins: one at a time per server, count as a connection. Secret SQL files removed on every exit path and swept at start. Status `{"servers":[{series, version, port, internal_port, state, pid?, connections, projects, keep_running, error?}]}`.
- Protocol `databases.list`, `databases.login {name}` (login + env lines + env_file). Daemon reloads databases after every `projects.*`.
- CLI `flo db`, `flo env [project] [--show] [--write]` (password hidden unless --show; --write keeps .env's permissions and the folder's).
- MSVC lesson: the Windows headers `#define small char` (rpcndr.h); never name anything `small` in Windows code (MinGW doesn't catch it).
- device_commit_files lesson: committing the same staged path again can send a cached copy; check the size on the PC with device_list_dir, or stage under a new path.
- Lessons: Linux TIME_WAIT after the relay closes makes the "is it free" probe fail → open the saved port directly; MariaDB on Windows creates data\my.ini itself (harmless); mariadb-install-db on Windows takes `--datadir=data` relative to its cwd.
- Not covered yet: Windows CI doesn't run the MariaDB test (the PC run stands in); non-ASCII data folder on Windows untested (relies on MariaDB 10.6+ running with UTF-8 as its code page).

## Step 5a window (branch step5-window)
- Plain Win32 + GDI/GDI+, model in `src/ui-win32/model` (strings EN/AR/KU, view model, theme; unit-tested on Linux). Polls the service every 2 s; switches send `projects.set`. Tray, single instance per service instance, DPI v2, RTL via WS_EX_LAYOUTRTL. `FLOSERVER_LANG=ar|ku`, `FLOSERVER_THEME=light|dark` for tests.
- Found on the PC: DrawText draws Unicode isolates (U+2066–2069) as boxes → LRE/RLE…PDF embeddings; the system cue banner was unreadable on dark → placeholder owner-drawn.
- Differences from the mockups (agent's list): no nginx/Node chips (service doesn't report per project), "Needs download" is a label not a button (downloads not in 5a), no "Waking…", footer "Service memory", Off projects show their folder, New project does nothing yet ("Coming soon"), opens 960×640, red banner when the service stops instead of a silent restart, owner-drawn rows not read by screen readers yet.

## Step 4c design (PR #12), implementation details within D6/D10/D11/Q1
- `child_process.h`: `ChildProcess` (Windows: job object KILL_ON_JOB_CLOSE, created suspended then assigned, STARTUPINFOEX handle list with only the named handles, CREATE_NO_WINDOW, own env block; POSIX: posix_spawn, own process group, `closefrom_np(3)`, kill = TERM then KILL and wait until the group is empty). `LoopbackListener` (127.0.0.1, SO_EXCLUSIVEADDRUSE / SO_REUSEADDR + CLOEXEC), `waiting_connections()` (select/poll), `drop_waiting_connection()`.
- **One php-cgi per port (found on the user's Windows PC 29 Oct):** several processes accepting on one shared socket lose connections on Windows (PHP_FCGI_CHILDREN=4 → 3 in 4 requests hang; also 4 separate php-cgi on one socket). So each pool has `kPhpWorkers` (4) ports, one php-cgi (PHP_FCGI_CHILDREN=0) per port, replaced when it ends; nginx `upstream floserver_php_<first port> { server 127.0.0.1:p max_fails=0; ... }`. `FLOSERVER_PHP_WORKERS` for tests. Recorded in the design note's Q1.
- **Reload wait:** `nginx -s reload` is asynchronous; the default server answers `/.floserver/config-id` with an id (SHA-256 of the config, 16 hex) and reload() waits until it answers 5 times in a row (≤ 5 s).
- **Idle + open connections:** a pool sleeps only when no connection to its ports is open (GetTcpTable / /proc/net/tcp).
- `php_pool.h`: php.ini per pool in `<data>/web/pools/<key>/php.ini` (512M limits, display_errors, opcache, error_log `<key>.php-errors.log`); Windows: `extension_dir` + **base extensions** (curl, fileinfo, gd, intl, mbstring, mysqli, openssl, pdo_mysql, pdo_sqlite, sodium, zip when present) + listed ones (missing listed one = problem); POSIX: no extension lines. php-cgi `-c php.ini` (relative, cwd = pool folder), env PHP_FCGI_CHILDREN=0, PHP_FCGI_MAX_REQUESTS=500, PHPRC/PHP_INI_SCAN_DIR removed, stdin = that worker's held socket, no stdout/stderr (php-cgi needs INVALID_HANDLE_VALUE on Windows). Idle from `<key>.access.log` size (truncated over 8 MB); default 10 min; 3 exits within 5 s of start in a minute → Failed for 30 s, waiting connections dropped (fast 502).
- `web_service.h`: `reload()` under one mutex (load config, plan, pools keep ports, certs when CA exists and isn't NotTrusted, ports picked when nginx starts and kept while it runs, config written to `nginx.next.conf`, `nginx -t -q`, rename, `-s reload` or start with `-p <web>/ -c nginx.conf` (+ `-g "daemon off;"` on POSIX)); stops nginx when nothing is served; stale nginx stopped via pid file. Supervisor thread: select 200 ms on non-running pools (shared_ptr copies), tick every 1 s, reload every 5 s, restart a dead nginx (10 s backoff after a failed start).
- nginx config additions: `http2_directive` (false for nginx < 1.25.1 → `listen ... ssl http2`), `user root;` only when euid 0.
- Env for tests/dev: `FLOSERVER_WEB_PORTS=18080,18443`, `FLOSERVER_IDLE_SECONDS`.
- Measured (Linux release): service 6.9 MB with pools running or asleep; first request after sleep ~0.35 s under ASan; 50/50 OK in a burst during wake.
- Known gap: on POSIX a crashed service leaves nginx/php-cgi running (Windows job objects end them); nginx is stopped at next start via pid file, leftover php-cgi isn't (for the macOS/Linux stage).
- Testing pitfall: never `pkill -f floserver-test-` (it matches the shell's own command line and kills it); use `pkill -x nginx; pkill -x php-cgi`.

## Step 4b design (PR #10), implementation details within D7
- `der.h`: small DER writer (tlv, integers, OIDs, UTCTime/GeneralizedTime, context tags, PEM). No parser: certs are read back via `ca.json` (base64 DER, fingerprint, key_id, not_after).
- `crypto.h`: `random_bytes`, `EcKeyPair` (P-256, d wiped on destruction via monocypher `crypto_wipe`), `generate_ec_key`, `ecdsa_sign_sha256` (raw r||s), `ecdsa_verify_sha256`, `protect_secret`/`unprotect_secret`. Windows: CNG (bcrypt) + DPAPI (`CRYPTPROTECT_UI_FORBIDDEN`); POSIX: OpenSSL 3 (`FLOSERVER_HAVE_OPENSSL` when CMake finds it; otherwise stubs refuse), protect = identity with 0600 files.
- `certificates.h`: CA = ECDSA P-256, 10 years, basicConstraints CA pathlen 0 (critical), keyUsage keyCertSign|cRLSign (critical), **EKU serverAuth**, **critical Name Constraints** permitting dNSName `localhost`, iPAddress 127.0.0.0/8 and ::1/128, rfc822Name and URI `localhost`, directoryName `O=FloServer`; SKI. Subject `O=FloServer, CN=FloServer local CA <8 hex>`. Leaf = 397 days (never past the CA), SAN `<name>.localhost`, EKU serverAuth, keyUsage digitalSignature, SKI/AKI, subject `O=FloServer, CN=<name>.localhost`. Renewed 30 days before the end or when the CA fingerprint changed.
- Files: `<data>/certs/ca.pem`, `ca.key` (DPAPI-sealed d||x||y on Windows), `ca.json`; `sites/<name>.pem` (leaf + CA), `.key` (PKCS#8 PEM, for nginx), `.json`. `write_private_file_atomically` (POSIX: 0600 + O_EXCL + folder 0700).
- CA is made only when there is none; expired or damaged → error "flo untrust, then flo trust" (never replaced silently). `ensure_site` holds one lock throughout. `certs.status` returns `expires_soon`.
- `trust_store.h`: Windows user Root store (`CertAddEncodedCertificateToStore` with USE_EXISTING, find with CERT_FIND_EXISTING, delete); refuses a CA without critical Name Constraints and EKU = serverAuth only. Called only from the CLI (Windows shows a prompt), never from floserverd. POSIX: Unsupported (macOS/Linux later).
- Protocol: `certs.status`, `certs.ensure_ca`, `certs.remove` (replies carry the public DER, never key material). CLI: `flo trust` (mentions Firefox `security.enterprise_roots.enabled`), `flo untrust` (removes from store, then files).
- Tests verify with the platform verifier and only our CA trusted (OpenSSL with X509_STRICT on Linux; CryptoAPI chain engine with exclusive root on Windows): shop.localhost ok; wrong host, expired, www.example.com, lookalike, other organisation, email, client login, code signing (Windows) all refused.
- Left for later: `OSSL_PARAM_clear_free` (OpenSSL 3.2+); Windows usage property on the root entry (not needed while the CA carries EKU).

## Step 4a design (PR #9), implementation details
- **config.json** (`project_registry.h`): `{"projects_dir"?, "projects": [{"name","path","on","keep_running", ...}]}` under the data folder. Unknown keys kept at top level and inside each project (`Project::other`). Damaged file → refused (`config_unreadable`), never overwritten. Non-boolean `on`/`keep_running` is an error. At most 500 projects.
- **Names**: DNS label a-z 0-9 `-`, ≤ 63, not starting/ending with `-`, not `xn--`. From the folder name (`My Shop` → `my-shop`, then `-2`…); non-ASCII-only → `project`.
- **Folders** stored by `real_folder()`: `fs::canonical` (long names, real case), `\\?\` stripped; on Windows only drive letters (**network shares refused**). Same folder twice (also through a link) refused.
- **Site plan** (`site_plan.h`): per project reads floserver.json (`read_project_config_file`: plain file only, ≤ 64 KiB, errors prefixed "floserver.json:", `root` through a link leading outside the project refused). Newest installed matching PHP, NTS only; default = newest installed (a Settings default later). States off / ready / needs_download / problem. Apache and Node-only projects are "problem: can't run in this build yet". A doc root nginx can't quote → that project alone is a problem. Pools keyed `php-<version>+ext+ext`; ports from 9100; a pool in `previous` keeps its port without probing (the service holds it); retired pools' ports aren't reused in the same reload.
- **nginx config** (`nginx_config.h`): `worker_processes 1`, pid/logs/temp under our work folder (the service must create `logs/` and `temp/` first; nginx makes only the last folder), `include` of the runtime's `conf/mime.types` and `conf/fastcgi_params`, `sendfile off`, `client_max_body_size 512m`, `server_names_hash_bucket_size 128`. Listen 127.0.0.1 and [::1] only when `ipv6` is set (default off; set from `loopback_ipv6_available()`, because nginx won't start with [::1] on a machine without IPv6). Default server: 404 text on http, `ssl_reject_handshake` on https. Per site: http→https 301 when it has a cert, else served on http; `http2 on`; front controller `try_files $uri $uri/ /index.php$is_args$args`; caseless `~*` locations for dot files (404) and PHP; PATH_INFO kept with `set $floserver_path_info` before `try_files`; `HTTPS on` param; `fastcgi_read_timeout 300s`; PHP requests logged as `$msec` to the pool's idle log. Paths quoted with `/`; `"`, `$`, `\`, control chars refused (`{ } ; #` are fine inside quotes, checked in real nginx).
- **Idle log** grows: in #11 the service measures idle from it and truncates it.
- **Service** (`project_service.h`): re-reads config.json on every request, one mutex for load-change-save. Protocol `projects.list`, `projects.add {path, name?}`, `projects.remove {name}`, `projects.set {name, on?, keep_running?}`. Off projects still show what they'd need (`missing`).
- **CLI**: `flo projects`, `flo add [folder] [--name n]`, `flo remove <name>` (files untouched), `flo use <php|node|mariadb> <version>` (edits the registered project's floserver.json when inside one, else the nearest one or a new one here; keeps other keys and order).
- `ports`: `loopback_ipv6_available()` (bind [::1]:0).

## Decisions taken 27–28 Oct
- Q1 sleep when idle: the service owns each pool's listening socket for good. On the first connection it starts php-cgi with that socket as stdin, which is how php-cgi starts its own children on Windows (hStdInput = socket, stdout/stderr INVALID_HANDLE_VALUE). Idle time comes from the pool's nginx access log. Linux prototype: first request after sleep 69–94 ms, 50/50 OK during wake, warm requests unchanged at 6.6 ms. Rebinding gave a 502 every time and was rejected. Still to check on real Windows: inheriting only that handle (PROC_THREAD_ATTRIBUTE_HANDLE_LIST). PHP sources: main/fastcgi.c around fcgi_is_fastcgi; sapi/cgi/cgi_main.c Windows children loop.
- Q2: repo hostkurd/floserver; CLI `floserver` plus a short copy named **`flo`** (changed from `fls` on 28 Oct). FloCMS's own CLI is `php flo`, a PHP script, so they don't clash.
- Q3: signing certificate doesn't block the build; seller and key holder chosen before the beta.
- Q4: try 80/443 first, else 8080/8443, 8081/8444… shown in the address.
- Q5: app updates through the same signed manifest, daily check, user clicks Update.
- Q6: no telemetry in 1.0; opt-in crash reports considered for 1.x.
- Q7: MariaDB only in 1.0; MySQL 8 later through the manifest if customers ask.
- Q8: templates in 1.0: PHP app, FloCMS, WordPress, Node app. The rest follow through the manifest.
- **Installer size (changed 28 Oct, recorded in the note's budget table and D15):** no hard 5 MB limit. The goal is "lightweight"; CI raises an alarm above 15 MB (look at why, not a failed PR); when size and code quality conflict, quality wins. Memory and start-time budgets still fail a PR. The alarm is built in step 6.
- Added to D5 while building: manifest expiry + sequence per signing key (max jump 100,000); several trusted keys; Ed25519 from Monocypher 4.0.2.
- Website floserver.org (floserver.app later, redirected); downloads from cdn.floserver.org with official URLs as fallback.
- Runtimes shared per computer, one copy per version, under %LOCALAPPDATA%\FloServer\runtimes (D8/D10).
- 1.0 runtime set (Windows x64): PHP NTS 8.5.12 / 8.4.27 / 8.3.36 / 8.2.35; MariaDB 11.4.13 / 11.8.9 (with GPL source archives); Node.js 24.22.1 / 22.23.4; Composer 2.10.3, nginx 1.30.6, Apache Lounge 2.4.69 VS18.
- Pre-ticked on first run: PHP 8.5, MariaDB 11.4, Node 24, Composer.
- Templates go in a `templates` section of the same signed manifest (D9).

## Step 3 design (PR #8), implementation details within D5/D8
- **Transport** (`http.h`, `http_windows.cpp`): WinHTTP, automatic proxy, TLS 1.2/1.3, redirect policy no https→http, max 5 redirects, final URL re-checked. Every request has `HttpLimits` (overall `max_time` + cancel flag); a `Watchdog` thread closes the request handle on deadline or cancel. No transport on macOS/Linux yet (`make_system_transport()` returns null; the service says so).
- **Runtime list** (`downloader.h`): signature then list, caps 4 KiB / 1 MiB, 60 s each; a BadSignature is retried once after 3 s (CloudFront mismatch window); Expired compares our clock with the server's Date header: off by > 1 h → "fix your date and time", else "the problem is on our side". Last sequence per key in `state/manifest-state.json` (atomic write, never lowered).
- **Files**: URLs in order (CDN, then upstream); stream to `<file>.part` with SHA-256; stop as soon as bytes exceed the listed size; time limit 2 min + size at 16 KiB/s; only exact size + hash is renamed into place.
- **Zip** (`zip_extract.h`, miniz 3.0.2 vendored, stdio off): all names checked before writing (no `..`, absolute/drive, `:`, device names, trailing dot/space, non-ASCII, `<>"|?*`); symlinks/encrypted refused; entry and size caps; exact size + CRC; single top folder dropped; empty archive refused.
- **Install layout** (`runtime_store.h`): `runtimes/<name>/<version>` (variant suffix only for non-NTS); staged in `.staging-…`, VC++ DLLs from `vcruntime\` next to floserverd copied into the top folder, `bin`, `Apache24\bin` when they hold an .exe and lack the DLL, marker `.floserver-runtime.json` (schema 1) written last, then renamed. Leftover `.staging-*`, `.removing-*`, `*.part`, `*.download` cleared at service start. Platform `any` runtimes (Composer) install on every platform.
- **Service** (`runtime_service.h`): installs are jobs on one worker thread; list fetched outside the lock; at most 16 waiting jobs; `stop()` cancels everything; floserverd cancels work on quit and `_Exit`s if a client is still busy after 2 s.
- **Protocol**: `runtimes.list {refresh?}`, `runtimes.install {name, version?}` → `{job}`, `runtimes.remove {name, version}`, `jobs.get {id}`, `jobs.cancel {id}`. Replies use JSON dump with UTF-8 replace, never throw. System errors go through `error_text()` (FormatMessageW → UTF-8).
- **CLI**: `flo runtimes [--refresh]`, `flo install <name> [version]` (progress line, Ctrl+C cancels), `flo uninstall <name> <version>`. Starts the service if needed.
- **CI**: Windows job installs nginx and Composer from the live CDN into a temp data folder, runs `nginx -v`, uninstalls, stops the service; every failure becomes an annotation (Write-Host, not piped). The Release build (exes + `vcruntime\`) is uploaded as an artifact per run.
- Env overrides for tests/dev: `FLOSERVER_INSTANCE`, `FLOSERVER_DATA_DIR`.
- Security review notes kept for later: revocation checking (WinHTTP fails hard when the revocation server is offline, so not enabled; hash + signature protect anyway); two services sharing one `FLOSERVER_DATA_DIR` would clear each other's staging (dev-only; add a lock file if it matters); for step 4 start runtimes with absolute paths and consider `SetDefaultDllDirectories` in floserverd.

## Step 2 design (PR #6), implementation details within D2
- **Windows pipe** `\\.\pipe\floserver-<SID>-<instance>`: SDDL `O:<sid>D:P(D;;GA;;;NU)(A;;GA;;;<sid>)S:(ML;;NWNR;;;ME)`; `FILE_FLAG_FIRST_PIPE_INSTANCE`; `PIPE_REJECT_REMOTE_CLIENTS`. The client checks the pipe's security descriptor (owner = user SID, label at least Medium) and connects with `SECURITY_IDENTIFICATION`; overlapped handles, 15 s request timeout (CLI uses 60–90 s for runtimes). The service doesn't check the client process.
- **POSIX:** socket in `$XDG_RUNTIME_DIR/floserver`, on macOS `$TMPDIR/floserver-<uid>`, else `~/.floserver/run`; folder 0700 and checked; flock for single instance; peer uid checked both ways.
- **Messages:** 4-byte little-endian length + JSON, at most 1 MiB.
- **floserverd:** at most 64 clients, a thread per client, exceptions contained. Measured 4.0 MB in a Linux release build.
- Windows quirk: a client that hangs up before the server's ConnectNamedPipe is never seen (ERROR_NO_DATA). Tests keep clients connected until accepted.

## Publishing routine (docs/manifest.md)
1. Upload files to `<runtime>/<version>/<original name>`.
2. Add them to `$files` in `tools/cdn-check/check-floserver-cdn.ps1`.
3. Run it with `-Bucket floserver-cdn-origin -Region eu-central-1`.
4. `floserver-manifest build floserver-cdn-report.json --sequence N --days 30 --accept <apache path>`.
5. Sign offline with `floserver-manifest sign manifest.json floserver-2026.key`.
6. Check with `floserver-manifest verify manifest.json`, which uses the built-in key.
7. Upload `manifest.json` and `.sig` together, then invalidate both in CloudFront.

## Code so far
- CMake 3.25, C and C++20. Code rules in `cmake/CodeRules.cmake`; `cmake/mingw-w64.cmake` for cross-builds.
- `src/core`: `project_config`, `version`, `ports`, `sha256`, `encoding`, `manifest`, `builtin`, `ipc`, `protocol`, `process`, `service_client`, `paths`, `http` (+ `http_windows`), `downloader`, `zip_extract`, `runtime_store`, `runtime_service`, and in PR #9 `project_registry`, `site_plan`, `nginx_config`, `project_service`.
- `src/daemon` floserverd (MSVC build copies VC++ DLLs into `bin/<config>/vcruntime/` via InstallRequiredSystemLibraries); `src/cli` floserver, copied as flo; `src/ui-win32` still a stub window.
- `third_party`: nlohmann/json, Monocypher 4.0.2, miniz 3.0.2, doctest.
- Tests: 129 cases with PR #12 (128 with PR #10). Linux CI installs `nginx-core php-cgi` for `test_web_service.cpp`. (`tests/test_util.h` has TempDir, SigningKey and FakeTransport). `test_service.cpp` runs the real floserverd with its own `FLOSERVER_DATA_DIR`.
- CI logs can't be read from the sandbox. Read annotations with `gh api repos/hostkurd/floserver/check-runs/<id>/annotations`; failing step names via `gh api repos/hostkurd/floserver/actions/jobs/<id> --jq '.steps[]'`.
- MSVC lessons: C4459 with `using namespace std::chrono`; `std::array` iterator isn't a pointer; `getenv` is C4996, use `get_env`; **in tests, build non-ASCII paths with `path_from_utf8` (a narrow string is read as ANSI on Windows) and use full paths that are absolute on Windows too (not `/x`)**.
- GCC lessons: release builds warn on ignored `write()` results and on `istreambuf_iterator` (null-dereference); `-Werror=infinite-recursion` caught a bad global replace.
- clang-tidy lessons: widen multiplications (`std::uint64_t{64} * 1024`); compare `mz_bool` with `MZ_FALSE`; use-after-move on optionals; unchecked optional members across functions (keep a plain value instead); single-char `find('$')`.
- Sandbox notes: `GH_TOKEN` is invalid, so `unset GH_TOKEN` before `gh` (add_repo with push access gives credentials); GraphQL is blocked, so open PRs with `gh api repos/hostkurd/floserver/pulls --input <json>`; clang's sanitizer runtime missing, use g++ for ASan; no IPv6; no wine; **MinGW now installs with `apt-get install mingw-w64`** (cross-build: `cmake -S . -B build-mingw -G Ninja -DCMAKE_BUILD_TYPE=MinSizeRel -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake`), which catches most Windows compile errors before CI; OpenSSL dev headers with `apt-get install libssl-dev`; cdn.floserver.org, nodejs.org, windows.php.net and Actions artifact storage aren't reachable; GitHub release downloads work. **`apt-get update && apt-get install nginx-core php-cgi` works** (nginx 1.24, php-cgi 8.3) for end-to-end checks of the generated config: nginx runs as root here, so add `user root;` to the test copy, strip `http2 on;` (1.24 lacks it), and leave out `[::1]`.
- **Stacked PRs:** when a PR is based on another PR's branch, retarget it to `main` (`gh api -X PATCH repos/hostkurd/floserver/pulls/<n> -f base=main`) as soon as the lower PR merges, *before* telling the user it's ready; otherwise merging it lands on the old branch (happened with #10). Prefer not stacking.
- After `git push`, the stop hook may say "no remote branch": check `git ls-remote`, then set tracking and `git update-ref`.

## Next steps
1. Points for the user:
   - merge PR #9 when CI is green (it's theirs to merge);
   - check Apache's SHA-256 (77d715d0…a652) by hand if not done;
   - decide who holds the key backup;
   - the service's memory on Windows after an install (`flo status`) wasn't reported yet; ask when next testing.
2. **PR #12** (step 4c) to ready after CI + review; then **step 4d (PR #13): MariaDB** per version (see Step 4 plan). Still unconfirmed: the user's `flo trust` test on Windows.
3. **Step 5: the Win32 window** to the mockups, EN/AR/KU, tray.
4. **Step 6: release.** MSI, installer-size alarm at 15 MB and the other D15 budgets in CI, code signing (Q3), beta. floserverd becomes a no-console (WIN32 subsystem) program before autostart. Build the release with a Visual Studio at least as new as Apache's VS18 build needs (VC++ DLLs come from it).
5. Gaps:
   - clang-tidy doesn't see the `_WIN32` code paths yet;
   - when WSAStartup fails, every port reads as taken and nothing is shown;
   - paths over 260 characters (long-path support) not handled in installs;
   - tar.gz/tar.xz archives and download resume not implemented (not needed for Windows 1.0);
   - service memory not yet measured on Windows with an install done;
   - no CI test runs `nginx -t` on a generated config yet (add in #11 with the CDN nginx on Windows);
   - concurrent `projects.add` calls not tested.

## Testing on the user's Windows PC (works since 29 Oct)
- The chat is linked to the PC "home-pc" (Windows 11, user `micro`). Granted folder: `C:\Users\micro\source`; Claude works only in `source\floserver-test` (bin\, data\, sites\, run*.ps1/.cmd, results*.txt). The device shell is a Linux sandbox with the folder mounted at `~/mnt/source` (it can't run .exe); deleting there is not permitted.
- Computer use is ON; File Explorer granted at tier "click" (no typing). Start a test by double-clicking `runN.cmd` in Explorer (open_application "File Explorer", full-res screenshot for coordinates), then `computer_release_lock`, then poll `results*.txt` via device_bash.
- Builds: the MinGW cross-build (`build-mingw/bin/*.exe`, static, only system DLLs) goes over with SendUserFile + device_commit_files to `C:\Users\micro\source\floserver-test\bin\`. The PC has the VC++ runtime in System32. The scripts set FLOSERVER_DATA_DIR to `floserver-test\data`, instance `pc-test`, ports 18080/18443.
- `run.ps1` = the CI flow; `run2.ps1` = cases A–E (sleep/wake, reloads with same/new pool, running/asleep); `run4.ps1` = 20 in a row + run2 + run; `run5.ps1` = MariaDB (results5.txt); `ui-setup.cmd` adds projects in every state; `ui-<en|ar|ku>-<light|dark>.cmd` open the window (`taskkill` first); `ui-close.cmd` closes the window and stops the service. Grant `bin\FloServer.exe` (resolve by full path) to screenshot it. Don't run two scripts at once (trust.cmd once ran during run5 and reset the projects).

## Speeding up (asked 29 Oct)
Done: computer use on (1), Claude merges when green (3), window in parallel (4). Not done: faster CI (5).
Suggested: (1) user opens the Claude desktop app on the Windows PC so this chat's linked computer is reachable and Claude runs Windows tests itself (was "not connected" on 29 Oct); (2) hand tests per milestone instead of per PR; (3) Claude merges when CI is green and the review has nothing open (only if the user agrees); (4) step 5 (Win32 window) in parallel with 4d on separate branches from `main`, no stacking; (5) faster CI: split the live Windows step into its own job and cache builds (do after #12 merges, it touches ci.yml).

## Working rules carried over
- Process: design note → mockups → approval → build. Tell the user straight away about any change to an approved decision.
- Arabic/Kurdish strings written by Claude always need a native read; list the keys to check. Still open from the mockups: يعمل / نائم / يستيقظ / متوقف, تنزيل المطلوب, إبقاؤه يعمل, نسخ أسطر .env, Western digits.
- Same code rules as Studio. Passwords and keys never in arguments, logs or shown text.
- Windows-specific behaviour needs the user's PC: send a build and a short list of things to try, or ask them to link the chat to their PC in the desktop app.
- Independent review agent before each PR is final; security-focused for anything on the channel, the downloader, or anything that writes server config.
- Commits end with the Co-Authored-By / Claude-Session lines; PR descriptions end with the Claude Code line.
