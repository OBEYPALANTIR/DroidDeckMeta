#!/usr/bin/env bash
# Builds libopenvr_api (static) plus the ovr-vulkan-smoke sample for the glibc
# arm64 Arch guest, natively in Docker on arm64 hosts. Output is a single
# dynamically-glibc-linked binary with openvr_api baked in:
#   ${DROIDDECK_BUILD_CACHE:-~/.cache/droiddeck-build}/openvr-work/out/ovr-vulkan-smoke
# Deliver it with `tools/droiddeckctl override` (one file) or via /sdcard.
set -euo pipefail

repo_root=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)
if [[ "$(uname -m)" =~ ^(arm64|aarch64)$ ]]; then
    kind=native
    platform=linux/arm64
else
    echo "openvr-sample: only native arm64 builds are supported (this host is $(uname -m))" >&2
    exit 2
fi
image_name=droiddeck-guest-${kind}:24.04-v1
for tool in docker curl tar shasum python3; do
    command -v "${tool}" >/dev/null || { echo "openvr-sample: missing ${tool}" >&2; exit 2; }
done

# The base guest image already has cmake, g++ and libvulkan-dev; no derived image.
python3 "${repo_root}/tools/docker_preflight.py" "${image_name}" \
    "tools/questbridge/guest-${kind}.Dockerfile" "${platform}"

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

work=${cache_dir}/openvr-work
rm -rf -- "${work}/src" "${work}/out"
mkdir -p "${work}/src" "${work}/out"
tar -xzf "${openvr_archive}" -C "${work}/src" --strip-components=1

docker run --rm -i --platform "${platform}" --user "$(id -u):$(id -g)" \
    -v "${work}:/work" \
    -v "${repo_root}/tools/questbridge/openvr-sample:/recipe:ro" \
    -w /work/src "${image_name}" bash -s <<'GUEST'
set -euo pipefail
# Static libopenvr_api (BUILD_SHARED defaults OFF). Output lands in bin/linux64
cmake -S . -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS="-Wno-error" -DCMAKE_CXX_FLAGS="-Wno-error"
cmake --build build -j "$(nproc)"
api_lib=$(find bin build -name 'libopenvr_api.a' 2>/dev/null | head -1)
test -n "${api_lib}" || { echo "GUEST: libopenvr_api.a not found"; find bin -maxdepth 3 | head; exit 1; }
echo "GUEST: ${api_lib}"
readelf -h "${api_lib}" | grep -E 'Class|Machine'
# The smoke: openvr_api static, glibc + libvulkan dynamic (guest has both).
g++ -O2 -std=c++17 -Wall -I headers /recipe/ovr_vulkan_smoke.cpp "${api_lib}" \
    -lvulkan -ldl -lpthread -o /work/out/ovr-vulkan-smoke
readelf -h /work/out/ovr-vulkan-smoke | grep -E 'Class|Machine'
readelf -d /work/out/ovr-vulkan-smoke | grep NEEDED
GUEST

ls -la "${work}/out/"
