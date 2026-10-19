#!/usr/bin/env bash
# Cross-builds the d3d11-probe Windows PE (x86_64) with the mingw-w64 cross
# compiler in a native arm64 container derived from the guest-native image.
# Reuses the pinned OpenVR archive recipe from openvr-sample/openvr.env and
# builds libopenvr_api static for Windows (API sources only, no proprietary
# runtime binaries). Output:
#   app/build/questbridge/d3d11-probe/d3d11-probe.exe
# Shipped to NEITHER the main APK NOR user game folders; deliver per the
# runbook (droiddeckctl override or /sdcard) into an isolated compatdata.
set -euo pipefail

repo_root=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)
if [[ "$(uname -m)" =~ ^(arm64|aarch64)$ ]]; then
    platform=linux/arm64
else
    echo "d3d11-probe: only native arm64 builds are supported (this host is $(uname -m))" >&2
    exit 2
fi
base_image=droiddeck-guest-native:24.04-v1
image_name=droiddeck-probe-mingw:24.04-v1
for tool in docker curl tar shasum python3; do
    command -v "${tool}" >/dev/null || { echo "d3d11-probe: missing ${tool}" >&2; exit 2; }
done

python3 "${repo_root}/tools/docker_preflight.py" "${base_image}" \
    "tools/questbridge/guest-native.Dockerfile" "${platform}"
docker build --platform "${platform}" -t "${image_name}" \
    -f "${repo_root}/tools/questbridge/d3d11-probe/Dockerfile" \
    "${repo_root}/tools/questbridge/d3d11-probe"

cache_dir=${DROIDDECK_BUILD_CACHE:-"${HOME}/.cache/droiddeck-build"}
mkdir -p "${cache_dir}"
# cached <sha256> <name> <command...>: prints the cached path, running the command (which writes
# to "$out") only when the cache has no file with that checksum.
cached() {
    local want="$1" name="$2"
    shift 2
    local path="${cache_dir}/${name}"
    if [[ -f "${path}" ]] && echo "${want}  ${path}" | shasum -a 256 -c - >/dev/null 2>&1; then
        echo "${path}"
        return 0
    fi
    local out="${path}.tmp.$$"
    rm -f -- "${out}"
    out="${out}" "$@"
    echo "${want}  ${out}" | shasum -a 256 -c - >/dev/null
    mv -f -- "${out}" "${path}"
    echo "${path}"
}

. "${repo_root}/tools/questbridge/openvr-sample/openvr.env"
openvr_archive=$(cached "${OPENVR_SHA256}" "openvr-${OPENVR_COMMIT}.tar.gz" \
    bash -c 'curl -fsSL --retry 3 -o "$out" "$0"' "${OPENVR_URL}")

work=${cache_dir}/d3d11-probe-work
rm -rf -- "${work}/src" "${work}/out"
mkdir -p "${work}/src" "${work}/out"
tar -xzf "${openvr_archive}" -C "${work}/src" --strip-components=1

docker run --rm -i --platform "${platform}" --user "$(id -u):$(id -g)" \
    -v "${work}:/work" \
    -v "${repo_root}/tools/questbridge/d3d11-probe:/recipe:ro" \
    -w /work/src "${image_name}" bash -s <<'GUEST'
set -euo pipefail
# Static OpenVR requires its WIN32 selector as well as MinGW's built-in _WIN32.
cat > /work/out/CMakeLists.txt <<'CMAKE'
cmake_minimum_required(VERSION 3.12)
project(D3D11ProbeOpenVR LANGUAGES C CXX)
add_subdirectory(/work/src openvr)
target_compile_definitions(openvr_api64 PRIVATE WIN32)
CMAKE
cmake -S /work/out -B build-win -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_SYSTEM_NAME=Windows \
    -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
    -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++ \
    -DCMAKE_C_FLAGS="-Wno-error" -DCMAKE_CXX_FLAGS="-Wno-error"
cmake --build build-win -j "$(nproc)"
api_lib=/work/src/bin/win64/libopenvr_api64.a
test -f "${api_lib}" || { echo "GUEST: ${api_lib} not found"; exit 1; }
echo "GUEST: ${api_lib}"
x86_64-w64-mingw32-g++ -O2 -std=c++17 -Wall -DHAVE_OPENVR -DOPENVR_BUILD_STATIC -I headers \
    /recipe/d3d11_probe.cpp "${api_lib}" \
    -ld3d11 -ldxgi -ldxguid -lole32 -lshell32 -static-libstdc++ -static-libgcc \
    -o /work/out/d3d11-probe.exe
x86_64-w64-mingw32-objdump -f /work/out/d3d11-probe.exe | grep -E 'pei-x86-64|architecture'
GUEST

out_dir="${repo_root}/app/build/questbridge/d3d11-probe"
mkdir -p "${out_dir}"
test -f "${work}/src/LICENSE" || { echo "d3d11-probe: LICENSE missing from pinned OpenVR source" >&2; exit 1; }
cp -f -- "${work}/out/d3d11-probe.exe" "${out_dir}/"
cp -f -- "${work}/src/LICENSE" "${out_dir}/LICENSE.openvr"
ls -la "${out_dir}/"
