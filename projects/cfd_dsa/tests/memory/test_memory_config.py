"""Memory selector contracts, runnable without importing gem5 SimObjects."""
import _support
import ast
from pathlib import Path
from types import SimpleNamespace
import unittest


CONFIG = _support.PROJECT_ROOT / 'configs' / 'run_cfd_dsa.py'


class ConvertedValue:
    def __init__(self, actual, requested_float=None):
        self.actual = actual
        self.requested_float = requested_float

    def getValue(self):
        return self.actual

    def __float__(self):
        return self.requested_float


class MemoryConfigurationTests(unittest.TestCase):
    def setUp(self):
        tree = ast.parse(CONFIG.read_text())
        selected = [node for node in tree.body if isinstance(node, ast.FunctionDef)
                    and node.name in ('build_dram_configuration',
                                      'describe_dram_configuration')]
        self.assertEqual(len(selected), 2)
        self.calls = []

        def constructor(kind):
            def create(**kwargs):
                obj = SimpleNamespace(kind=kind, **kwargs)
                self.calls.append(obj)
                return obj
            return create

        self.namespace = dict(AddrRange=lambda size: ('range', size),
            SimpleMemory=constructor('SimpleMemory'),
            MemCtrl=constructor('MemCtrl'),
            DDR4_2400_8x8=constructor('DDR4_2400_8x8'))
        exec(compile(ast.Module(body=selected, type_ignores=[]),
                     str(CONFIG), 'exec'), self.namespace)
        self.build = self.namespace['build_dram_configuration']
        self.describe = self.namespace['describe_dram_configuration']

    def test_legacy_simple_memory_constructor_unchanged(self):
        obj = self.build('simple', '128MiB', '30ns', '32GB/s')
        self.assertEqual(vars(obj), dict(kind='SimpleMemory',
            range=('range', '128MiB'), latency='30ns', bandwidth='32GB/s'))
        self.assertEqual(len(self.calls), 1)

    def test_simple_latency_and_bandwidth_sweep_reaches_memory(self):
        obj = self.build('simple', '128MiB', '65ns', '16GB/s',
                         ('--dram-latency', '--dram-bandwidth'))
        self.assertEqual((obj.latency, obj.bandwidth), ('65ns', '16GB/s'))

    def test_ddr4_uses_unmodified_preset_and_separate_mapping(self):
        obj = self.build('ddr4', '128MiB', '30ns', '32GB/s')
        self.assertEqual(obj.kind, 'MemCtrl')
        self.assertEqual(vars(obj.dram), dict(kind='DDR4_2400_8x8',
                                             range=('range', '128MiB')))
        # Do not shrink physical chip geometry to match the mapped address window.
        self.assertNotIn('device_size', vars(obj.dram))

    def test_rejects_unknown_model_and_silently_ignored_knobs(self):
        for model, overrides in [('typo', ()),
                ('ddr4', ('--dram-latency',)),
                ('ddr4', ('--dram-bandwidth',))]:
            with self.subTest(model=model, overrides=overrides):
                with self.assertRaises(ValueError):
                    self.build(model, '128MiB', '30ns', '32GB/s', overrides)
        self.assertEqual(self.calls, [])

    def test_simple_snapshot_records_quantized_parameters(self):
        memory = SimpleNamespace(range=SimpleNamespace(size=lambda: 134217728),
            latency=ConvertedValue(30000), latency_var=ConvertedValue(0),
            bandwidth=ConvertedValue(29.0, 34359738368.0))
        spec = self.describe(memory, 'simple', '128MiB', '30ns', '32GB/s')
        self.assertEqual(spec['latency_ticks'], 30000)
        self.assertEqual(spec['bandwidth_ticks_per_byte'], 29.0)
        self.assertEqual(spec['requested_bandwidth_bytes_per_second'], 34359738368.0)
        self.assertEqual(spec['mapped_size_bytes'], 134217728)

    def test_ddr_snapshot_does_not_claim_simple_latency_or_bandwidth(self):
        dram = SimpleNamespace(range=SimpleNamespace(size=lambda: 134217728),
            device_size=1073741824, device_bus_width=8, devices_per_rank=8,
            ranks_per_channel=2, banks_per_rank=16, bank_groups_per_rank=4,
            device_rowbuffer_size=1024, burst_length=8,
            addr_mapping='RoRaBaCoCh', page_policy='open_adaptive',
            read_buffer_size=64, write_buffer_size=128, activation_limit=4)
        for key in ('tCK', 'tBURST', 'tCCD_L', 'tRCD', 'tCL', 'tRP', 'tRAS',
                    'tRRD', 'tRRD_L', 'tXAW', 'tRFC', 'tREFI', 'tWR', 'tWTR', 'tRTW'):
            setattr(dram, key, ConvertedValue(833))
        memory = SimpleNamespace(dram=dram, mem_sched_policy='frfcfs',
            static_frontend_latency=ConvertedValue(10000),
            static_backend_latency=ConvertedValue(10000),
            write_high_thresh_perc=85, write_low_thresh_perc=50,
            min_writes_per_switch=16, min_reads_per_switch=16)
        spec = self.describe(memory, 'ddr4', '128MiB', '30ns', '32GB/s')
        self.assertEqual(spec['preset'], 'DDR4_2400_8x8')
        self.assertNotIn('latency', spec)
        self.assertNotIn('bandwidth', spec)
        self.assertEqual(spec['device_capacity_bytes'], 17179869184)
        self.assertEqual(spec['mapped_size_bytes'], 134217728)
        self.assertEqual(spec['controller']['static_frontend_latency_ticks'], 10000)


if __name__ == '__main__':
    unittest.main()
