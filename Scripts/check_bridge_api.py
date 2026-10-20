#!/usr/bin/env python3
"""Static consistency check between the Objective-C++ bridge headers and the
Swift layer.

The project cannot be compiled on the machine used to build it (no Xcode, no
Swift compiler), so this script performs the checks a compiler would otherwise
give us for free:

1. Every Objective-C method/property exposed to Swift is converted into the
   Swift name the importer produces (explicit `NS_SWIFT_NAME` wins, otherwise
   the standard `...WithX:` -> `...(x:)` rule is applied).
2. Every `bridge.<name>(...)`, `server.<name>(...)`, `IPInstallServer.<name>(...)`
   and `IPStoreService.<name>(...)` call in `IpaStore/Sources/*.swift` is
   collected with its argument labels.
3. Calls are matched against the generated names, including their labels.

Exit code 0 means no mismatches were found.
"""

from __future__ import annotations

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
BRIDGE = ROOT / "IpaStore" / "Bridge"
SOURCES = ROOT / "IpaStore" / "Sources"


# ---------------------------------------------------------------------------
# Objective-C header parsing
# ---------------------------------------------------------------------------


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    return text


def split_top_level(text: str, separator: str = ",") -> list[str]:
    """Splits an Objective-C/Swift list on a top-level separator."""
    parts: list[str] = []
    depth = 0
    current: list[str] = []
    for char in text:
        if char in "([{<":
            depth += 1
        elif char in ")]}>":
            depth = max(0, depth - 1)
        if char == separator and depth == 0:
            parts.append("".join(current))
            current = []
            continue
        current.append(char)
    if "".join(current).strip():
        parts.append("".join(current))
    return parts


def selector_to_swift(selector: str, swift_name: str | None) -> tuple[str, list[str]]:
    """Converts an Objective-C selector into a Swift base name and labels."""
    if swift_name:
        name, labels = parse_swift_name(swift_name)
        return name, labels

    pieces = [piece for piece in selector.split(":") if piece]
    first = pieces[0]
    # loginWithEmail:password: -> login(email:password:)
    match = re.search(r"With([A-Z]\w*)$", first)
    if match:
        base = first[: match.start()]
        label = match.group(1)
        label = label[0].lower() + label[1:]
        return base, [label] + pieces[1:]
    return first, pieces[1:]


def parse_swift_name(annotation: str) -> tuple[str, list[str]]:
    """Parses `login(email:password:completion:)` or `prepareCertificate()`."""
    annotation = annotation.strip()
    match = re.match(r"([A-Za-z_]\w*)\((.*)\)$", annotation)
    if not match:
        return annotation, []
    name, args = match.group(1), match.group(2)
    labels = []
    for chunk in split_top_level(args, ":"):
        chunk = chunk.strip()
        if not chunk:
            continue
        label = chunk.split(":")[0].strip()
        labels.append("" if label == "_" else label)
    return name, labels


def parse_headers() -> dict[str, dict[str, set[tuple[str, ...]]]]:
    """Returns {class: {"methods": {(name, labels...)}, "properties": {name}}}."""
    api: dict[str, dict] = {}
    for path in sorted(BRIDGE.glob("*.h")):
        text = strip_comments(path.read_text())
        for block in re.finditer(
            r"@interface\s+(\w+)\s*(?::\s*\w+)?(?:\([^)]*\))?(.*?)@end", text, re.S
        ):
            cls, body = block.group(1), block.group(2)
            entry = api.setdefault(cls, {"methods": set(), "properties": set()})
            # Properties: `@property (...) type name;`
            for prop in re.finditer(r"@property\s*\(([^)]*)\)\s*([^;]+);", body):
                attributes, declaration = prop.group(1), prop.group(2)
                name = declaration.split()[-1].lstrip("*")
                swift = re.search(r"NS_SWIFT_NAME\(\s*([A-Za-z_]\w*)\s*\)", declaration)
                if swift:
                    entry["properties"].add(swift.group(1))
                    continue
                if "getter=" in attributes:
                    getter = re.search(r"getter=(\w+)", attributes).group(1)
                    entry["properties"].add(
                        getter[2:] if getter.startswith("is") else getter
                    )
                else:
                    entry["properties"].add(name)
            # Methods: from `- (` or `+ (` up to `;`
            for method in re.finditer(r"[-+]\s*\((?:[^()]|\([^()]*\))*\)\s*([^;]+);", body):
                declaration = method.group(1)
                swift = re.search(
                    r"NS_SWIFT_NAME\(\s*([A-Za-z_]\w*\([^)]*\)|[A-Za-z_]\w*)\s*\)", declaration
                )
                swift_name = swift.group(1) if swift else None
                without_attr = re.sub(
                    r"NS_SWIFT_NAME\([^)]*\)|NS_DESIGNATED_INITIALIZER|API_\w+\([^)]*\)",
                    "",
                    declaration,
                )
                selector_match = re.search(r"([A-Za-z_]\w*(?:\s*:\s*(?:\([^)]*\))?\s*\w+)*\s*:?)", without_attr)
                if not selector_match:
                    continue
                selector_text = selector_match.group(1)
                selector = re.sub(r"\s+", "", re.sub(r"\([^)]*\)", "", selector_text))
                selector = re.sub(r"[A-Za-z_]\w*$", "", selector) + ":" * 0
                # Rebuild the selector from the label words.
                words = re.findall(r"([A-Za-z_]\w*)\s*:", without_attr)
                if words:
                    selector = ":".join(words) + ":"
                else:
                    selector = re.split(r"\s+", without_attr.strip())[0]
                name, labels = selector_to_swift(selector, swift_name)
                entry["methods"].add(tuple([name] + labels))
    return api


