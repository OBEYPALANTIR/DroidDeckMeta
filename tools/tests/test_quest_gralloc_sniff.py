"""The Quest u_gralloc patch (tools/quest/mesa-patches/gralloc_ubwc_detect.py) must read the UBWC
flag from the right int for every handle layout it claims to know, compiled as C."""
import pathlib
import runpy
import subprocess
import sys
import tempfile
import textwrap
import types
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = ROOT / 'tools/quest/mesa-patches/gralloc_ubwc_detect.py'
LINEAR, COMPRESSED, INVALID = 0, 1, 2

HARNESS = textwrap.dedent("""
    #include <stdbool.h>
    #include <stdint.h>
    #include <stdio.h>
    #include <stdlib.h>
    #define DRM_FORMAT_MOD_LINEAR %d
    #define DRM_FORMAT_MOD_QCOM_COMPRESSED %d
    struct native_handle { int version, numFds, numInts; int data[64]; };
    struct u_gralloc_buffer_handle { const struct native_handle *handle; };
    struct u_gralloc_buffer_basic_info { unsigned long long modifier; };
    static void sniff(struct u_gralloc_buffer_handle *hnd, struct u_gralloc_buffer_basic_info *out) {
       out->modifier = %d;
    %s
          out->modifier = ubwc ? DRM_FORMAT_MOD_QCOM_COMPRESSED : DRM_FORMAT_MOD_LINEAR;
       }
    }
    int main(int argc, char **argv) {
       struct native_handle h = { 0, atoi(argv[1]), atoi(argv[2]) };
       for (int i = 0; i < h.numFds + h.numInts; i++) h.data[i] = (int)strtoul(argv[3 + i], NULL, 0);
       struct u_gralloc_buffer_handle hnd = { &h };
       struct u_gralloc_buffer_basic_info out;
       sniff(&hnd, &out);
       printf("%%llu", out.modifier);
       return 0;
    }
""")


def patch_text():
    captured = {}
    stub = types.ModuleType('mesa_edit')
    stub.replace = lambda path, old, new, what: captured.update(old=old, new=new)
    sys.modules['mesa_edit'] = stub
    try:
        runpy.run_path(str(SCRIPT))
    finally:
        del sys.modules['mesa_edit']
    return captured


class QuestGrallocSniff(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        source = pathlib.Path(cls.temp.name) / 'sniff.c'
        source.write_text(HARNESS % (LINEAR, COMPRESSED, INVALID, patch_text()['new']))
        cls.binary = pathlib.Path(cls.temp.name) / 'sniff'
        subprocess.run(['cc', '-Wall', '-Werror', str(source), '-o', str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def sniff(self, fds, ints):
        args = [str(fds), str(len(ints))] + ['%#x' % v for v in [3] * fds + ints]
        return int(subprocess.run([str(self.binary)] + args, capture_output=True, text=True, check=True).stdout)

    GMSM = 0x676d736d
    UBWC = 0x08000000

    def test_qti_handle_reads_flags_after_the_magic(self):
        self.assertEqual(self.sniff(2, [self.GMSM, self.UBWC, 1920, 1080]), COMPRESSED)
        self.assertEqual(self.sniff(2, [self.GMSM, 0, 1920, 1080]), LINEAR)

    def test_quest_handle_has_one_int_before_the_magic(self):
        # libgralloccore.so on Quest 3 (vros 209): {fd, fd_metadata, 1, 'gmsm', flags, ...}, 2 fds / 27 ints.
        self.assertEqual(self.sniff(2, [1, self.GMSM, self.UBWC] + [0] * 24), COMPRESSED)
        self.assertEqual(self.sniff(2, [1, self.GMSM, 0] + [0] * 24), LINEAR)
        # The magic's own bits must never be read as the flags (bit 27 of 'gmsm' is clear).
        self.assertEqual(self.sniff(4, [1, self.GMSM, self.UBWC] + [0] * 51), COMPRESSED)
        # A magic at slot 1 with no flags int after it stays unknown; the magic is never the flags.
        self.assertEqual(self.sniff(2, [1, self.GMSM]), INVALID)

    def test_magicless_handle_keeps_the_gmsm_less_read(self):
        self.assertEqual(self.sniff(2, [self.UBWC, 0]), LINEAR)
        self.assertEqual(self.sniff(2, [0, self.UBWC]), COMPRESSED)

    def test_too_few_ints_stays_invalid(self):
        self.assertEqual(self.sniff(1, [self.GMSM]), INVALID)
        self.assertEqual(self.sniff(1, []), INVALID)


if __name__ == '__main__':
    unittest.main()
