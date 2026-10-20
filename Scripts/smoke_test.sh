#!/usr/bin/env bash
# Boots a simulator, installs the built app, checks that the UI launches and
# then runs the in-app smoke test (see IpaStore/Sources/SmokeTest.swift).
#
# Usage:
#   bash Scripts/smoke_test.sh                 # uses the Debug simulator build
#   APP_PATH=... BUNDLE_ID=... bash Scripts/smoke_test.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

APP_PATH="${APP_PATH:-build/DerivedData/Build/Products/Debug-iphonesimulator/IpaStore.app}"
BUNDLE_ID="${BUNDLE_ID:-com.example.ipastore}"
LOG_DIR="$ROOT/build/logs"
SCREENSHOT="$LOG_DIR/smoke-screen.png"
SMOKE_LOG="$LOG_DIR/smoke.log"

if [ ! -d "$APP_PATH" ]; then
  echo "app bundle not found: $APP_PATH" >&2
  exit 1
fi
mkdir -p "$LOG_DIR"

# The simulator only grants keychain access to a signed app that declares a
# keychain access group; without it the install server cannot store its key.
echo "== Signature"
codesign -dv "$APP_PATH" 2>&1 | sed -n '1,6p' || true
if codesign -d --entitlements :- "$APP_PATH" 2>/dev/null \
    | grep -q "keychain-access-groups"; then
  echo "keychain-access-groups: present"
else
  echo "keychain-access-groups: missing (the smoke test will fail)" >&2
fi

fail() {
  echo "SMOKE TEST: FAIL - $*" >&2
  exit 1
}

pick_device() {
  xcrun simctl list devices available --json | python3 -c '
import json
import sys

devices = json.load(sys.stdin)["devices"]
runtimes = sorted(
    (name for name in devices if "iOS" in name),
    reverse=True,
)
for runtime in runtimes:
    for device in devices[runtime]:
        if device.get("isAvailable") and device["name"].startswith("iPhone"):
            print(device["udid"])
            raise SystemExit
'
}

UDID="$(pick_device)"
if [ -z "$UDID" ]; then
  echo "no iPhone simulator is available; creating one" >&2
  DEVICE_TYPE="$(xcrun simctl list devicetypes --json | python3 -c '
import json
import sys

types = json.load(sys.stdin)["devicetypes"]
for entry in types:
    if entry["name"].startswith("iPhone"):
        print(entry["identifier"])
        break
')"
  RUNTIME="$(xcrun simctl list runtimes --json | python3 -c '
import json
import sys

runtimes = [entry for entry in json.load(sys.stdin)["runtimes"]
            if entry.get("isAvailable") and entry["platform"] == "iOS"]
runtimes.sort(key=lambda entry: entry.get("version", ""), reverse=True)
print(runtimes[0]["identifier"] if runtimes else "")
')"
  [ -n "$DEVICE_TYPE" ] && [ -n "$RUNTIME" ] || fail "cannot create a simulator"
  UDID="$(xcrun simctl create IPAShelf-Smoke "$DEVICE_TYPE" "$RUNTIME")"
fi

echo "== Booting simulator $UDID"
xcrun simctl boot "$UDID" 2>/dev/null || true
xcrun simctl bootstatus "$UDID" -b >/dev/null

cleanup() {
  xcrun simctl terminate "$UDID" "$BUNDLE_ID" >/dev/null 2>&1 || true
  xcrun simctl shutdown "$UDID" >/dev/null 2>&1 || true
}
trap cleanup EXIT

echo "== Installing $APP_PATH"
xcrun simctl install "$UDID" "$APP_PATH"

# --- 1. The user interface must launch and stay alive. ----------------------
echo "== Launching the app"
LAUNCH_OUTPUT="$(xcrun simctl launch "$UDID" "$BUNDLE_ID" 2>&1)" || {
  echo "$LAUNCH_OUTPUT" >&2
  fail "the app did not launch"
}
echo "$LAUNCH_OUTPUT"
sleep 10
if ! xcrun simctl spawn "$UDID" launchctl list | grep -q "$BUNDLE_ID"; then
  fail "the app exited while starting up"
fi
xcrun simctl io "$UDID" screenshot "$SCREENSHOT" >/dev/null 2>&1 || true
echo "screenshot: $SCREENSHOT"
xcrun simctl terminate "$UDID" "$BUNDLE_ID" >/dev/null 2>&1 || true

# --- 2. The in-app smoke test must pass. -----------------------------------
echo "== Running the smoke test"
: >"$SMOKE_LOG"
set +e
xcrun simctl launch --console-pty "$UDID" "$BUNDLE_ID" -IPSmokeTest 1 \
  >"$SMOKE_LOG" 2>&1 &
launch_pid=$!
set -e
for _ in $(seq 1 120); do
  if grep -q -e "SMOKE: PASS" -e "SMOKE: FAIL" "$SMOKE_LOG"; then
    break
  fi
  if ! kill -0 "$launch_pid" 2>/dev/null; then
    break
  fi
  sleep 1
done
set +e
wait "$launch_pid" 2>/dev/null
set -e

cat "$SMOKE_LOG"
# Surface the individual checks through the Checks API; the raw log is only
# reachable from the Actions log viewer.
if [ -n "${GITHUB_ACTIONS:-}" ]; then
  while IFS= read -r line; do
    case "$line" in
      SMOKE:*) echo "::notice::$line" ;;
    esac
  done <"$SMOKE_LOG"
fi
if ! grep -q "SMOKE: PASS" "$SMOKE_LOG"; then
  fail "the in-app smoke test did not report SMOKE: PASS"
fi
echo "SMOKE TEST: PASS"
