#!/usr/bin/env bash
# Builds IPAShelf.app with xcodebuild.
#
#   bash Scripts/build_app.sh                       # iOS Simulator, Debug
#   bash Scripts/build_app.sh --device              # generic iOS device
#   DEVELOPMENT_TEAM=ABCDE12345 bash Scripts/build_app.sh --device --sign
#   bash Scripts/build_app.sh --configuration Release --device
#   bash Scripts/build_app.sh --destination 'platform=iOS Simulator,name=iPhone 18 Pro'
#
# Simulator builds are signed ad hoc: without a signature the simulator
# refuses keychain access (OSStatus -34018) and the install server cannot
# store its CA key. Device builds stay unsigned because CI has no
# provisioning profile, which is what the .ipa artifact contains. Pass
# --sign to build an installable device app instead; set DEVELOPMENT_TEAM
# (and optionally PROVISIONING_PROFILE_SPECIFIER) to sign with your team.
# Pass --unsigned to disable signing entirely.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${root}"

destination="generic/platform=iOS Simulator"
configuration="Debug"
signing_mode="default"
clean=0
extra=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --device)
      destination="generic/platform=iOS"
      shift
      ;;
    --simulator)
      destination="generic/platform=iOS Simulator"
      shift
      ;;
    --destination)
      destination="$2"
      shift 2
      ;;
    --configuration)
      configuration="$2"
      shift 2
      ;;
    --clean)
      clean=1
      shift
      ;;
    --sign)
      signing_mode="project"
      shift
      ;;
    --unsigned)
      signing_mode="none"
      shift
      ;;
    --)
      shift
      extra+=("$@")
      break
      ;;
    *)
      extra+=("$1")
      shift
      ;;
  esac
done

case "${signing_mode}" in
  default)
    if [[ "${destination}" == *Simulator* ]]; then
      signing_mode="adhoc"
    else
      signing_mode="none"
    fi
    ;;
esac

case "${signing_mode}" in
  project)
    signing=()
    if [[ -n "${DEVELOPMENT_TEAM:-}" ]]; then
      signing+=(DEVELOPMENT_TEAM="${DEVELOPMENT_TEAM}")
    fi
    if [[ -n "${PROVISIONING_PROFILE_SPECIFIER:-}" ]]; then
      signing+=(PROVISIONING_PROFILE_SPECIFIER="${PROVISIONING_PROFILE_SPECIFIER}")
    fi
    ;;
  adhoc)
    signing=(CODE_SIGNING_ALLOWED=YES CODE_SIGNING_REQUIRED=YES
             CODE_SIGN_STYLE=Manual CODE_SIGN_IDENTITY=-
             PROVISIONING_PROFILE_SPECIFIER=)
    ;;
  *)
    signing=(CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY=)
    ;;
esac

actions=(build)
if [[ "${clean}" == "1" ]]; then
  actions=(clean build)
fi

if [[ "${signing_mode}" == "none" && "${destination}" != *Simulator* ]]; then
  echo
  echo "WARNING: building an unsigned device app."
  echo "The IPA produced by package_ipa.sh will not install on a real device."
  echo "To install, sign with a development/distribution identity:"
  echo "  DEVELOPMENT_TEAM=XXXXXXXXXX bash Scripts/build_app.sh --device --sign"
  echo
fi

echo "xcodebuild: ${configuration} / ${destination}"
xcodebuild \
  -project IpaStore.xcodeproj \
  -scheme IpaStore \
  -configuration "${configuration}" \
  -derivedDataPath build/DerivedData \
  -destination "${destination}" \
  ${signing[@]+"${signing[@]}"} \
  "${actions[@]}" "${extra[@]+"${extra[@]}"}"
