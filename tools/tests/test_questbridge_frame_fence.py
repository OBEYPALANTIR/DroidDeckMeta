from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

QUESTBRIDGE = Path(__file__).resolve().parents[1] / 'questbridge'
OPENXR_INCLUDE = Path(__file__).resolve().parents[2] / 'app/src/main/cpp/thirdparty/openxr/include'


@unittest.skipUnless(sys.platform == 'linux', 'needs Linux')
class QuestBridgeFrameFenceTest(unittest.TestCase):
    def test_sync_fd_payload_ownership_and_errors(self):
        cc = shutil.which('gcc')
        self.assertIsNotNone(cc, 'Linux frame-fence regression requires gcc')
        # Real Vulkan headers, injectable production entry points: no libvulkan linkage or GPU calls.
        with tempfile.TemporaryDirectory() as folder:
            binary = str(Path(folder) / 'test-qb-frame-fence')
            compiled = subprocess.run(
                [cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-O2',
                 '-I', str(QUESTBRIDGE / 'runtime'), '-I', str(QUESTBRIDGE / 'protocol'),
                 '-I', str(OPENXR_INCLUDE), '-o', binary,
                 str(QUESTBRIDGE / 'tests' / 'test_qb_frame_fence.c'), '-pthread', '-ldl'],
                capture_output=True, text=True, timeout=30)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            result = subprocess.run([binary], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('PASS: qb_vk_end_frame_fence payload ownership and error propagation', result.stdout)


if __name__ == '__main__':
    unittest.main()
