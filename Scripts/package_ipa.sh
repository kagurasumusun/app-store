#!/usr/bin/env bash
# Packages the arm64 device build into a distributable .ipa.
#
#   bash Scripts/package_ipa.sh
#   CONFIGURATION=Debug bash Scripts/package_ipa.sh
#   APP_PATH=... bash Scripts/package_ipa.sh
#
# Output: build/artifacts/IpaStore-<marketing version>.ipa plus a .sha256 file
# and the package details in build/logs/ipa.log.
#
# The archive is a plain zip whose root holds Payload/IpaStore.app, which is
# what Xcode, Apple Configurator and every .ipa installer expect. CI has no
# signing identity, so the binary inside is unsigned; sign the .app with a
# development or distribution profile (or run build_app.sh --sign with
# DEVELOPMENT_TEAM set) before installing it on a device.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

CONFIGURATION="${CONFIGURATION:-Release}"
APP_NAME="IpaStore"
APP_PATH="${APP_PATH:-build/DerivedData/Build/Products/${CONFIGURATION}-iphoneos/${APP_NAME}.app}"
ARTIFACT_DIR="build/artifacts"

fail() {
  echo "IPA packaging: FAIL - $*" >&2
  exit 1
}

[ -d "$APP_PATH" ] || fail "app bundle not found: $APP_PATH (build it with --device first)"
[ -f "$APP_PATH/$APP_NAME" ] || fail "executable missing inside $APP_PATH"

plist() {
  /usr/libexec/PlistBuddy -c "Print :$1" "$APP_PATH/Info.plist" 2>/dev/null || true
}

VERSION="$(plist CFBundleShortVersionString)"
[ -n "$VERSION" ] || fail "CFBundleShortVersionString is missing from Info.plist"
BUILD_NUMBER="$(plist CFBundleVersion)"
BUNDLE_ID="$(plist CFBundleIdentifier)"
[ -n "$BUNDLE_ID" ] || fail "CFBundleIdentifier is missing from Info.plist"

# --- The archive must only carry the device slice. -------------------------
ARCHITECTURES="$(lipo -archs "$APP_PATH/$APP_NAME")"
echo "architectures: $ARCHITECTURES"
case " $ARCHITECTURES " in
  *" arm64 "*) ;;
  *) fail "the binary has no arm64 slice (is this a simulator build?)" ;;
esac
case " $ARCHITECTURES " in
  *" x86_64 "*|*" i386 "*) fail "the binary contains a simulator slice" ;;
esac

PLATFORM="$(plist DTPlatformName)"
echo "platform: ${PLATFORM:-unknown}"
case "$PLATFORM" in
  ""|iphoneos) ;;
  *) fail "the bundle targets '$PLATFORM', not iphoneos" ;;
esac

# DTPlatformName can be absent from an unsigned product, so read the load
# command that actually decides where the binary runs.
if command -v vtool >/dev/null 2>&1; then
  BUILD_PLATFORM="$(vtool -show-build "$APP_PATH/$APP_NAME" 2>/dev/null \
    | awk '/platform/ {print $2; exit}')"
  echo "build platform: ${BUILD_PLATFORM:-unknown}"
  [ "$BUILD_PLATFORM" = "IOS" ] \
    || fail "the binary was built for '${BUILD_PLATFORM:-unknown}', not IOS"
fi

MINIMUM_OS="$(plist MinimumOSVersion)"
echo "minimum os: ${MINIMUM_OS:-unknown}"
[ -n "$MINIMUM_OS" ] || fail "MinimumOSVersion is missing from Info.plist"

echo "bundle id: $BUNDLE_ID"
echo "version: $VERSION ($BUILD_NUMBER)"

# A device install needs a signature; say so loudly instead of shipping a
# silently unusable archive.
if codesign -v "$APP_PATH" >/dev/null 2>&1; then
  echo "signature: valid"
  codesign -dv "$APP_PATH" 2>&1 | sed -n '1,4p'
else
  echo "signature: none (unsigned build; sign before installing on a device)"
fi

# --- Build the Payload zip. ------------------------------------------------
WORK="$(mktemp -d "${TMPDIR:-/tmp}/ipastore-ipa.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

mkdir -p "$WORK/Payload"
if command -v ditto >/dev/null 2>&1; then
  ditto "$APP_PATH" "$WORK/Payload/${APP_NAME}.app"
else
  cp -R "$APP_PATH" "$WORK/Payload/${APP_NAME}.app"
fi
[ -d "$WORK/Payload/${APP_NAME}.app" ] || fail "could not stage the app bundle"

mkdir -p "$ARTIFACT_DIR"
IPA="$ARTIFACT_DIR/${APP_NAME}-${VERSION}.ipa"
rm -f "$IPA" "$IPA.sha256"

if command -v ditto >/dev/null 2>&1; then
  # ditto keeps bundle metadata, symlinks and extended attributes intact.
  ditto -c -k --sequesterRsrc "$WORK" "$IPA"
else
  (cd "$WORK" && zip -qry "$ROOT/$IPA" Payload)
fi

[ -s "$IPA" ] || fail "the archive was not written"

# --- Verify the result reads like an .ipa. ---------------------------------
unzip -l "$IPA" | grep -q "Payload/${APP_NAME}.app/${APP_NAME}\$" \
  || fail "Payload/${APP_NAME}.app/${APP_NAME} is missing from the archive"
unzip -l "$IPA" | grep -q "Payload/${APP_NAME}.app/Info.plist" \
  || fail "Info.plist is missing from the archive"

# Round-trip the packaged Info.plist so a truncated archive cannot pass.
CHECK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/ipastore-ipa-check.XXXXXX")"
trap 'rm -rf "$WORK" "$CHECK_DIR"' EXIT
unzip -q "$IPA" "Payload/${APP_NAME}.app/Info.plist" -d "$CHECK_DIR"
PACKAGED_ID="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' \
  "$CHECK_DIR/Payload/${APP_NAME}.app/Info.plist" 2>/dev/null || true)"
[ "$PACKAGED_ID" = "$BUNDLE_ID" ] \
  || fail "packaged identifier '$PACKAGED_ID' differs from '$BUNDLE_ID'"

if command -v shasum >/dev/null 2>&1; then
  shasum -a 256 "$IPA" | awk '{print $1}' >"$IPA.sha256"
else
  sha256sum "$IPA" | awk '{print $1}' >"$IPA.sha256"
fi

SIZE="$(du -h "$IPA" | awk '{print $1}')"
echo
echo "IPA: $IPA"
echo "size: $SIZE"
echo "sha256: $(cat "$IPA.sha256")"
echo "contents:"
unzip -l "$IPA" | sed -n '1,6p'

# Read the finished archive back with an independent implementation: layout,
# plist, arm64-only slices and the build platform.
if command -v python3 >/dev/null 2>&1; then
  python3 Scripts/verify_ipa.py --expect-bundle-id "$BUNDLE_ID" "$IPA" \
    || fail "the packaged archive did not verify"
else
  echo "python3 not found: skipping the independent archive check" >&2
fi

echo "IPA packaging: PASS"

# The artifact is only reachable through the Actions UI; surface its
# identity in the Checks API as well.
if [ -n "${GITHUB_ACTIONS:-}" ]; then
  echo "::notice::device IPA $(basename "$IPA") - $SIZE, sha256 $(cat "$IPA.sha256")"
fi
