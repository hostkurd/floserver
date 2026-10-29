#!/usr/bin/env python3
"""Runs clang-tidy only when its verdict could have changed.

CMake calls this in place of clang-tidy (FLOSERVER_CLANG_TIDY_CACHE) as
  clang_tidy_cache.py <clang-tidy> <options...> <source> -- <compile command>
The key is the raw text of the source and of every file it includes (found
through the preprocessor, so a header counts, and so do a macro nobody uses
and a NOLINT on an #include line), the compile and clang-tidy commands, the
clang-tidy version, and every .clang-tidy file. A file that
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
    return out + ["-E"]


def included_files(preprocessed):
    """Every file named in the preprocessor's line markers."""
    files = set()
    for line in preprocessed.splitlines():
        # # 12 "/path/to/file.h" 2
        if line.startswith(b"# ") and b'"' in line:
            name = line.split(b'"', 2)[1]
            if not name.startswith(b"<"):
                files.add(name)
    return sorted(files)


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

    pre_cmd = preprocess_command(compile_cmd)
    pre = subprocess.run(pre_cmd, capture_output=True)
    if pre.returncode != 0:
        return subprocess.call(args)
    version = subprocess.run([tidy_cmd[0], "--version"], capture_output=True).stdout

    parts = [version, "\0".join(tidy_cmd).encode(), "\0".join(pre_cmd).encode(), *tidy_configs(source)]
    for name in included_files(pre.stdout):
        try:
            parts += [name, Path(os.fsdecode(name)).read_bytes()]
        except OSError:
            return subprocess.call(args)
    key = hashlib.sha256()
    for part in parts:
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
