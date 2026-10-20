#!/usr/bin/env python3
"""Turn compiler diagnostics in a build log into GitHub Actions annotations.

GitHub renders `::error file=...,line=...::message` lines in the workflow log
as annotations on the commit, which makes failures visible from the Checks API
without downloading the (large) raw log.

Usage:
    python3 Scripts/ci_annotate.py build/logs/simulator.log
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

MAX_ERRORS = 60
MAX_WARNINGS = 20
TAIL_LINES = 25
TAIL_CHARS = 6000

# clang / swiftc: /path/to/file.swift:12:34: error: message
LOCATED = re.compile(
    r"^(?P<file>[^\s:][^:]*\.(?:swift|mm|m|h|hpp|cpp|c|cc|plist|xcprivacy))"
    r":(?P<line>\d+)(?::(?P<column>\d+))?:"
    r"\s*(?P<severity>error|warning|note|fatal error):\s*(?P<message>.*)$"
)

# xcodebuild: error: "...". A line without a location still matters.
BARE = re.compile(r"^(?:[^:]*:\s*)?(?P<severity>error|fatal error):\s*(?P<message>.*)$")


def escape(value: str) -> str:
    return (
        value.replace("%", "%25")
        .replace("\r", "%0D")
        .replace("\n", "%0A")
    )


def annotate(path: Path) -> int:
    errors = 0
    warnings = 0
    try:
        lines = path.read_text(errors="replace").splitlines()
    except OSError as error:
        print(f"::warning::Could not read {path}: {error}")
        return 0

    for line in lines:
        stripped = line.strip()
        match = LOCATED.match(stripped)
        if match:
            severity = match.group("severity")
            message = escape(f"{path.name}: {match.group('message')}")
            location = (
                f"file={match.group('file')},line={match.group('line')}"
            )
            if match.group("column"):
                location += f",col={match.group('column')}"
            if severity == "error":
                if errors < MAX_ERRORS:
                    print(f"::error {location},title=Build error::{message}")
                errors += 1
            elif severity == "warning":
                if warnings < MAX_WARNINGS:
                    print(f"::warning {location},title=Build warning::{message}")
                warnings += 1
            continue

        bare = BARE.match(stripped)
        if bare and bare.group("severity") == "error":
            if errors < MAX_ERRORS:
                message = escape(bare.group("message"))
                print(f"::error title=Build error::{message}")
            errors += 1

    return errors


def annotate_tail(path: Path) -> None:
    """Emits the end of the log as one annotation.

    Compiler diagnostics are parsed above; this catches everything else
    (shell errors, linker failures, `xcodebuild` summaries) that would
    otherwise only be visible in the raw Actions log.
    """
    try:
        lines = [line for line in path.read_text(errors="replace").splitlines() if line.strip()]
    except OSError:
        return
    if not lines:
        return
    tail = "\n".join(lines[-TAIL_LINES:])[-TAIL_CHARS:]
    print(f"::error title={path.name} (tail)::{escape(tail)}")


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print("usage: ci_annotate.py <log> [<log> ...]", file=sys.stderr)
        return 2

    total = 0
    for argument in argv[1:]:
        path = Path(argument)
        if not path.is_file():
            continue
        found = annotate(path)
        total += found
        print(f"{path}: {found} error line(s)")
        if found == 0:
            annotate_tail(path)

    # Failures are reported through annotations; the job already failed.
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
