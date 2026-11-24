"""Physical channel mapping, effective resource snapshots and independent audit."""
import _support
import ast
import copy
from pathlib import Path
from types import SimpleNamespace
import unittest
from memory.test_memory_config import ConvertedValue
from reporting.test_single_line_config_audit import fixture
from single_line_config_audit import audit_configuration
from cfd_reporting import dram_statistics

CONFIG=_support.PROJECT_ROOT/'configs/run_cfd_dsa.py'


class Range:
    def __init__(self,*args,**kwargs):
        self.start=0 if len(args)==1 and not kwargs else args[0]
        size=args[0] if len(args)==1 and not kwargs else kwargs['size']
        self.end=self.start+({'128MiB':134217728}.get(size,size))
        self.masks=[1<<(kwargs['intlvHighBit']-kwargs['intlvBits']+1+i) for i in range(kwargs.get('intlvBits',0))]
        self.intlvMatch=kwargs.get('intlvMatch',0)
    def size(self):return (self.end-self.start)>>len(self.masks)
    def __str__(self):
        return f'{self.start}:{self.end}'+(f':{self.intlvMatch}:'+':'.join(map(str,self.masks)) if self.masks else '')


def namespace():
    calls=[]
    def dram(**kwargs):
        calls.append(('DDR4_2400_8x8',dict(kwargs)))
        values=dict(device_size=1073741824,device_bus_width=8,devices_per_rank=8,
            ranks_per_channel=2,banks_per_rank=16,bank_groups_per_rank=4,
            device_rowbuffer_size=1024,burst_length=8,addr_mapping='RoRaBaCoCh',
            page_policy='open_adaptive',read_buffer_size=64,write_buffer_size=128,activation_limit=4)
        for key in ('tCK','tBURST','tCCD_L','tRCD','tCL','tRP','tRAS','tRRD',
                    'tRRD_L','tXAW','tRFC','tREFI','tWR','tWTR','tRTW'):
            values[key]=ConvertedValue(833 if key=='tCK' else 3332 if key=='tBURST' else 14160)
        return SimpleNamespace(**values,**kwargs)
    def controller(**kwargs):
        index=sum(k=='MemCtrl' for k,v in calls);calls.append(('MemCtrl',dict(kwargs)))
        return SimpleNamespace(path=lambda:f'system.dram_ctrls{index}',
            mem_sched_policy='frfcfs',static_frontend_latency=ConvertedValue(10000),
            static_backend_latency=ConvertedValue(10000),write_high_thresh_perc=85,
            write_low_thresh_perc=50,min_writes_per_switch=16,min_reads_per_switch=16,**kwargs)
    tree=ast.parse(CONFIG.read_text())
    names={'build_dram_configuration','describe_dram_configuration','build_dram_channels','describe_dram_channels'}
    selected=[n for n in tree.body if isinstance(n,ast.FunctionDef) and n.name in names]
    assert len(selected)==4,'physical-channel helpers are not integrated'
    result=dict(AddrRange=Range,DDR4_2400_8x8=dram,MemCtrl=controller,
        SimpleMemory=lambda **kw:SimpleNamespace(**kw))
    exec(compile(ast.Module(body=selected,type_ignores=[]),str(CONFIG),'exec'),result)
    return result,calls


def channel_fixture(channels=2):
    ns,_=namespace();memories=ns['build_dram_channels']('ddr4','128MiB','30ns','32GB/s',channels)
    spec=ns['describe_dram_channels'](memories,'ddr4','128MiB','30ns','32GB/s')
    resolved,config=fixture();resolved['memory']=spec
    system=config['system'];system['spm']=system.pop('mem_ctrl');system['spm']['path']='system.spm'
    system['spm'].update(conf_table_reported=False,in_addr_map=True,range='1879048192:1879310336')
    system['mem_ranges']=['0:134217728','1879048192:1879310336'];system['cache_line_size']=64
    actual=[]
    for i,s in enumerate(spec['controllers']):
        child=dict(type='DRAMInterface',path=s['path']+'.dram',range=s['range'],
                   **s['geometry'],**s['timing_ticks'],**s['interface'])
        actual.append(dict(type='MemCtrl',path=s['path'],dram=child,
            clk_domain='system.clk_domain',
            port={'peer':f'system.membus.mem_side_ports[{i}]'},
            **{k.removesuffix('_ticks'):v for k,v in s['controller'].items()}))
    system['dram_ctrls']=actual
    return resolved,config


