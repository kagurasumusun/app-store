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
  "Core/src/sap_software.cpp"
  "Core/src/sap_tables.cpp"
  "Core/src/store_client.cpp"
  "Core/src/owned_apps.cpp"
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
cp "${root}/Core/tests/sap_handshake_fixture.json" "${fixtures}/sap_handshake_fixture.json"

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

python3 - "${fixtures}/sample.ipa" "${fixtures}/sample-with-metadata.ipa" "${fixtures}/sample-store.ipa" "${fixtures}/sample-store-zip64.ipa" <<'PY'
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
manifest = {
    "SinfPaths": [
        "SC_Info/Sample.sinf",
        "Frameworks/Kit.framework/SC_Info/Kit.sinf",
    ],
}


def write_sample(path, with_metadata, with_sinf_files=True):
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("Payload/Sample.app/Info.plist",
                         plistlib.dumps(info, fmt=plistlib.FMT_BINARY))
        archive.writestr("Payload/Sample.app/Sample", b"MACH-O-PAYLOAD")
        if with_sinf_files:
            archive.writestr("Payload/Sample.app/SC_Info/Sample.sinf",
                             b"original-sinf")
        archive.writestr("Payload/Sample.app/SC_Info/Manifest.plist",
                         plistlib.dumps(manifest, fmt=plistlib.FMT_BINARY))
        archive.writestr("Payload/Sample.app/Frameworks/Kit.framework/Kit",
                         b"MACH-O-FRAMEWORK")
        if with_sinf_files:
            archive.writestr(
                "Payload/Sample.app/Frameworks/Kit.framework/SC_Info/Kit.sinf",
                b"original-kit-sinf")
        archive.writestr("Payload/Sample.app/Assets.car", b"\x00" * 2048)
        if with_metadata:
            archive.writestr("iTunesMetadata.plist", b"stale-metadata")


write_sample(sys.argv[1], with_metadata=False)
write_sample(sys.argv[2], with_metadata=True)
# The store's own shape: SC_Info declares the sinf slots in a manifest,
# but no sinf files and no metadata document exist until patched in.
write_sample(sys.argv[3], with_metadata=False, with_sinf_files=False)

# The zip64-tailed variant: identical entries, but the archive ends with a
# zip64 end-of-central-directory record + locator, as larger store packages
# do. Built by re-wrapping the store fixture's tail by hand.
import struct

data = open(sys.argv[3], "rb").read()
eocd = data.rfind(b"PK\x05\x06")
(_, _, _, entries, _, cd_size, cd_offset, comment_len) = struct.unpack_from(
    "<IHHHHIIH", data, eocd)
comment = data[eocd + 22:eocd + 22 + comment_len]
prefix = data[:eocd]
record = struct.pack("<IQHHIIQQQQ", 0x06064B50, 44, 789, 45, 0, 0,
                     entries, entries, cd_size, cd_offset)
locator = struct.pack("<IIQI", 0x07064B50, 0, len(prefix), 1)
tail = struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, 0xFFFF, 0xFFFF,
                   0xFFFFFFFF, 0xFFFFFFFF, comment_len) + comment
open(sys.argv[4], "wb").write(prefix + record + locator + tail)
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
    assert archive.read("Payload/Sample.app/SC_Info/Sample.sinf") == b"patched-sinf-1"
    assert archive.read(
        "Payload/Sample.app/Frameworks/Kit.framework/SC_Info/Kit.sinf"
    ) == b"patched-sinf-2"
    raw_metadata = archive.read("iTunesMetadata.plist")
    assert raw_metadata.startswith(b"bplist00"), "metadata must be binary plist"
    metadata = plistlib.loads(raw_metadata)
    assert metadata["apple-id"] == "user@example.com"
    assert metadata["userName"] == "user@example.com"
    assert metadata["itemId"] == 123456789
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

if [[ "${SKIP_OPENSSL_CHECKS:-0}" == "1" ]]; then
  echo "openssl checks: skipped (SKIP_OPENSSL_CHECKS=1)"
  exit 0
fi

# macOS ships LibreSSL while Linux distributions ship OpenSSL; both print the
# decoded extensions slightly differently. Compare case-insensitively and dump
# the decoded text when an expectation fails.
openssl_version="$(openssl version 2>&1 || true)"
echo "openssl: ${openssl_version}"

require_pattern() {
  local text="$1"
  local pattern="$2"
  local label="$3"
  if ! grep -qi -- "${pattern}" <<<"${text}"; then
    echo "openssl check failed: ${label}" >&2
    echo "  pattern: ${pattern}" >&2
    echo "  ${openssl_version}" >&2
    printf '%s\n' "${text}" >&2
    exit 1
  fi
}

spki_text="$(openssl pkey -pubin -inform DER -in "${fixtures}/test-key.der" -text -noout)"
require_pattern "${spki_text}" "prime256v1" "SPKI curve name"
require_pattern "${spki_text}" "P-256" "SPKI curve"
echo "openssl SPKI validation: ok"

ca_text="$(openssl x509 -inform DER -in "${fixtures}/test-ca.der" -text -noout)"
require_pattern "${ca_text}" "CA:TRUE" "CA basic constraints"
require_pattern "${ca_text}" "Certificate Sign" "CA key usage"
require_pattern "${ca_text}" "CN *= *IPAShelf Root CA" "CA subject"
echo "openssl CA certificate validation: ok"

leaf_text="$(openssl x509 -inform DER -in "${fixtures}/test-cert.der" -text -noout)"
require_pattern "${leaf_text}" "CA:FALSE" "leaf basic constraints"
require_pattern "${leaf_text}" "DNS:localhost" "leaf SAN (localhost)"
require_pattern "${leaf_text}" "DNS:ipashelf.local" "leaf SAN (host)"
require_pattern "${leaf_text}" "IP Address:127.0.0.1" "leaf SAN (loopback)"
require_pattern "${leaf_text}" "IP Address:192.168.1.5" "leaf SAN (LAN)"
require_pattern "${leaf_text}" "TLS Web Server Authentication" "leaf EKU"
require_pattern "${leaf_text}" "ecdsa-with-SHA256" "leaf signature algorithm"
echo "openssl leaf certificate validation: ok"

pem_text="$(openssl x509 -in "${fixtures}/test-cert.pem" -noout -subject)"
require_pattern "${pem_text}" "CN *= *IPAShelf Device" "PEM subject"
echo "openssl PEM validation: ok"
