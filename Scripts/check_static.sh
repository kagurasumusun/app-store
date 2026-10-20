#!/usr/bin/env bash
# Static checks for the parts of the project that cannot be compiled on a
# Linux builder (Swift and Objective-C++).
#
#   bash Scripts/check_static.sh
#
# The checks are deliberately independent of Xcode:
#   * tree-sitter-swift parses every Swift file (if the wheel is installed)
#   * check_bridge_api.py matches Swift call sites against ObjC selectors
#   * pbxproj validates the generated Xcode project
#   * a light bracket/paren balance pass covers the ObjC++ and C++ sources
set -uo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

status=0

echo "== Swift syntax (tree-sitter)"
if python3 -c "import tree_sitter, tree_sitter_swift" >/dev/null 2>&1; then
  python3 - <<'PY' || status=1
import pathlib, sys
import tree_sitter_swift
from tree_sitter import Language, Parser

parser = Parser(Language(tree_sitter_swift.language()))
failed = 0
files = sorted(pathlib.Path("IpaStore/Sources").glob("*.swift"))
for path in files:
    tree = parser.parse(path.read_bytes())
    stack = [tree.root_node]
    errors = []
    while stack:
        node = stack.pop()
        if node.type == "ERROR" or node.is_missing:
            errors.append(f"{node.start_point[0] + 1}:{node.type}")
        stack.extend(node.children)
    if errors:
        failed += 1
        print(f"  {path}: {', '.join(errors[:5])}")
print(f"  {len(files)} Swift files, {failed} with syntax errors")
sys.exit(1 if failed else 0)
PY
else
  echo "  skipped: pip install tree-sitter tree-sitter-swift"
fi

echo "== Objective-C++ bridge API"
python3 Scripts/check_bridge_api.py || status=1

echo "== Xcode project"
if python3 -c "import pbxproj" >/dev/null 2>&1; then
  python3 - <<'PY' || status=1
import sys
from pbxproj import XcodeProject

project = XcodeProject.load("IpaStore.xcodeproj/project.pbxproj")
target = project.objects.get_targets()[0]
counts = {}
for phase in target.buildPhases:
    obj = project.objects[phase]
    counts[type(obj).__name__] = len(obj.files)
print(f"  target {target.name}: {counts}")
refs = len(project.objects.get_objects_in_section("PBXFileReference"))
print(f"  {refs} file references")
sys.exit(0 if counts.get("PBXSourcesBuildPhase", 0) > 25 else 1)
PY
else
  echo "  skipped: pip install pbxproj"
fi

echo "== Xcode project references"
python3 Scripts/check_project_refs.py || status=1

echo "== Brace balance (Swift / ObjC++ / C++)"
python3 - <<'PY' || status=1
import pathlib, sys

def strip_code(text):
    out, i, n = [], 0, len(text)
    while i < n:
        ch = text[i]
        if ch == "/" and i + 1 < n and text[i+1] == "/":
            while i < n and text[i] != "\n":
                i += 1
        elif ch == "/" and i + 1 < n and text[i+1] == "*":
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i+1] == "/"):
                i += 1
            i += 2
        elif ch == '"':
            i += 1
            while i < n:
                if text[i] == "\\":
                    i += 2
                    continue
                if text[i] == '"':
                    i += 1
                    break
                i += 1
        elif ch == "'":
            i += 1
            while i < n:
                if text[i] == "\\":
                    i += 2
                    continue
                if text[i] == "'":
                    i += 1
                    break
                i += 1
        else:
            out.append(ch)
            i += 1
    return "".join(out)

patterns = ("IpaStore/Sources/*.swift", "IpaStore/Bridge/*.mm",
            "IpaStore/Bridge/*.h", "Core/src/*.cpp", "Core/include/ipashelf/*.hpp")
problems = 0
for pattern in patterns:
    for path in sorted(pathlib.Path(".").glob(pattern)):
        stack, line, ok = [], 1, True
        for ch in strip_code(path.read_text(errors="ignore")):
            if ch == "\n":
                line += 1
            elif ch in "([{":
                stack.append((ch, line))
            elif ch in ")]}":
                pairs = {")": "(", "]": "[", "}": "{"}
                if not stack or stack[-1][0] != pairs[ch]:
                    print(f"  {path}: unmatched {ch} at line {line}")
                    problems += 1
                    ok = False
                    break
                stack.pop()
        if ok and stack:
            print(f"  {path}: unclosed {stack[-1][0]} from line {stack[-1][1]}")
            problems += 1
print(f"  bracket problems: {problems}")
sys.exit(1 if problems else 0)
PY

if [ "$status" -eq 0 ]; then
  echo "STATIC CHECKS: PASS"
else
  echo "STATIC CHECKS: FAIL"
fi
exit "$status"