class DramChannelTests(unittest.TestCase):
    def test_one_channel_uses_original_builder_and_full_window(self):
        ns,calls=namespace();mem=ns['build_dram_channels']('ddr4','128MiB','30ns','32GB/s')
        self.assertEqual(len(mem),1);self.assertEqual(mem[0].dram.range.size(),134217728)
        self.assertEqual(mem[0].dram.range.masks,[])
        self.assertEqual([k for k,v in calls],['DDR4_2400_8x8','MemCtrl'])
        spec=ns['describe_dram_channels'](mem,'ddr4','128MiB','30ns','32GB/s')
        self.assertNotIn('channels',spec)

    def test_two_real_controllers_cover_each_cache_line_exactly_once(self):
        ns,calls=namespace();mem=ns['build_dram_channels']('ddr4','128MiB','30ns','32GB/s',2)
        self.assertEqual(sum(k=='MemCtrl' for k,v in calls),2)
        for address in list(range(0,4096,8))+[134217727]:
            hits=[m for m in mem if (address>>6)&1==m.dram.range.intlvMatch]
            self.assertEqual(len(hits),1)
        self.assertEqual(sum(m.dram.range.size() for m in mem),134217728)
        self.assertTrue(all(set(v)=={'range'} for k,v in calls if k=='DDR4_2400_8x8'))

    def test_snapshot_accounts_for_two_buses_and_finite_controller_queues(self):
        resolved,_=channel_fixture();s=resolved['memory']
        self.assertEqual((s['channels'],s['interleave_bytes'],s['mapped_size_bytes']),(2,64,134217728))
        self.assertEqual(s['data_bus_width_bits'],128)
        self.assertEqual(s['device_capacity_bytes'],34359738368)
        self.assertEqual((s['read_buffer_entries'],s['write_buffer_entries']),(128,256))
        self.assertEqual([x['range'] for x in s['controllers']],['0:134217728:0:64','0:134217728:1:64'])

    def test_reject_unsupported_channel_counts_and_simple_knob_combinations(self):
        ns,calls=namespace()
        for model,n,overrides in [('ddr4',0,()),('ddr4',3,()),('simple',2,()),('ddr4',2,('--dram-bandwidth',))]:
            with self.subTest(model=model,n=n),self.assertRaises(ValueError):
                ns['build_dram_channels'](model,'128MiB','30ns','32GB/s',n,overrides)
        self.assertEqual(calls,[])

    def test_independent_audit_accepts_actual_two_channel_mapping(self):
        resolved,config=channel_fixture();result=audit_configuration(resolved,config,command=['gem5.opt','--dram-channels=2'])
        self.assertEqual(result['memory']['channel_count'],2)
        self.assertEqual(result['memory']['data_bus_width_bits'],128)
        self.assertEqual(result['memory']['mapped_size_bytes'],134217728)

    def test_audit_rejects_missing_aliasing_changed_timing_and_free_queues(self):
        resolved,config=channel_fixture()
        changes=[]
        bad=copy.deepcopy(config);bad['system']['dram_ctrls'].pop();changes.append((resolved,bad))
        bad=copy.deepcopy(config);bad['system']['dram_ctrls'][1]['dram']['range']='0:134217728:0:64';changes.append((resolved,bad))
        bad=copy.deepcopy(config);bad['system']['dram_ctrls'][1]['dram']['range']='0:134217728:1:32';changes.append((resolved,bad))
        bad=copy.deepcopy(config);bad['system']['dram_ctrls'][1]['dram']['tRCD']=1;changes.append((resolved,bad))
        bad=copy.deepcopy(config);bad['system']['dram_ctrls'][1]['port']['peer']='system.membus.mem_side_ports[0]';changes.append((resolved,bad))
        fake=copy.deepcopy(resolved);fake['memory']['write_buffer_entries']=128;changes.append((fake,config))
        fake=copy.deepcopy(resolved);fake['memory']['mapped_size_bytes']=268435456;changes.append((fake,config))
        for i,(r,c) in enumerate(changes):
            with self.subTest(mutation=i),self.assertRaises(ValueError):audit_configuration(r,c)
        with self.assertRaises(ValueError):audit_configuration(resolved,config,command=['gem5.opt','--dram-channels=1'])

    def test_stat_totals_sum_channels_not_snapshots_or_aliases(self):
        text='\n'.join(['system.dram_ctrls0.dram.bytesRead::total 10',
            'system.dram_ctrls1.dram.bytesRead::total 20','system.dram_ctrls0.dram.bytesWritten::total 3',
            'system.dram_ctrls1.dram.bytesWritten::total 4','system.dram_ctrls0.dram.bytesRead::total 30',
            'system.dram_ctrls1.dram.bytesRead::total 40'])
        self.assertEqual(dram_statistics(text),dict(dram_bytes_read=70,dram_bytes_written=7,dram_reported_channels=2))
        self.assertEqual(dram_statistics('system.dram.bytesRead::total 8\nsystem.dram.dram.bytesRead::total 9\nsystem.dram.bytesRead::total 10'),
            dict(dram_bytes_read=9,dram_reported_channels=1))

    def test_spm_must_not_become_the_general_se_allocation_pool(self):
        resolved,config=channel_fixture();config['system']['spm']['conf_table_reported']=True
        with self.assertRaisesRegex(ValueError,'SPM'):
            audit_configuration(resolved,config)


    def test_four_real_controllers_exact_64_byte_address_coverage(self):
        ns,calls=namespace();mem=ns['build_dram_channels']('ddr4','128MiB','30ns','32GB/s',4)
        self.assertEqual(sum(k=='MemCtrl' for k,v in calls),4)
        addresses=list(range(0,8192,8))+list(range(134216704,134217728,8))
        for address in addresses:
            hits=[m for m in mem if sum(((address&mask).bit_count()%2)<<i for i,mask in enumerate(m.dram.range.masks))==m.dram.range.intlvMatch]
            self.assertEqual(len(hits),1)
        self.assertEqual([m.dram.range.masks for m in mem],[[64,128]]*4)
        self.assertEqual(sum(m.dram.range.size() for m in mem),134217728)
        self.assertTrue(all(set(v)=={'range'} for k,v in calls if k=='DDR4_2400_8x8'))

    def test_four_channel_snapshot_charges_all_external_resources(self):
        resolved,_=channel_fixture(4);s=resolved['memory']
        self.assertEqual((s['channels'],s['interleave_bytes'],s['mapped_size_bytes']),(4,64,134217728))
        self.assertEqual((s['data_bus_width_bits'],s['device_capacity_bytes']),(256,68719476736))
        self.assertEqual((s['read_buffer_entries'],s['write_buffer_entries']),(256,512))
        self.assertEqual([x['mapped_size_bytes'] for x in s['controllers']],[33554432]*4)
        for group in ('geometry','timing_ticks','interface','controller'):
            self.assertTrue(all(c[group]==s['controllers'][0][group] for c in s['controllers']))

    def test_four_channel_actual_mapping_passes_independent_audit(self):
        resolved,config=channel_fixture(4)
        a=audit_configuration(resolved,config,command=['gem5.opt','--dram-channels=4'])
        self.assertEqual(a['memory']['channel_count'],4)
        self.assertEqual(a['memory']['mapped_size_bytes'],134217728)
        self.assertEqual((a['memory']['read_buffer_entries'],a['memory']['write_buffer_entries']),(256,512))

    def test_four_channel_aliases_mask_order_and_free_hardware_are_rejected(self):
        resolved,config=channel_fixture(4);changes=[]
        bad=copy.deepcopy(config);bad['system']['dram_ctrls'].pop();changes.append((resolved,bad))
        for value in ['0:134217728:2:64:128','0:134217728:3:128:64','0:134217728:3:64','0:268435456:3:64:128']:
            bad=copy.deepcopy(config);bad['system']['dram_ctrls'][3]['dram']['range']=value;changes.append((resolved,bad))
        bad=copy.deepcopy(config);bad['system']['dram_ctrls'][3]['dram']['tBURST']=1;changes.append((resolved,bad))
        bad=copy.deepcopy(config);bad['system']['dram_ctrls'][3]['port']['peer']='system.membus.mem_side_ports[1]';changes.append((resolved,bad))
        for key,value in [('read_buffer_entries',128),('write_buffer_entries',256),('data_bus_width_bits',128),('device_capacity_bytes',34359738368)]:
            fake=copy.deepcopy(resolved);fake['memory'][key]=value;changes.append((fake,config))
        for r,c in changes:
            with self.assertRaises(ValueError):audit_configuration(r,c)

    def test_four_channel_count_must_match_actual_command(self):
        resolved,config=channel_fixture(4)
        with self.assertRaises(ValueError):
            audit_configuration(resolved,config,command=['gem5.opt','--dram-channels=2'])

    def test_four_channels_reject_simple_memory_and_timing_scaling_knobs(self):
        ns,calls=namespace()
        for model,overrides in [('simple',()),('ddr4',('--dram-bandwidth',)),('ddr4',('--dram-latency',))]:
            with self.assertRaises(ValueError):
                ns['build_dram_channels'](model,'128MiB','30ns','32GB/s',4,overrides)
        self.assertEqual(calls,[])

    def test_statistics_sum_four_physical_channels_latest_snapshot(self):
        text='\n'.join(f'system.dram_ctrls{i}.dram.bytesRead::total {k*(i+1)}' for k in [10,20] for i in range(4))
        self.assertEqual(dram_statistics(text),dict(dram_bytes_read=200,dram_reported_channels=4))


if __name__=='__main__':unittest.main()
