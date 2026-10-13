from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

QUESTBRIDGE = Path(__file__).resolve().parents[1] / 'questbridge'
# The OpenXR headers the guest runtime is built against (vendored for the app's native side).
OPENXR_INCLUDE = Path(__file__).resolve().parents[2] / 'app/src/main/cpp/thirdparty/openxr/include'


@unittest.skipUnless(shutil.which('gcc') or shutil.which('cc'), 'needs a C compiler')
class QuestBridgeMathTest(unittest.TestCase):
    def test_velocity_composition(self):
        cc = shutil.which('gcc') or shutil.which('cc')
        with tempfile.TemporaryDirectory() as folder:
            binary = str(Path(folder) / 'test-qb-math')
            subprocess.run([cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-O2',
                            '-I', str(QUESTBRIDGE / 'runtime'), '-I', str(QUESTBRIDGE / 'protocol'),
                            '-I', str(OPENXR_INCLUDE),
                            '-o', binary, str(QUESTBRIDGE / 'tests' / 'test_qb_math.c'), '-lm'], check=True)
            result = subprocess.run([binary], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertIn('PASS: qb_math velocity composition', result.stdout)


if __name__ == '__main__':
    unittest.main()
