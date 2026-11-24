"""Build and exercise the same bounded divider class used by gem5."""
import _support
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = _support.REPO_ROOT


class DividerModelTests(unittest.TestCase):
    def test_microarchitecture_and_fp64(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / 'divider-test')
            subprocess.run(['g++', '-std=c++17', '-O2', '-I'+str(ROOT/'src'),
                            '-I'+str(ROOT/'ext/softfloat'),
                            str(_support.FIXTURES/'fp64_divider_model.cc'),
                            str(ROOT/'build/ARM/ext/softfloat/libsoftfloat.a'),
                            '-o', binary], check=True)
            completed = subprocess.run([binary], check=True, text=True, capture_output=True)
            self.assertIn('FP64_DIVIDER_MODEL_PASS', completed.stdout)


if __name__ == '__main__':
    unittest.main()
