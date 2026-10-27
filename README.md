# FloServer

A tiny local development environment for PHP, Node.js and databases, by
HostKurd. Native C++, Windows first. Free, closed source.

The design note with every decision (D1–D16) lives in the FloCMS Server
project on claude.ai. This repository follows it; a change to a decision goes
back to the note first.

## Layout

| Folder | What it is |
| --- | --- |
| `src/core` | The shared core library: every rule and decision, no UI code (D1) |
| `src/daemon` | `floserverd`, the background service in the user's session (D2) |
| `src/cli` | `floserver` command line; the build also makes a copy named `fls` (D2, Q2). Install rules come with the installer. |
| `src/ui-win32` | `FloServer.exe`, the Windows window, plain Win32 (D1) |
| `tests` | Unit tests for the core (doctest) |
| `third_party` | Header-only libraries, see `third_party/README.md` |

## Building

Needs CMake 3.25+ and a C++20 compiler.

Windows (Visual Studio 2022):

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release
```

Linux or macOS, for the core and the tests:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DFLOSERVER_SANITIZE=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Options: `FLOSERVER_SANITIZE` (ASan + UBSan), `FLOSERVER_CLANG_TIDY`
(clang-tidy as errors), `FLOSERVER_BUILD_TESTS` (on by default).
`cmake/mingw-w64.cmake` cross-compiles the Windows programs from Linux.

## Code rules

The same as FloCMS Studio:

- Warnings are errors on every compiler; clang-tidy findings are errors.
- Tests run under AddressSanitizer and UndefinedBehaviorSanitizer in CI.
- No raw `new`/`delete` outside owning types.
- Passwords never go in command-line arguments, logs or text shown on screen.
- Everything listens on 127.0.0.1 only. A port is "free" only if we can listen
  on it; never connect to another program's port to test it.
- No admin rights, no Windows services, no hosts-file edits.
