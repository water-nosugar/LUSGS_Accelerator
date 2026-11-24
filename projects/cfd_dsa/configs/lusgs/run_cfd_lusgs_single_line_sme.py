"""Run the direct single-line solver with the controller-owned SME ZA array."""

from pathlib import Path
import runpy
import sys

directory = Path(__file__).resolve().parent
sys.path.insert(0, str(directory))
from step2_profile import _append_default

if any(arg == '--coeff-compute-backend' or arg.startswith('--coeff-compute-backend=')
       for arg in sys.argv[1:]):
    raise ValueError('the SME entry point selects the compute backend')

for name, value in (
    ('--coeff-compute-backend', 'sme-za'),
    ('--coeff-compute-model', 'shared'),
    ('--coeff-base-solve', 'direct'),
    ('--coeff3-div-mode', 'reciprocal'),
    ('--coeff-warm-diagonal', 'reciprocal'),
    ('--coeff-divider-model', 'timing'),
    ('--coeff-line-storage', 'tiled'),
    ('--coeff-line-opt-level', '13'),
    ('--coeff-interface-timing', '1'),
    ('--coeff-spm-model', 'dma'),
    ('--coeff-spm-write-width', '64'),
    ('--coeff-spm-bank-granularity', '8'),
    ('--coeff-line-spm-bytes', '262144'),
    ('--coeff-line-tile-buffers', '2'),
    ('--coeff-line-tile-dma-width', '64'),
    ('--coeff-line-cache-cells', '128'),
    ('--coeff-warm-forward-tree', '0'),
    ('--coeff-warm-rhs-tree', '0'),
    ('--coeff-preprocess-trace-enable', '1'),
    ('--coeff-resource-stats', '1'),
    ('--coeff-publish-lu', '0'),
    ('--coeff-poison-outputs', '1'),
):
    _append_default(sys.argv, name, value)

sys.argv[0] = str(directory / 'run_cfd_lusgs_single_line.py')
runpy.run_path(sys.argv[0], run_name='__main__')
