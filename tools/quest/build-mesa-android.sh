#!/usr/bin/env bash
# Build the Quest Android Turnip from a DD-Turnip Corresponding Source tarball
# (Droid-Deck/Drivers-CI release asset DD-Turnip-vX.Y.Z-source.tar.xz: Mesa tree +
# DroidDeck's patch recipe), with tools/quest/mesa-patches/*.py overlaid on the recipe's
# patches/scripts/. Mirrors the recipe's scripts/build_android.sh for a macOS or Linux host.
#
#   tools/quest/build-mesa-android.sh <DD-Turnip-source.tar.xz> <ndk> [work-dir]
# work-dir must not exist yet (default: a fresh mktemp directory); nothing is ever deleted.
# Output (printed last): <work-dir>/out/turnip-quest-base.tzst = adrenotools meta.json,
# libvulkan_freedreno.so, NOTICE (DroidDeck recipe), LICENSE.mesa (Mesa licence overview) and
# PROVENANCE (source tarball name + SHA-256, Mesa version, NDK, overlay patch SHA-256s).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
tarball=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
ndk_root=$2
if [ $# -ge 3 ]; then
    work=$3
    [ -e "$work" ] && { echo "work-dir $work already exists; pass a new path" >&2; exit 1; }
    mkdir -p "$work"
else
    work=$(mktemp -d "${TMPDIR:-/tmp}/quest-mesa-android.XXXXXX")
fi
host=$(ls "$ndk_root/toolchains/llvm/prebuilt")
ndk="$ndk_root/toolchains/llvm/prebuilt/$host/bin"
api=34
# macOS ships bison 2.3; ir3_parser.y needs a current bison (brew install bison, keg-only).
[ -d /opt/homebrew/opt/bison/bin ] && PATH="/opt/homebrew/opt/bison/bin:$PATH"

mkdir -p "$work/out"
tar -xJf "$tarball" -C "$work"
src=$(ls -d "$work"/DD-Turnip-*-source)
cp "$here"/mesa-patches/*.py "$src/drivers/patches/scripts/"
python3 "$src/drivers/scripts/apply_patches.py" android "$src/mesa" 2>&1 | tee "$work/out/patch.log"
grep -q 'Meta Quest' "$src/mesa/src/util/u_gralloc/u_gralloc_fallback.c"

cat > "$work/cross.ini" <<INI
[binaries]
ar = '$ndk/llvm-ar'
c = ['$ndk/aarch64-linux-android$api-clang']
cpp = ['$ndk/aarch64-linux-android$api-clang++', '-fno-exceptions', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '--start-no-unused-arguments', '-static-libstdc++', '--end-no-unused-arguments']
c_ld = '$ndk/ld.lld'
cpp_ld = '$ndk/ld.lld'
strip = '$ndk/llvm-strip'
pkg-config = ['env', 'PKG_CONFIG_LIBDIR=$work/no-pkgconfig', 'pkg-config']

[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'armv8'
endian = 'little'
INI
mkdir -p "$work/no-pkgconfig"

flags="-D__ANDROID__ -Wno-error -Wno-deprecated-declarations -Wno-incompatible-pointer-types-discards-qualifiers -Wno-incompatible-pointer-types"
CFLAGS="$flags" CXXFLAGS="$flags" meson setup "$work/build" "$src/mesa" \
  --cross-file "$work/cross.ini" --prefix "$work/install" \
  -Dbuildtype=release -Dstrip=true -Dplatforms=android -Dplatform-sdk-version=36 -Dandroid-stub=true \
  -Dandroid-libbacktrace=disabled -Dgallium-drivers= -Dvulkan-drivers=freedreno -Dvulkan-beta=true \
  -Dfreedreno-kmds=kgsl -Degl=disabled -Dvideo-codecs=
ninja -C "$work/build" -j "${BUILD_JOBS:-$(getconf _NPROCESSORS_ONLN)}" install

out="$work/out"
cp "$work/install/lib/libvulkan_freedreno.so" "$out/"
cp "$src/mesa/docs/license.rst" "$out/LICENSE.mesa"
# The licence texts themselves, as DD-Turnip's release zip ships them (LICENSES/mesa/*, GPL-3.0),
# flattened because the adrenotools archive is flat: LICENSE.mesa.MIT, LICENSE.mesa.exceptions.*, ...
(cd "$src/mesa/LICENSES" && find . -type f | sed 's|^\./||') | while read -r f; do
    cp "$src/mesa/LICENSES/$f" "$out/LICENSE.mesa.$(echo "$f" | tr / .)"
done
cp "$src/drivers/LICENSE" "$out/LICENSE.GPL-3.0"
ls "$out"/LICENSE.mesa.MIT "$out"/LICENSE.GPL-3.0 >/dev/null
cp "$src/NOTICE" "$out/NOTICE"
recipe=${src##*/}; recipe=${recipe%-source}   # e.g. DD-Turnip-v0.2.0
cat > "$out/meta.json" <<JSON
{
  "schemaVersion": 1,
  "name": "$recipe + Quest gralloc",
  "description": "Mesa $(cat "$src/mesa/VERSION"), DroidDeck $recipe recipe with Meta Quest gralloc handle detection (tools/quest/mesa-patches)",
  "author": "DroidDeckMeta",
  "packageVersion": "1",
  "vendor": "Mesa",
  "driverVersion": "$recipe-quest",
  "minApi": 29,
  "libraryName": "libvulkan_freedreno.so"
}
JSON
{
  echo "source: $(basename "$tarball") sha256 $(shasum -a 256 "$tarball" | cut -d' ' -f1)"
  echo "mesa: $(cat "$src/mesa/VERSION")"
  echo "ndk: $(sed -n 's/^Pkg.Revision *= *//p' "$ndk_root/source.properties")"
  for p in "$here"/mesa-patches/*.py; do echo "overlay: $(basename "$p") sha256 $(shasum -a 256 "$p" | cut -d' ' -f1)"; done
  echo "libvulkan_freedreno.so sha256 $(shasum -a 256 "$out/libvulkan_freedreno.so" | cut -d' ' -f1) (not bit-reproducible between builds)"
} > "$out/PROVENANCE"
tar -cf - -C "$out" meta.json libvulkan_freedreno.so NOTICE PROVENANCE $(cd "$out" && ls LICENSE*) | zstd -q -19 -c > "$out/turnip-quest-base.tzst"
echo "$out/turnip-quest-base.tzst"
