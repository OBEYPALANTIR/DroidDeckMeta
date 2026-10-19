#!/usr/bin/env bash
# Builds the diagnostic Quest Turnip: the Mesa pinned in mesa.env, patched by
# patch-native-import.py, compiled with the Android NDK against the real Android 14 platform
# headers and arm64 libraries that stage-sysroot.py staged from android-sysroot.json
# (android-stub=false, HIDL mapper4 gralloc backend, KGSL kernel driver).
# Output: app/build/questbridge/native-driver/libvulkan_freedreno.so (stripped) and provenance.json
# beside it. Nothing is packaged or installed.
#
#   tools/questbridge/native-driver/build-driver.sh <staged-sysroot-root> <android-ndk-root>
set -euo pipefail

usage='usage: build-driver.sh <staged-sysroot-root> <android-ndk-root>'
here=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(CDPATH= cd -- "${here}/../../.." && pwd)
sysroot=$(CDPATH= cd -- "${1:?${usage}}" && pwd)
ndk_root=$(CDPATH= cd -- "${2:?${usage}}" && pwd)
# The Quest runs Android 14. SDK 34 makes Mesa probe the HIDL mapper4 dependency and never the
# SDK 35+ ui (mapper5) one.
api=34
out_dir=${repo_root}/app/build/questbridge/native-driver
# A failed build must not leave an earlier driver looking current.
rm -f -- "${out_dir}/libvulkan_freedreno.so" "${out_dir}/provenance.json"

# macOS ships bison 2.3; Mesa's freedreno parsers need a current one (brew install bison, keg-only).
if [[ -d /opt/homebrew/opt/bison/bin ]]; then
    PATH="/opt/homebrew/opt/bison/bin:${PATH}"
fi
for tool in meson ninja pkg-config python3 glslangValidator bison flex curl shasum tar; do
    command -v "${tool}" >/dev/null || { echo "build-driver.sh: ${tool} not found" >&2; exit 1; }
