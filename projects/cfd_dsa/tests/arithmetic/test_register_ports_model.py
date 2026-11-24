"""Compile and exercise the exact bounded RF port allocator used by gem5."""
import _support
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = _support.REPO_ROOT


class RegisterPortsModelTests(unittest.TestCase):
    def test_finite_atomic_cycle_credits_and_held_completions(self):
        with tempfile.TemporaryDirectory(prefix='cfd-register-ports-') as directory:
            binary = Path(directory)/'register-ports'
            subprocess.run(['g++', '-std=c++17', '-O2', '-Wall', '-Wextra',
                '-I'+str(ROOT/'src'),
                str(_support.FIXTURES/'register_ports_model.cc'),
                '-o', str(binary)], check=True, capture_output=True, text=True)
            completed = subprocess.run([str(binary)], check=True,
                                       capture_output=True, text=True)
            self.assertIn('REGISTER_PORTS_MODEL_PASS', completed.stdout)


if __name__ == '__main__':
    unittest.main()
