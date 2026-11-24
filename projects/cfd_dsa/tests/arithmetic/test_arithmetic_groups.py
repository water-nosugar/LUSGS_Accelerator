"""Exercise the actual C++ bounded scoreboard and ownership policy."""
from pathlib import Path
import subprocess
import tempfile
import unittest


class ArithmeticGroupsTests(unittest.TestCase):
    def test_bounded_groups_partition_aging_and_registered_return(self):
        root=Path(__file__).resolve().parents[4]
        with tempfile.TemporaryDirectory() as folder:
            binary=Path(folder)/'groups'
            subprocess.run(['g++','-std=c++17','-O2','-I'+str(root/'src'),
                str(root/'projects/cfd_dsa/tests/fixtures/arithmetic_groups.cc'),
                '-o',str(binary)],check=True,capture_output=True,text=True)
            result=subprocess.run([str(binary)],check=True,capture_output=True,text=True)
            self.assertIn('ARITHMETIC_GROUPS_PASS',result.stdout)
