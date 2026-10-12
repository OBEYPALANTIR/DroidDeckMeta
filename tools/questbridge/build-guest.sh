#!/usr/bin/env bash
# Cross-builds the guest OpenXR pieces (Khronos loader, hello_xr, QuestBridge runtime) for the
# glibc arm64 Arch guest into app/build/questbridge/linuxfs.
set -euo pipefail

repo_root=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
image_name=${DROIDDECK_GUEST_IMAGE:-droiddeck-guest-cross:24.04-v1}
for tool in docker curl tar shasum python3; do
    if ! command -v "${tool}" >/dev/null 2>&1; then
        echo "${tool} is required to cross-build the guest OpenXR libraries." >&2
        exit 1
    fi
done

# A wedged daemon used to hang indefinitely and then trigger an unnecessary image rebuild.
python3 "${repo_root}/tools/docker_preflight.py" "${image_name}" tools/questbridge/guest-cross.Dockerfile

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

. "${repo_root}/tools/questbridge/openxr-sdk.env"
sdk_archive=$(cached "${OPENXR_SDK_SHA256}" "OpenXR-SDK-Source-${OPENXR_SDK_TAG}.tar.gz" \
    bash -c 'curl -fsSL --retry 3 -o "$out" "$0"' "${OPENXR_SDK_URL}")

work=app/build/questbridge/work
out=app/build/questbridge/linuxfs
rm -rf -- "${repo_root:?}/${work}" "${repo_root:?}/${out}"
mkdir -p "${repo_root}/${work}" "${repo_root}/${out}"
tar -xzf "${sdk_archive}" -C "${repo_root}/${work}"

docker run --rm -i --platform linux/amd64 --user "$(id -u):$(id -g)" -v "${repo_root}:/src" -w /src \
    -e WORK="/src/${work}" -e OUT="/src/${out}" -e SDK="/src/${work}/OpenXR-SDK-Source-${OPENXR_SDK_TAG}" \
    "${image_name}" bash -s <<'GUEST'
set -euo pipefail
build=${WORK}/build
stage=${WORK}/stage
cat > "${WORK}/aarch64-linux-gnu.cmake" <<'EOF'
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
set(CMAKE_LIBRARY_ARCHITECTURE aarch64-linux-gnu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
EOF
# Linux always runs the SDK's presentation.cmake, which demands one window backend; xlib is the
# cheapest (headers plus Xxf86vm/Xrandr at configure time) and hello_xr's Vulkan path links no X
# library. GL/GLES/EGL lookups are disabled so the build stays Vulkan-only whatever the image has.
cmake -S "${SDK}" -B "${build}" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="${WORK}/aarch64-linux-gnu.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${stage}" \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DBUILD_LOADER=ON \
    -DDYNAMIC_LOADER=ON \
    -DBUILD_API_LAYERS=OFF \
    -DBUILD_TESTS=ON \
    -DBUILD_SDK_TESTS=ON \
    -DBUILD_CONFORMANCE_TESTS=OFF \
    -DBUILD_WITH_SYSTEM_JSONCPP=OFF \
    -DPRESENTATION_BACKEND=xlib \
    -DBUILD_WITH_XCB_HEADERS=OFF \
    -DBUILD_WITH_WAYLAND_HEADERS=OFF \
    -DCMAKE_DISABLE_FIND_PACKAGE_OpenGL=ON \
    -DCMAKE_DISABLE_FIND_PACKAGE_OpenGLES=ON \
    -DCMAKE_DISABLE_FIND_PACKAGE_EGL=ON
cmake --build "${build}" --target openxr_loader hello_xr
for component in Headers Loader hello_xr; do
    cmake --install "${build}" --component "${component}"
done

lib=${OUT}/usr/local/lib/droiddeck-xr
mkdir -p "${lib}" "${OUT}/usr/local/bin" "${OUT}/usr/local/share/licenses/openxr-sdk"
cp -L "${stage}/lib/libopenxr_loader.so.1" "${lib}/libopenxr_loader.so.1"
cp "${stage}/bin/hello_xr" "${lib}/hello_xr"
cp "${SDK}/LICENSE" "${OUT}/usr/local/share/licenses/openxr-sdk/LICENSE"
elves=("${lib}/libopenxr_loader.so.1" "${lib}/hello_xr")

# The runtime sources land separately; the loader and hello_xr are still useful without them.
runtime=tools/questbridge/runtime
shopt -s nullglob
sources=("${runtime}"/*.c)
if (( ${#sources[@]} )); then
    aarch64-linux-gnu-gcc -shared -fPIC -O2 -Wall -pthread -std=gnu11 \
        -I "${runtime}" -I tools/questbridge/protocol -I "${stage}/include" \
        -o "${lib}/libopenxr_questbridge.so" "${sources[@]}" -ldl -lpthread
    elves+=("${lib}/libopenxr_questbridge.so")
fi
if [[ -f "${runtime}/questbridge.json" ]]; then
    cp "${runtime}/questbridge.json" "${lib}/questbridge.json"
fi

aarch64-linux-gnu-strip --strip-unneeded "${elves[@]}"
for elf in "${elves[@]}"; do
    if ! aarch64-linux-gnu-readelf -h "${elf}" | grep -q 'Machine:.*AArch64'; then
        echo "${elf} is not an AArch64 ELF" >&2
        exit 1
    fi
done
if ! aarch64-linux-gnu-readelf -d "${lib}/hello_xr" | grep -q 'NEEDED.*\[libopenxr_loader\.so\.1\]'; then
    echo "hello_xr does not link libopenxr_loader.so.1" >&2
    exit 1
fi
GUEST

cat > "${repo_root}/${out}/usr/local/bin/droiddeck-hello-xr" <<'EOF'
#!/bin/sh
# Khronos hello_xr against the QuestBridge runtime; both stay overridable for other runtimes.
dir=/usr/local/lib/droiddeck-xr
export XR_RUNTIME_JSON="${XR_RUNTIME_JSON:-$dir/questbridge.json}"
export LD_LIBRARY_PATH="$dir${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$dir/hello_xr" -g Vulkan2 "$@"
EOF
chmod 755 "${repo_root}/${out}/usr/local/bin/droiddeck-hello-xr"
find "${repo_root}/${out}" -type f | sed "s|^${repo_root}/||" | sort
