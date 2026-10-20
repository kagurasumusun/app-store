#!/usr/bin/env python3
"""Verifies that every file reference in IpaStore.xcodeproj resolves.

`pbxproj` happily parses a project whose group paths point somewhere else, and
xcodebuild only notices at build time ("Build input file cannot be found").
This check walks the group tree and resolves each reference the way Xcode
does.

Usage:
    python3 Scripts/check_project_refs.py
"""

from __future__ import annotations

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
PROJECT = ROOT / "IpaStore.xcodeproj" / "project.pbxproj"

# References that intentionally have no file on disk.
ALLOWED_MISSING = {
    "IpaStore.app",  # build product
    "Security.framework",  # SDK framework
}

ENTRY = re.compile(r"^\t\t([0-9A-F]{24}) /\* (.*?) \*/ = \{\s*$")
FILE_REF = re.compile(
    r"^\t\t([0-9A-F]{24}) /\* (.*?) \*/ = \{isa = PBXFileReference;(.*)\};\s*$"
)
CHILD = re.compile(r"([0-9A-F]{24})")


def parse(text: str) -> tuple[dict[str, dict], dict[str, str], str]:
    lines = text.splitlines()
    groups: dict[str, dict] = {}
    refs: dict[str, str] = {}
    root = ""

    index = 0
    while index < len(lines):
        line = lines[index]
        reference = FILE_REF.match(line)
        if reference:
            body = reference.group(3)
            path = re.search(r"path = (?:\"([^\"]*)\"|([^;]+));", body)
            if path:
                refs[reference.group(1)] = (path.group(1) or path.group(2)).strip()
            index += 1
            continue

        entry = ENTRY.match(line)
        if entry:
            block: list[str] = []
            cursor = index + 1
            while cursor < len(lines) and lines[cursor].strip() != "};":
                block.append(lines[cursor])
                cursor += 1
            body = "\n".join(block)
            if "isa = PBXGroup;" in body:
                children: list[str] = []
                in_children = False
                for inner in block:
                    stripped = inner.strip()
                    if stripped.startswith("children = ("):
                        in_children = True
                        continue
                    if in_children:
                        if stripped == ");":
                            in_children = False
                            continue
                        children.extend(CHILD.findall(stripped))
                name = re.search(r"^\t\t\tname = (.*);$", body, re.M)
                path = re.search(r"^\t\t\tpath = (.*);$", body, re.M)
                groups[entry.group(1)] = {
                    "name": (name.group(1) if name else entry.group(2)).strip('"'),
                    "path": path.group(1).strip('"') if path else "",
                    "children": children,
                }
            index = cursor + 1
            continue

        index += 1

    # The project references its main group; find it in the PBXProject block.
    for match in re.finditer(r"mainGroup = ([0-9A-F]{24})", text):
        root = match.group(1)
        break

    return groups, refs, root


def main() -> int:
    if not PROJECT.exists():
        print(f"project file missing: {PROJECT}")
        return 1

    groups, refs, root = parse(PROJECT.read_text())
    if not root or root not in groups:
        print("could not locate the main group in the project")
        return 1

    unresolved: list[str] = []
    checked = 0

    def walk(identifier: str, base: pathlib.Path) -> None:
        nonlocal checked
        group = groups.get(identifier)
        if group is None:
            name = refs.get(identifier)
            if name is None:
                return
            checked += 1
            candidate = base / name
            if not candidate.exists() and pathlib.Path(name).name not in ALLOWED_MISSING:
                unresolved.append(str(candidate.relative_to(ROOT)))
            return
        here = base / group["path"] if group["path"] else base
        for child in group["children"]:
            walk(child, here)

    walk(root, ROOT)

    for missing in unresolved:
        print(f"missing project reference: {missing}")
    print(f"project references: {checked} checked, {len(unresolved)} unresolved")
    return 1 if unresolved else 0


if __name__ == "__main__":
    sys.exit(main())
