"""The checked-in Quest base driver must ship Mesa's and the GPL-3.0 licence texts, not just a pointer."""
import importlib.util
import io
from pathlib import Path
import subprocess
import tarfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('build_driver', ROOT / 'tools/quest/build-driver.py')
build_driver = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build_driver)


class QuestDriverNotices(unittest.TestCase):
    def test_base_archive_carries_licence_texts(self):
        data = subprocess.run(['zstd', '-dc', str(build_driver.BASE)], capture_output=True, check=True).stdout
        with tarfile.open(fileobj=io.BytesIO(data)) as archive:
            files = {m.name: archive.extractfile(m).read() for m in archive.getmembers() if m.isfile()}
        for name in build_driver.REQUIRED_NOTICES:
            self.assertIn(name, files)
        self.assertIn(b'Permission is hereby granted', files['LICENSE.mesa.MIT'])
        self.assertIn(b'GNU GENERAL PUBLIC LICENSE', files['LICENSE.GPL-3.0'])
        self.assertIn(b'Version 3', files['LICENSE.GPL-3.0'])


if __name__ == '__main__':
    unittest.main()
