# FloServer — notes for Claude

Read these first, in this order:

1. `docs/handoff.md` — where the work stands, what's next, open decisions,
   lessons from earlier sessions. Update it after each milestone (merged PR,
   decision taken) and before a session ends.
2. `docs/design.md` — the approved design (D1–D16) and proposals (D17).
3. `README.md` — layout, build commands, code rules.

The master copies of the handoff and the design note live in the claude.ai
project "Flo Server" (`floserver-handoff.md`, and the "FloServer — Design
note" doc). A session attached to that project should keep both in step with
the repo copies; a session without it uses the repo copies and says so.

## Working rules (the user's)

- Process for anything new: design note → mockups → the user's approval →
  build. Tell the user straight away about any change to an approved
  decision; don't build a proposed decision before it's approved.
- Every PR gets an independent review (a separate agent that hasn't seen the
  work) before it's final; security-focused for the service channel, the
  downloader, and anything that writes server config.
- **Merge rule (29 Oct):** Claude merges a PR itself when all CI checks pass
  and the review has nothing open, and tells the user each time. Branch from
  `main`; no stacked PRs.
- Arabic and Kurdish (Sorani) strings Claude writes always need a native read:
  list them for the user. UI work: send screenshots (EN, AR, dark).
- Windows behaviour is checked on a real Windows PC before it's called done.
- Passwords and keys never in arguments, logs or shown text.
- Commits end with the Co-Authored-By / Claude-Session lines the session
  gives; PR descriptions end with the Claude Code line.
- Write to the user in plain, short English; no jargon they didn't use.

## Build and test

- Linux (core, CLI, daemon, tests; the CI flow):
  `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DFLOSERVER_SANITIZE=ON && cmake --build build && ctest --test-dir build --output-on-failure`.
  Real-runtime tests need `nginx-core php-cgi mariadb-server` (CI installs
  them; `test_database_service` fails in CI if MariaDB is missing).
- Windows with Visual Studio 2022: see `README.md`. MSVC is stricter than
  MinGW: no `getenv` (use `core::get_env`), and never name anything `small`
  (the Windows headers `#define small char`).
- Cross-build the Windows programs from Linux:
  `cmake -S . -B build-mingw -G Ninja -DCMAKE_BUILD_TYPE=MinSizeRel -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake`
  (`apt-get install mingw-w64`). Static, only system DLLs.
- clang-tidy: configure with `-DFLOSERVER_CLANG_TIDY=ON` (clang-18).
- Env for tests and dev copies: `FLOSERVER_DATA_DIR`, `FLOSERVER_INSTANCE`,
  `FLOSERVER_WEB_PORTS=18080,18443`, `FLOSERVER_IDLE_SECONDS`,
  `FLOSERVER_PHP_WORKERS`, `FLOSERVER_LANG=en|ar|ku`,
  `FLOSERVER_THEME=light|dark`.

## CI and GitHub

- CI runs on pull requests and on `main`: MSVC, MinGW, Linux GCC release,
  Linux clang ASan+UBSan, clang-tidy; plus a daily check of the live runtime
  list on cdn.floserver.org.
- Failing test lines show as annotations on the run:
  `gh api repos/hostkurd/floserver/check-runs/<id>/annotations`.
- In Claude's cloud sandbox: `unset GH_TOKEN` before `gh`; GraphQL is blocked,
  so open PRs with `gh api repos/hostkurd/floserver/pulls --input <json>` and
  merge with `gh api -X PUT repos/hostkurd/floserver/pulls/<n>/merge -f merge_method=merge`;
  CI job logs (blob storage) and cdn.floserver.org aren't reachable from there.
