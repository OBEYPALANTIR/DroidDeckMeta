#!/usr/bin/env python3
"""Build the Quest allocation adapter around the Quest Android Turnip (tools/quest/turnip-quest-base.tzst,
produced by tools/quest/build-mesa-android.sh)."""
import io
import json
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[2]
BASE = ROOT / 'tools/quest/turnip-quest-base.tzst'
OUTPUT = ROOT / 'app/build/prepared-assets/graphics_driver/adrenotools-turnip-quest.tzst'
# Mesa (MIT) + DroidDeck's GPL-3.0 patch set: the shipped driver must carry these texts.
REQUIRED_NOTICES = ('NOTICE', 'LICENSE.mesa', 'LICENSE.mesa.MIT', 'LICENSE.GPL-3.0', 'PROVENANCE')


def build(ndk, output=OUTPUT):
    hosts = list((Path(ndk) / 'toolchains/llvm/prebuilt').glob('*'))
    if len(hosts) != 1:
        raise ValueError('Expected one NDK host toolchain')
    llvm = hosts[0] / 'bin'
    data = subprocess.run(['zstd', '-dc', str(BASE)], capture_output=True, check=True).stdout
    with tempfile.TemporaryDirectory(prefix='quest-driver-') as folder:
        work = Path(folder)
        with tarfile.open(fileobj=io.BytesIO(data)) as archive:
            # Read the base's flat regular files by name; no archive paths are extracted.
            files = {m.name: archive.extractfile(m).read() for m in archive.getmembers()
                     if m.isfile() and '/' not in m.name and not m.name.startswith('.')}
            base = json.loads(files.pop('meta.json'))
            library = base['libraryName']
            driver = files.pop(library)
        missing = [name for name in REQUIRED_NOTICES if name not in files]
        if missing:
            raise ValueError(f'{BASE.name} lacks {", ".join(missing)}; rebuild it with build-mesa-android.sh')
        (work / library).write_bytes(driver)
        wrapper = work / 'libquest_turnip.so'
        subprocess.run([
            str(llvm / 'aarch64-linux-android26-clang'), '-shared', '-fPIC', '-O2',
            '-Wall', '-Wextra', '-Werror', '-fvisibility=hidden',
            '-Wl,-z,max-page-size=16384', '-Wl,-soname,libquest_turnip.so',
            '-Wl,--no-as-needed', str(work / library), '-Wl,--as-needed',
            str(ROOT / 'tools/quest/android_wrapper.c'), str(ROOT / 'tools/quest/kgsl_compat.c'),
            '-ldl', '-llog', '-o', str(wrapper),
        ], check=True)
        subprocess.run([str(llvm / 'llvm-strip'), '--strip-unneeded', str(wrapper)], check=True)
        metadata = json.dumps({
            'libraryName': 'libquest_turnip.so',
            'name': base['name'] + ' - Quest GPUOBJ adapter',
            'description': base['description'] + '; wrapped with the Quest 3/3S KGSL allocation adapter.',
            'driverVersion': base['driverVersion'] + '+gpuobj1',
        }, indent=2).encode() + b'\n'
        stream = io.BytesIO()
        with tarfile.open(fileobj=stream, mode='w', format=tarfile.USTAR_FORMAT) as archive:
            # NOTICE, LICENSE.mesa and PROVENANCE from the base travel with the wrapped driver.
            for name, content in [('meta.json', metadata), ('libquest_turnip.so', wrapper.read_bytes()),
                                  (library, driver)] + sorted(files.items()):
                entry = tarfile.TarInfo(name)
                entry.size = len(content)
                entry.mode = 0o644
                archive.addfile(entry, io.BytesIO(content))
        compressed = subprocess.run(['zstd', '-q', '-19', '-c'], input=stream.getvalue(),
                                    capture_output=True, check=True).stdout
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(compressed)
        print(output)


if __name__ == '__main__':
    build(sys.argv[1])