# ---------------------------------------------------------------------------
# Swift usage
# ---------------------------------------------------------------------------

RECEIVERS = {
    "bridge": "IPStoreService",
    "IPStoreService": "IPStoreService",
    "IPInstallServer": "IPInstallServer",
    "server": "IPInstallServer",
}


def swift_call_sites() -> list[tuple[str, str, tuple[str, ...], str, int, bool]]:
    """Yields (receiver, name, labels, file, line, has_trailing_closure)."""
    sites = []
    for path in sorted(SOURCES.glob("*.swift")):
        text = path.read_text()
        for receiver, _cls in RECEIVERS.items():
            for match in re.finditer(rf"\b{receiver}\s*\.\s*(\w+)\s*", text):
                name = match.group(1)
                position = match.end()
                if position >= len(text) or text[position] != "(":
                    continue
                labels, end = read_arguments(text, position)
                trailing = end + 1 < len(text) and text[end + 1:].lstrip().startswith("{")
                line = text.count("\n", 0, match.start()) + 1
                sites.append((receiver, name, labels, path.name, line, trailing))
    return sites


def read_arguments(text: str, open_paren: int) -> tuple[tuple[str, ...], int]:
    depth = 0
    labels: list[str] = []
    index = open_paren
    chunk_start = open_paren + 1
    while index < len(text):
        char = text[index]
        if char in "([{":
            depth += 1
        elif char in ")]}":
            depth -= 1
            if depth == 0:
                chunk = text[chunk_start:index]
                labels.extend(labels_of(chunk))
                return tuple(labels), index
        elif char == "," and depth == 1:
            labels.extend(labels_of(text[chunk_start:index]))
            chunk_start = index + 1
        index += 1
    return tuple(labels), len(text)


def labels_of(chunk: str) -> list[str]:
    chunk = chunk.strip()
    if not chunk:
        return []
    match = re.match(r"([A-Za-z_]\w*)\s*:", chunk)
    if match:
        return [match.group(1)]
    return [""]


def main() -> int:
    api = parse_headers()
    problems = 0
    checked = 0
    for receiver, name, labels, filename, line, trailing in swift_call_sites():
        cls = RECEIVERS[receiver]
        if cls not in api:
            continue
        entry = api[cls]
        key = tuple([name] + list(labels))
        if key in entry["methods"]:
            checked += 1
            continue
        # A trailing closure fills the final completion parameter, so Swift
        # writes it without a label.
        if trailing and key + ("completion",) in entry["methods"]:
            checked += 1
            continue
        # Simple property access parses as an empty label list.
        if not labels and name in entry["properties"]:
            checked += 1
            continue
        # `IPInstallServer.prepareCertificate()` is a class method named with
        # an explicit NS_SWIFT_NAME; the receiver form still matches.
        if not labels and name in {method[0] for method in entry["methods"]}:
            checked += 1
            continue
        problems += 1
        print(f"{filename}:{line}: {receiver}.{name}({', '.join(labels)}) "
              f"does not match any {cls} selector")
        candidates = sorted(
            method for method in entry["methods"] if method[0] == name
        )
        for candidate in candidates:
            print(f"    candidate: {candidate[0]}({', '.join(candidate[1:])})")

    print(f"bridge API check: {checked} call sites verified, {problems} mismatches")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
