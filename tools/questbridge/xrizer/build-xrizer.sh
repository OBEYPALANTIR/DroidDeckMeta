#!/usr/bin/env bash
# Builds xrizer (OpenVR on OpenXR) for the glibc arm64 Arch guest into
# app/build/questbridge/linuxfs/usr/local/lib/xrizer, the OpenVR runtime directory that
# openvrpaths.vrpath names (bin/linuxarm64/vrclient.so): natively on arm64 hosts, cross on x86_64.
# build-guest.sh recreates linuxfs from scratch, so run this after it.
set -euo pipefail

repo_root=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)
# Under qemu (colima on Apple silicon without Rosetta) the amd64 image's cargo was OOM-killed at -j2
# and deadlocked in fork at -j1, so arm64 hosts build natively unless DROIDDECK_GUEST_CROSS=1.
if [[ "$(uname -m)" =~ ^(arm64|aarch64)$ && "${DROIDDECK_GUEST_CROSS:-0}" != 1 ]]; then
    kind=native
    platform=linux/arm64
else
    kind=cross
    platform=linux/amd64
fi
image_name=droiddeck-guest-${kind}:24.04-v1-xrizer
for tool in docker curl tar shasum python3; do
    if ! command -v "${tool}" >/dev/null 2>&1; then
        echo "${tool} is required to build xrizer." >&2
        exit 1
    fi
done

# A wedged daemon used to hang indefinitely and then trigger an unnecessary image rebuild.
# The xrizer image is built FROM the guest image, so that one goes first.
python3 "${repo_root}/tools/docker_preflight.py" "droiddeck-guest-${kind}:24.04-v1" \
    "tools/questbridge/guest-${kind}.Dockerfile" "${platform}"
python3 "${repo_root}/tools/docker_preflight.py" "${image_name}" \
    "tools/questbridge/xrizer/xrizer-${kind}.Dockerfile" "${platform}"

# Pinned downloads are kept between builds, named by their checksum, so a rebuild fetches nothing
# it already has. A cached file is checked again before use; a bad one is fetched anew.
cache_dir=${DROIDDECK_BUILD_CACHE:-"${HOME}/.cache/droiddeck-build"}
mkdir -p "${cache_dir}"
# cached <sha256> <name> <command...>: prints the cached path, running the command (which writes
# to "$out") only when the cache has no file with that checksum.
cached() {
    local sha=$1 name=$2 out
    shift 2
    out="${cache_dir}/${sha}-${name}"
    if [[ -f "${out}" ]] && printf '%s  %s\n' "${sha}" "${out}" | shasum -a 256 -c - >/dev/null 2>&1; then
        echo "${out}"
        return 0
    fi
    rm -f "${out}.part"
    out="${out}.part" "$@" >&2
    printf '%s  %s\n' "${sha}" "${out}.part" | shasum -a 256 -c - >&2
    mv "${out}.part" "${out}"
    echo "${out}"
}

. "${repo_root}/tools/questbridge/xrizer/xrizer.env"
xrizer_archive=$(cached "${XRIZER_SHA256}" "xrizer-${XRIZER_COMMIT}.tar.gz" \
    bash -c 'curl -fsSL --retry 3 -o "$out" "$0"' "${XRIZER_URL}")

# Sources, the cargo home and the target directory live in the cache, outside the repo; the target
# directory survives between runs (cold build about 3 min natively), sources are recreated.
work=${cache_dir}/xrizer-work
out=app/build/questbridge/linuxfs
rm -rf -- "${work}/src" "${repo_root:?}/${out}/usr/local/lib/xrizer" \
    "${repo_root:?}/${out}/usr/local/share/licenses/xrizer"
mkdir -p "${work}/src" "${repo_root}/${out}"
tar -xzf "${xrizer_archive}" -C "${work}/src" --strip-components=1

# One rustc at a time by default: the 2 GiB colima VM has no swap.
docker run --rm -i --platform "${platform}" --user "$(id -u):$(id -g)" \
    -v "${work}:/work" -v "${repo_root}/${out}:/out" -w /work/src \
    -e CARGO_HOME=/work/cargo -e CARGO_TARGET_DIR=/work/target -e CARGO_BUILD_JOBS="${CARGO_BUILD_JOBS:-1}" \
    -e XRIZER_VERSION="${XRIZER_VERSION}" "${image_name}" bash -s <<'GUEST'
set -euo pipefail
# Like xrizer's release builds, static-openxr links the Khronos loader in, so games need no
# libopenxr_loader.so.1 on their library path; XR_RUNTIME_JSON (or active_runtime.json) still picks
# the runtime. A tarball has no git history, so XRIZER_VERSION replaces the git-version feature.
# xbuild is xrizer's `cargo xbuild`: it lays out bin/<openvr platform dir>/vrclient.so.
cargo run --locked --quiet --package xbuild -- --release --locked --target aarch64-unknown-linux-gnu \
    --no-default-features --features monado,static-openxr

lib=/out/usr/local/lib/xrizer
mkdir -p "${lib}" /out/usr/local/share/licenses/xrizer
# -L: xbuild symlinks vrclient.so to the library. Unlike build-guest.sh nothing is stripped further;
# the symbol table names the frames in xrizer's panic backtraces.
cp -RL /work/target/aarch64-unknown-linux-gnu/release/bin "${lib}/"
cp LICENSE /out/usr/local/share/licenses/xrizer/LICENSE

client=${lib}/bin/linuxarm64/vrclient.so
if ! readelf -h "${client}" | grep -q 'Machine:.*AArch64'; then
    echo "${client} is not an AArch64 ELF" >&2
    exit 1
fi
# openvr_api loads the runtime through this export (VR_LoadHmdSystemInternal in openvr_api_public.cpp).
if ! readelf --dyn-syms -W "${client}" | grep ' VRClientCoreFactory$' >/dev/null; then
    echo "${client} does not export VRClientCoreFactory" >&2
    exit 1
fi
GUEST

find "${repo_root}/${out}/usr/local/lib/xrizer" "${repo_root}/${out}/usr/local/share/licenses/xrizer" -type f |
    sed "s|^${repo_root}/||" | sort