done
hosts=("${ndk_root}"/toolchains/llvm/prebuilt/*/bin)
bin=${hosts[0]}
if [[ ${#hosts[@]} != 1 || ! -x "${bin}/aarch64-linux-android${api}-clang" ]]; then
    echo "build-driver.sh: ${ndk_root} has no single host toolchain with aarch64-linux-android${api}-clang" >&2
    exit 1
fi

# Pinned downloads are kept between builds, named by their checksum, so a rebuild fetches nothing
# it already has. A cached file is checked again before use; a bad one is fetched anew.
cache_dir=${DROIDDECK_BUILD_CACHE:-"${HOME}/.cache/droiddeck-build"}/quest-native-mapper/mesa
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
    # Command substitution clears errexit, so every failed step must return explicitly.
    rm -f -- "${out}.part" || return
    out="${out}.part" "$@" >&2 || { rm -f -- "${out}.part"; return 1; }
    printf '%s  %s\n' "${sha}" "${out}.part" | shasum -a 256 -c - >&2 || {
        rm -f -- "${out}.part"
        return 1
    }
    mv -- "${out}.part" "${out}" || { rm -f -- "${out}.part"; return 1; }
    echo "${out}"
}

. "${here}/mesa.env"
archive=$(cached "${MESA_SHA256}" "mesa-${MESA_COMMIT}.tar.gz" \
    bash -c 'curl -fsSL --retry 3 -o "$out" "$0"' "${MESA_URL}")

# Sources and the build directory are recreated each run, so no earlier patch state survives.
work=${cache_dir}/work
src=${work}/src
build=${work}/build
rm -rf -- "${work}"
mkdir -p "${src}" "${work}/pkgconfig"
# git archive records its commit in the pax global header: tie the checksum to MESA_COMMIT.
python3 - "${archive}" "${MESA_COMMIT}" <<'PY'
import sys
import tarfile

with tarfile.open(sys.argv[1]) as tar:
    if tar.pax_headers.get('comment') != sys.argv[2]:
        sys.exit(f'{sys.argv[1]}: not a git archive of Mesa {sys.argv[2]}')
PY
tar -xzf "${archive}" -C "${src}" --strip-components=1
python3 "${here}/patch-native-import.py" "${src}" | tee "${work}/patched.txt"

# Mesa's non-stub Android dependencies (meson.build) as pkg-config modules over the staged platform
# modules: Cflags are each module's exported include dirs, Libs its arm64 library. The module sets
# follow Mesa's own android/Android.mk (mapper = mapper@4.0, gralloctypes, hidlbase, utils).
python3 - "${here}/android-sysroot.json" "${sysroot}" "${work}" "${bin}" "${api}" <<'PY'
import hashlib
import json
import sys
from pathlib import Path

manifest_path, root, work, bin_dir, api = sys.argv[1:]
manifest, root, work = json.loads(Path(manifest_path).read_text()), Path(root), Path(work)
if any(c.isspace() or c in '\'"\\' for c in str(root)):
    sys.exit(f'{root}: pkg-config and meson need a sysroot path without spaces or quotes')
for entry in manifest['files']:
    path = root / entry['path']
    if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != entry['sha256']:
        sys.exit(f'{path}: missing, or not the file android-sysroot.json pins; rerun stage-sysroot.py')
exports, libs = manifest['module_exports'], manifest['libs']


def module(name, version, modules, libraries):
    dirs = dict.fromkeys(d for m in modules for d in exports[m])
    lines = [f'Name: {name}',
             f'Description: {" ".join(modules)} ({manifest["tag"]}, vndk {manifest["revision"]})',
             f'Version: {version}',
             'Cflags: ' + ' '.join(f'-isystem {root / d}' for d in dirs)]
    if libraries:
        lines.append('Libs: ' + ' '.join(str(root / libs[lib]) for lib in libraries))
    (work / 'pkgconfig' / f'{name}.pc').write_text('\n'.join(lines) + '\n')


module('cutils', '34', ['libcutils'], ['libcutils.so'])
module('hardware', '34', ['libhardware'], ['libhardware.so'])
module('log', '34', ['liblog'], ['liblog.so'])
module('sync', '34', ['libsync'], ['libsync.so'])
module('nativewindow', '34', ['libnativewindow', 'libnativebase_headers'], ['libnativewindow.so'])
module('android-hwvulkan-headers', '34', ['hwvulkan_headers'], [])
module('android.hardware.graphics.mapper', '4.0',
       ['android.hardware.graphics.mapper@4.0', 'libgralloctypes', 'libhidlbase', 'libutils'],
       ['android.hardware.graphics.mapper@4.0.so', 'libgralloctypes.so', 'libhidlbase.so',
        'libutils.so'])

# Gralloc4 and IMapper pass std::vector/std::string, so all of Mesa's C++ must use the platform
# libc++ (std::__1) the mapper libraries were built with, not the NDK's (std::__ndk1).
cpp_args = ['-nostdinc++'] + [a for d in manifest['libcxx_include_roots'] for a in ('-isystem', str(root / d))]
(work / 'cross.ini').write_text(f'''\
[binaries]
c = ['{bin_dir}/aarch64-linux-android{api}-clang']
cpp = ['{bin_dir}/aarch64-linux-android{api}-clang++']
ar = '{bin_dir}/llvm-ar'
strip = '{bin_dir}/llvm-strip'
pkg-config = ['env', 'PKG_CONFIG_LIBDIR={work}/pkgconfig', 'PKG_CONFIG_PATH=', 'PKG_CONFIG_SYSROOT_DIR=', 'pkg-config']

[built-in options]
cpp_args = {cpp_args!r}
cpp_link_args = ['-nostdlib++', '{root / manifest["cxx_abi"]["runtime"]}']

[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'armv8'
endian = 'little'
''')
PY

# nofallback: every dependency comes from the NDK or the staged sysroot, never a downloaded wrap.
meson_options=(--wrap-mode=nofallback -Dbuildtype=release -Dplatforms=android
    -Dplatform-sdk-version="${api}" -Dandroid-stub=false -Dandroid-libbacktrace=disabled
    -Dvulkan-drivers=freedreno -Dfreedreno-kmds=kgsl -Dgallium-drivers= -Degl=disabled)
meson setup "${build}" "${src}" --cross-file "${work}/cross.ini" "${meson_options[@]}"

# The diagnostics only mean something with the real HIDL mapper4 backend: prove the configured
# build uses it (not the android-stub libraries, not the SDK 35+ mapper5 file) before compiling.
python3 - "${build}/meson-info" "${api}" <<'PY'
import json
import sys
from pathlib import Path

info, api = Path(sys.argv[1]), int(sys.argv[2])
options = {o['name']: o['value'] for o in json.loads((info / 'intro-buildoptions.json').read_text())}
for name, value in {'platforms': ['android'], 'platform-sdk-version': api, 'android-stub': False,
                    'vulkan-drivers': ['freedreno'], 'freedreno-kmds': ['kgsl']}.items():
    if options.get(name) != value:
        sys.exit(f'meson option {name} is {options.get(name)!r}, expected {value!r}')
targets = json.loads((info / 'intro-targets.json').read_text())
if any('android_stub' in t['defined_in'] for t in targets):
    sys.exit('meson configured the android-stub libraries')
groups = [g for t in targets if t['name'] == '_mesa_u_gralloc' for g in t['target_sources']]
sources = {Path(s).name for g in groups for s in g.get('sources', [])}
if 'u_gralloc_imapper4_api.cpp' not in sources or 'u_gralloc_imapper5_api.cpp' in sources:
    sys.exit(f'u_gralloc is not built with the mapper4 backend: {sorted(sources)}')
if not any('-DUSE_IMAPPER4_METADATA_API' in g.get('parameters', []) for g in groups):
    sys.exit('u_gralloc is built without -DUSE_IMAPPER4_METADATA_API')
PY

ninja -C "${build}" src/freedreno/vulkan/libvulkan_freedreno.so
unstripped=${build}/src/freedreno/vulkan/libvulkan_freedreno.so
mkdir -p "${out_dir}"
"${bin}/llvm-strip" --strip-unneeded -o "${out_dir}/libvulkan_freedreno.so" "${unstripped}"

python3 - "${here}" "${repo_root}" "${src}" "${work}" "${sysroot}" "${ndk_root}" "${bin}" \
    "${unstripped}" "${out_dir}" "${api}" "${meson_options[@]}" <<'PY'
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path

here, repo_root, src, work, sysroot, ndk, bin_dir, unstripped, out_dir = map(Path, sys.argv[1:10])
api, meson_options = sys.argv[10], sys.argv[11:]


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(*command):
    return subprocess.run([str(c) for c in command], check=True, capture_output=True, text=True).stdout


needed = re.findall(r'\(NEEDED\)\s+Shared library: \[(.+?)\]',
                    run(bin_dir / 'llvm-readelf', '--dynamic', unstripped))
imports = run(bin_dir / 'llvm-nm', '-D', '--undefined-only', '--format=just-symbols', unstripped).split()
build_id = re.search(r'Build ID: ([0-9a-f]+)', run(bin_dir / 'llvm-readelf', '--notes', unstripped))
# Linked to the real HIDL mapper4 client on the platform C++ runtime, with the diagnostics in.
for library in ('android.hardware.graphics.mapper@4.0.so', 'libgralloctypes.so', 'libhidlbase.so',
                'libc++.so'):
    if library not in needed:
        sys.exit(f'{unstripped}: no DT_NEEDED {library} in {needed}')
for library in ('libui.so', 'libc++_shared.so'):
    if library in needed:
        sys.exit(f'{unstripped}: unexpected DT_NEEDED {library}')
if not any(s.startswith('_ZN7android8hardware8graphics6mapper4V4_07IMapper10getService') for s in imports):
    sys.exit(f'{unstripped}: does not import IMapper 4.0 getService')
if b'nativeimport' not in unstripped.read_bytes():
    sys.exit(f'{unstripped}: native import diagnostics are missing')

env = dict(line.split('=', 1) for line in (here / 'mesa.env').read_text().split())
manifest = json.loads((here / 'android-sysroot.json').read_text())
patched = [line.split(' ', 1)[1] for line in (work / 'patched.txt').read_text().splitlines()]
ndk_revision = re.search(r'Pkg\.Revision\s*=\s*(\S+)', (ndk / 'source.properties').read_text())
output = out_dir / 'libvulkan_freedreno.so'
provenance = {
    'output': {
        'path': str(output.relative_to(repo_root)),
        'sha256': sha256(output),
        'unstripped': str(unstripped),
        'unstripped_sha256': sha256(unstripped),
        'build_id': build_id.group(1) if build_id else None,
        'needed': needed,
    },
    'mesa': {
        'commit': env['MESA_COMMIT'],
        'url': env['MESA_URL'],
        'archive_sha256': env['MESA_SHA256'],
        'version': (src / 'VERSION').read_text().strip(),
    },
    'patch': {
        'script_sha256': sha256(here / 'patch-native-import.py'),
        'files': {path: sha256(src / path) for path in patched},
    },
    'gralloc_backend': 'HIDL IMapper 4.0 (u_gralloc_imapper4_api.cpp, -DUSE_IMAPPER4_METADATA_API), '
                       'android-stub=false',
    'sysroot': {
        'root': str(sysroot),
        'manifest_sha256': sha256(here / 'android-sysroot.json'),
        'tag': manifest['tag'],
        'revision': manifest['revision'],
        'pkgconfig': {p.name: p.read_text() for p in sorted((work / 'pkgconfig').iterdir())},
        'cross_file': (work / 'cross.ini').read_text(),
    },
    'toolchain': {
        'target': f'aarch64-linux-android{api}',
        'ndk': str(ndk),
        'ndk_revision': ndk_revision.group(1) if ndk_revision else None,
        'clang': run(bin_dir / 'clang', '--version').splitlines()[0],
        'meson': run('meson', '--version').strip(),
        'meson_options': meson_options,
    },
    'build_script_sha256': sha256(here / 'build-driver.sh'),
}
(out_dir / 'provenance.json').write_text(json.dumps(provenance, indent=2) + '\n')
print(output)
PY
