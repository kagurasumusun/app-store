#!/usr/bin/env bash
# Builds and runs the portable IPAShelf core tests.
#
# The iOS application compiles the same sources with Xcode; this script keeps
# the protocol layer honest on any host with a C++20 compiler and a C compiler.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${root}/build/core-tests"
cxx="${CXX:-c++}"
cc="${CC:-cc}"
std="${CXX_STD:--std=c++20}"
warnings=(-Wall -Wextra -Werror -Wno-unused-parameter)
# macOS still ships bash 3.2, where expanding an empty array under `set -u`
# aborts the script. ${arr[@]+"${arr[@]}"} keeps the script portable.
sanitizers=()
if [[ "${SANITIZE:-0}" == "1" ]]; then
  sanitizers=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi

mkdir -p "${build_dir}"

c_sources=(
  "ThirdParty/miniz/miniz.c"
  "ThirdParty/miniz/miniz_tdef.c"
  "ThirdParty/miniz/miniz_tinfl.c"
  "ThirdParty/miniz/miniz_zip.c"
)
cpp_sources=(
  "Core/src/base.cpp"
  "Core/src/crypto.cpp"
  "Core/src/json.cpp"
  "Core/src/plist.cpp"
  "Core/src/url.cpp"
  "Core/src/models.cpp"
  "Core/src/http.cpp"
  "Core/src/bag.cpp"
  "Core/src/sap.cpp"
  "Core/src/store_client.cpp"
  "Core/src/ipa.cpp"
  "Core/src/ota.cpp"
  "Core/src/x509.cpp"
  "Core/tests/test_core.cpp"
)

objects=()
for source in "${c_sources[@]}"; do
  object="${build_dir}/$(basename "${source%.c}").o"
  "${cc}" -O2 -g -I"${root}/ThirdParty/miniz" -c "${root}/${source}" -o "${object}"
  objects+=("${object}")
done
for source in "${cpp_sources[@]}"; do
  object="${build_dir}/$(basename "${source%.cpp}").o"
  "${cxx}" "${std}" "${warnings[@]}" ${sanitizers[@]+"${sanitizers[@]}"} -O1 -g \
    -I"${root}/Core/include" -I"${root}/Core/tests" -I"${root}/ThirdParty/miniz" \
    -c "${root}/${source}" -o "${object}"
  objects+=("${object}")
done

"${cxx}" "${std}" "${warnings[@]}" ${sanitizers[@]+"${sanitizers[@]}"} "${objects[@]}" \
  -pthread -o "${build_dir}/test_core"

fixtures="${build_dir}/fixtures"
rm -rf "${fixtures}"
mkdir -p "${fixtures}"

python3 - "${fixtures}/sample.bplist" <<'PY'
import datetime
import plistlib
import sys

document = {
    "CFBundleIdentifier": "com.example.sample",
    "CFBundleShortVersionString": "2.4.1",
    "CFBundleVersion": "241",
    "BuildMachineOSBuild": "24A430",
    "LSMinimumSystemVersion": "18.0",
    "NSHumanReadableCopyright": "© 2026 Example",
    "IsBeta": False,
    "ItemCount": 42,
    "Rating": 4.5,
    "LaunchDate": datetime.datetime(2026, 9, 1, 12, 30, 0),
    "Payload": b"\x00\x01\x02\xfe\xff",
    "SupportedDevices": ["iPhone17,1", "iPad16,3"],
    "Nested": {"flag": True, "items": [1, 2, 3]},
}
with open(sys.argv[1], "wb") as handle:
    plistlib.dump(document, handle, fmt=plistlib.FMT_BINARY, sort_keys=False)
PY

python3 - "${fixtures}/sample.ipa" <<'PY'
import plistlib
import sys
import zipfile

