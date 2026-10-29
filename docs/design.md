# FloServer design (copy of the design note)

The master copy is the "FloServer — Design note" doc in the claude.ai
project Flo Server (https://claude.ai/artifact/7vJVpQSFLnty5yz8nUM4Hg),
approved 27 Oct 2026. This copy lets any session read the decisions from
the repo. If they disagree, the doc wins; update this file when it changes.
Mockups (approved 27 Oct): https://claude.ai/artifact/5hKStiT4gyDZKjXnj8twnP

FloServer is a free, closed-source, tiny local development environment for
PHP, Node.js and databases, built in native C++, Windows first. The
installer ships no runtimes: PHP, MariaDB, Node.js, Composer and the web
server are downloaded on first run, and each project chooses its own
versions.

- **Who it's for:** web developers who use XAMPP, Laragon, WampServer or MAMP
  today, starting with HostKurd customers and FloCMS users in the region.
- **Promise:** smallest and lightest in its class, no admin rights,
  per-project versions, Kurdish and Arabic built in, free with no nag popups.
- **Not:** a production server, a container tool, or a code editor.
- **Name and site:** FloServer, floserver.org; downloads from
  cdn.floserver.org.

## Budgets (checked in CI, D15)

| Measure | Budget |
| --- | --- |
| Installer (MSI, no runtimes) | lightweight; CI alarm above 15 MB (not a failure) |
| UI memory, window open | under 20 MB |
| Service memory, nothing running | under 10 MB |
| First window shown | under 300 ms |
| Memory with every project idle | close to the service alone (D11) |

## Decisions (all approved unless marked)

- **D1. Language and UI:** C++20 + CMake, as in FloCMS Studio. All logic in a
  core library with no UI code; each platform's UI only draws and calls the
  core. Windows UI: plain Win32, common controls v6, per-monitor DPI, our own
  dark theme. No Qt, no webview.
- **D2. Three programs:** `floserverd` (per-user background service, not a
  Windows service: runtimes, processes, web config, certificates, idle
  timers; keeps running when the window closes), `FloServer.exe` (the window
  + tray icon, a client of the service), `floserver` / `flo` (CLI). They talk
  over a user-only local channel (named pipe with a user-only ACL; Unix socket
  in a 0700 folder) with small JSON messages. One service per user.
- **D3. Platforms:** Windows 10 21H2+ and 11 x64 first (ARM64 when runtimes
  have builds); macOS 13+ second; Linux x64 third.
- **D4. Licence:** closed source, free, no account, no nag, no telemetry by
  default. Needs an OV code-signing certificate (Q3: seller and key holder
  chosen before the beta). Runtimes keep their own licences, shown on first
  install; never modified or rebranded. EULA in the installer.
- **D5. Runtimes from a signed manifest:** `manifest.json` + `.sig`
  (Ed25519, Monocypher; keys built in, several trusted at once), with
  `expires` and a `sequence` that only goes up. Every file checked by SHA-256.
  Mirrored on S3 + CloudFront at cdn.floserver.org with upstream URLs as
  fallback. Per-user installs, never Program Files. Windows PHP from
  windows.php.net; MariaDB, Node.js, Composer official builds; nginx and Apache
  Lounge Windows builds. macOS/Linux PHP: static-php-cli built in our CI
  (later). Release steps: `docs/manifest.md`.
- **D6. Web server:** one shared nginx by default; Apache downloaded only for
  projects that ask (`"web": "apache"`), proxied behind nginx. Config written
  by the service, never by hand. PHP as FastCGI: php-cgi on Windows, php-fpm
  on macOS/Linux. Caddy rejected (size).
- **D7. Addresses and HTTPS without admin:** `https://<name>.localhost`. Ports
  80/443 when free, else 8080/8443… shown in the address (Q4). A local CA
  (key in the user's app folder, name-constrained to localhost) added to the
  *user's* certificate store; Windows asks once. Removing FloServer removes
  the CA.
- **D8. Folders:** projects in `%USERPROFILE%\FloServer\projects` (changeable;
  any folder can be added); app data in `%LOCALAPPDATA%\FloServer`
  (`runtimes\<name>\<version>`, `data\mariadb-<series>`, `certs`, `logs`,
  `config.json`). Uninstall never deletes projects or databases unless ticked.
- **D9. Templates:** data in the manifest (JSON + starter files), running real
  commands and showing them. 1.0 set (Q8): PHP app, FloCMS (hands off to
  Studio), WordPress, Node app — plus "FastAPI app" if D17 is approved.
  Laravel, Static site and Database only follow through the manifest.
- **D10. `floserver.json` per project:** `php`, `node`, `database`
  (`"mariadb-11.4"`), `web`, `root`, `extensions`; goes into git. Missing
  versions are offered for download. Each PHP version + extension set is one
  FastCGI pool. Terminal shims (`php`, `node`, `npm`, `composer`) on PATH find
  the nearest `floserver.json`. `.env` database lines only when the user asks.
- **D11. Sleep when idle:** a project is On or Off; On doesn't mean running.
  The service holds each pool's listening socket; on the first connection it
  starts php-cgi with that socket (Q1: one php-cgi per port on Windows, 4 per
  pool). Idle from the access log, default 10 min. Node dev servers and
  databases follow the same rule. "Keep running" turns sleep off.
- **D12. Databases:** one MariaDB per version in use, each project its own
  database and user. Ports from 3306 up, never another stack's. 127.0.0.1
  only. Passwords in Windows Credential Manager, never in logs or shown text.
  MySQL 8 later if customers ask (Q7). PostgreSQL and Redis later.
- **D13. Languages:** English, Arabic, Kurdish (Sorani), full RTL; code words
  stay left-to-right. Arabic/Kurdish strings need a native read before each
  release.
- **D14. FloCMS Studio:** the service writes a small file listing its runtimes
  and paths; Studio offers to use them. No shared code or DLLs at runtime.
- **D15. Budgets in CI** (table above). Same code rules as Studio: no raw
  new/delete outside owners, warnings as errors, clang-tidy, ASan/UBSan.
- **D16. Release plan:** 1.0 Windows (D1–D15); 1.x one-file project export and
  import; 2.0 one-click deploy to HostKurd hosting, and macOS; later Linux,
  PostgreSQL, Redis.
- **D17. Python projects with venv — PROPOSED 29 Oct, waiting for the user's
  approval. Don't build before it's approved.** Python 3.13 and 3.12 from
  python-build-standalone "install_only" builds (full CPython with venv and
  pip) on the CDN. `"python": "3.13"` in floserver.json (`flo use python`),
  optional `"command"`. FloServer makes `.venv` in the project and installs
  `requirements.txt` / `pyproject.toml` with pip (uv maybe later). nginx
  forwards to the app's own server on a chosen port, like Node (D11): FastAPI
  first-class (`uvicorn <module>:app --reload`, app found in `main.py` or
  `app/main.py`), Django (`manage.py runserver`), Flask (`flask run`).
  `flo env` adds `DATABASE_URL`. Shims: `python`/`pip` use the project's
  venv. Window: Python chip; Runtimes screen; not pre-ticked on first run.
  Template "FastAPI app". Built in step 4e with Node.

## Screens (approved mockups)

1. First run: language → runtimes to download (PHP, MariaDB, Node, Composer
   pre-ticked, sizes shown) → done; the certificate prompt here.
2. Main screen: project list (name, address, version chips, state On /
   Sleeping / Waking / Off / Needs download, one switch); top bar New project,
   search, Settings; footer with what's running and memory.
3. Project panel (right side): open in browser / folder / terminal, version
   dropdowns, database name + "Copy .env lines", Keep running, live log.
4. New project: template cards → name and folder → versions → Create, showing
   the real commands.
5. Runtimes (Settings): installed with size and "used by N projects",
   available to download, remove unused.
6. Tray: status, open, quick on/off, quit.
