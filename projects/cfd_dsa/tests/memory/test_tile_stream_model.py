"""Exercise the actual bounded read-order selector used by gem5."""
import _support
from pathlib import Path
import subprocess
import tempfile
import unittest


class TileStreamModelTests(unittest.TestCase):
    def test_cell_coverage_and_unchanged_packet_work(self):
        with tempfile.TemporaryDirectory(prefix='cfd-tile-stream-') as directory:
            binary = Path(directory)/'tile-stream'
            subprocess.run(['g++', '-std=c++17', '-O2', '-Wall', '-Wextra',
                '-I'+str(_support.REPO_ROOT/'src'),
                str(_support.FIXTURES/'tile_stream_model.cc'),
                '-o', str(binary)], check=True, capture_output=True, text=True)
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True)
            self.assertIn('TILE_STREAM_MODEL_PASS', result.stdout)


if __name__ == '__main__':
    unittest.main()
