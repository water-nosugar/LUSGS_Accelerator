import _support
import subprocess
import tempfile
import unittest
from pathlib import Path


class ReferenceIsolationTest(unittest.TestCase):
    def test_real_oracle_preserves_hardware_outputs_on_success_and_error(self):
        source=_support.FIXTURES/'raw_reference_isolation.c'
        with tempfile.TemporaryDirectory(prefix='cfd-oracle-test-') as tmp:
            binary=Path(tmp)/'test'
            subprocess.run(['cc','-O2',str(source),'-o',str(binary),'-lm'],
                           check=True,capture_output=True,text=True)
            result=subprocess.run([str(binary)],check=True,capture_output=True,text=True)
            self.assertIn('RAW_REFERENCE_ISOLATION_PASS',result.stdout)


if __name__=='__main__':unittest.main()
