#!/usr/bin/env python3
"""Runs clang-tidy only when its verdict could have changed.

CMake calls this in place of clang-tidy (FLOSERVER_CLANG_TIDY_CACHE) as
  clang_tidy_cache.py <clang-tidy> <options...> <source> -- <compile command>
The key is the preprocessed source (so every header it includes counts),
the clang-tidy command and version, and every .clang-tidy file. A file that
passed with the same key before passes again without running clang-tidy.
Only passes are remembered, so a failure is always reported in full.
"""

import hashlib
import os
import subprocess
import sys
from pathlib import Path


def preprocess_command(compile_cmd):
    """The compile command changed to print the preprocessed source."""
    out = []
    skip_next = False
    for arg in compile_cmd:
        if skip_next:
            skip_next = False
            continue
        if arg in ("-o", "-MF", "-MT", "-MQ"):
            skip_next = True
            continue
        if arg in ("-c", "-MD", "-MMD") or arg.startswith(("-o", "-MF")):
            continue
        out.append(arg)
    # -C keeps comments, so adding or removing a NOLINT changes the key.
    return out + ["-E", "-C"]


def tidy_configs(source):
    """Every .clang-tidy from the source's folder up to the root."""
    found = []
    folder = Path(source).resolve().parent
    while True:
        config = folder / ".clang-tidy"
        if config.is_file():
            found.append(config.read_bytes())
        if folder.parent == folder:
            return found
        folder = folder.parent


def main():
    args = sys.argv[1:]
    cache_dir = os.environ.get("FLOSERVER_CLANG_TIDY_CACHE_DIR")
    if "--" not in args or not cache_dir:
        return subprocess.call(args)
    split = args.index("--")
    tidy_cmd, compile_cmd = args[:split], args[split + 1:]
    source = tidy_cmd[-1]

    pre = subprocess.run(preprocess_command(compile_cmd), capture_output=True)
    if pre.returncode != 0:
        return subprocess.call(args)
    version = subprocess.run([tidy_cmd[0], "--version"], capture_output=True).stdout

    key = hashlib.sha256()
    for part in [version, "\0".join(tidy_cmd).encode(), *tidy_configs(source), pre.stdout]:
        key.update(hashlib.sha256(part).digest())
    marker = Path(cache_dir) / key.hexdigest()
    if marker.exists():
        marker.touch()  # still in use: CI drops markers left untouched for weeks
        return 0

    result = subprocess.call(args)
    if result == 0:
        marker.parent.mkdir(parents=True, exist_ok=True)
        marker.touch()
    return result


if __name__ == "__main__":
    sys.exit(main())
