#!/usr/bin/env bash
# Builds IPAShelf.app with xcodebuild.
#
#   bash Scripts/build_app.sh                       # iOS Simulator, Debug
#   bash Scripts/build_app.sh --device              # generic iOS device
#   bash Scripts/build_app.sh --configuration Release --device
#   bash Scripts/build_app.sh --destination 'platform=iOS Simulator,name=iPhone 18 Pro'
#
# Signing is disabled by default: CI has no provisioning profile and the
# portable core is what we want to type-check. Pass --sign to keep the
# project's automatic signing for local runs on a device.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${root}"

destination="generic/platform=iOS Simulator"
configuration="Debug"
signing=(CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY=)
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
      signing=()
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

actions=(build)
if [[ "${clean}" == "1" ]]; then
  actions=(clean build)
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