info = {
    "CFBundleIdentifier": "com.example.sample",
    "CFBundleShortVersionString": "3.1.4",
    "CFBundleVersion": "314",
    "CFBundleExecutable": "Sample",
    "CFBundleSupportedPlatforms": ["iPhoneOS"],
    "MinimumOSVersion": "18.0",
}
with zipfile.ZipFile(sys.argv[1], "w", zipfile.ZIP_DEFLATED) as archive:
    archive.writestr("Payload/Sample.app/Info.plist",
                     plistlib.dumps(info, fmt=plistlib.FMT_BINARY))
    archive.writestr("Payload/Sample.app/Sample", b"MACH-O-PAYLOAD")
    archive.writestr("Payload/Sample.app/SC_Info/Sample.sinf", b"original-sinf")
    archive.writestr("Payload/Sample.app/Assets.car", b"\x00" * 2048)
    archive.writestr("iTunesMetadata.plist", b"stale-metadata")
PY

"${build_dir}/test_core" "${fixtures}"

python3 - "${fixtures}/generated.bplist" "${fixtures}/patched.ipa" <<'PY'
import plistlib
import sys
import zipfile

with open(sys.argv[1], "rb") as handle:
    document = plistlib.load(handle)
assert document["Name"] == "IpaStore"
assert document["Unicode"] == "\u65e5\u672c\u8a9e \U0001F680"
assert document["Large"] == 9000000000
assert document["Negative"] == -42
assert document["List"] == list(range(20))
assert document["Blob"] == b"payload"
print("python plistlib validation: ok")

with zipfile.ZipFile(sys.argv[2]) as archive:
    names = set(archive.namelist())
    assert "Payload/Sample.app/Sample" in names
    assert "Payload/Sample.app/Assets.car" in names
    assert "iTunesArtwork" in names
    assert archive.read("Payload/Sample.app/Sample") == b"MACH-O-PAYLOAD"
    assert archive.read("Payload/Sample.app/SC_Info/Sample.sinf") == b"patched-sinf"
    metadata = plistlib.loads(archive.read("iTunesMetadata.plist"))
    assert metadata["apple-id"] == "user@example.com"
    assert metadata["itemId"] == 123456789
    assert metadata["SinfPaths"] == ["Payload/Sample.app/SC_Info/Sample.sinf"]
print("python IPA validation: ok")
PY

python3 - "${fixtures}/install.plist" <<'PY'
import plistlib
import sys

with open(sys.argv[1], "rb") as handle:
    document = plistlib.load(handle)
items = document["items"]
assert len(items) == 1
assets = items[0]["assets"]
assert [asset["kind"] for asset in assets] == [
    "software-package",
    "display-image",
    "full-size-image",
]
assert assets[0]["url"].startswith("https://")
metadata = items[0]["metadata"]
assert metadata["bundle-identifier"] == "com.example.sample"
assert metadata["bundle-version"] == "4.2.0"
assert metadata["kind"] == "software"
assert metadata["title"] == "Sample <&>"
print("python OTA manifest validation: ok")
PY

spki_text="$(openssl pkey -pubin -inform DER -in "${fixtures}/test-key.der" -text -noout)"
grep -q "prime256v1" <<<"${spki_text}"
grep -q "P-256" <<<"${spki_text}"
echo "openssl SPKI validation: ok"

ca_text="$(openssl x509 -inform DER -in "${fixtures}/test-ca.der" -text -noout)"
grep -q "CA:TRUE" <<<"${ca_text}"
grep -q "Certificate Sign" <<<"${ca_text}"
grep -q "CN = IPAShelf Root CA" <<<"${ca_text}"
echo "openssl CA certificate validation: ok"

leaf_text="$(openssl x509 -inform DER -in "${fixtures}/test-cert.der" -text -noout)"
grep -q "CA:FALSE" <<<"${leaf_text}"
grep -q "DNS:localhost" <<<"${leaf_text}"
grep -q "DNS:ipashelf.local" <<<"${leaf_text}"
grep -q "IP Address:127.0.0.1" <<<"${leaf_text}"
grep -q "IP Address:192.168.1.5" <<<"${leaf_text}"
grep -q "TLS Web Server Authentication" <<<"${leaf_text}"
grep -q "Signature Algorithm: ecdsa-with-SHA256" <<<"${leaf_text}"
echo "openssl leaf certificate validation: ok"

openssl x509 -in "${fixtures}/test-cert.pem" -noout -subject | grep -q "CN = IPAShelf Device"
echo "openssl PEM validation: ok"
