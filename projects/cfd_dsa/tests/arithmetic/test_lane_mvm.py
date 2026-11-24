"""Independent adversarial tests for the row scheduler's hardware contract."""
import _support
from pathlib import Path
import unittest
import re
from check_lane_mvm import check_lane_mvm
from check_register_ports import replay_register_ports
from single_line_config_audit import Effective, capacity_audit


def fixture(local=True):
    config={'--coeff-lane-mvm':'1','--coeff-lane-local':str(int(local)),
        '--coeff-lane-issue-width':'10','--coeff-lane-select-latency':'1',
        '--coeff-lane-route-latency':'1','--coeff-line-product-depth':'5',
        '--coeff-backward-feedback-tree':'1','--coeff-rf-read-words':'16',
        '--coeff-rf-write-words':'8','--coeff-cluster-mul-lat':'3',
        '--coeff-cluster-add-lat':'4','--coeff-cluster-div-lat':'12',
        '--coeff-cluster-div-ii':'4','--lusgs-cells':'2'}
    events=[];cluster=[];time=2;identity=0
    suffix='token=1 context=1 epoch=0 cell=0'
    for col in range(5):
        events.extend([(0,0,f'CFD_LANE_OPERAND cycle=0 {suffix} column={col}'),
            (0,0,f'CFD_BACK_OPERAND cycle=0 token=1 cell=0 event=use column={col}')])
    for row in range(5):
        values={};ends={}
        for node in range(10):
            identity+=1;mul=node<5;lat=3 if mul else 4
            left,right=((-1,-1) if mul else [(-1,0),(1,2),(3,4),(5,6),(8,7)][node-5])
            a=float(node+row+1) if mul else float(100+row) if left<0 else values[left]
            b=float(node+1) if mul else values[right]
            value=a*b if mul else a-b if node in (5,8,9) else a+b
            ready=0 if mul else max(ends[x]+1 for x in (left,right) if x>=0)
            pool='mul' if mul else 'add';caller=8 if mul else 7 if node==9 else 9
            lr=(1 if left<0 else 2) if local and not mul else 0
            local_result=int(local and node!=9)
            events.append((time,3,f'CFD_RF_ISSUE cycle={time} id={identity} pool={pool} lanes=1 caller={caller} read_words={2-lr} ready={time+lat} capacity={15 if mul else 20} local_reads={lr} local_result={local_result} row={row}'))
            events.append((time,4,f'CFD_LANE_MVM cycle={time} {suffix} event=issue node={node} row={row} id={identity} ready={ready} a={a.hex()} b={b.hex()}'))
            if local_result:events.append((time+lat,0,f'CFD_LANE_WRITE cycle={time+lat} id={identity} row={row}'))
            events.append((time+lat,1,f'CFD_RF_COMPLETE cycle={time+lat} id={identity} pool={pool} lanes=1 nominal={time+lat}'))
            events.append((time+lat,2,f'CFD_LANE_MVM cycle={time+lat} {suffix} event=complete node={node} row={row} value={value.hex()}'))
            cluster.append(dict(cycle=str(time),pool=pool,caller=str(caller),lanes='1',complete_cycle=str(time+lat)))
            values[node]=value;ends[node]=time+lat;time+=8
    events.append((time,9,'CFD_LINE_SCHEDULE token=1'))
    return '\n'.join(e[2] for e in sorted(events)),config,cluster


class LaneMvmTests(unittest.TestCase):
    def test_independent_rows_release_early_and_preserve_fp64_order(self):
        for local in (False,True):
            text,config,cluster=fixture(local)
            rf=replay_register_ports(text,config,cluster)
            result=check_lane_mvm(text,config,cluster,rf)
            self.assertEqual(result['scalar_operations'],50)
            self.assertGreater(result['rows_released_before_product_group'],0)
            self.assertEqual(rf.summary['write_words'],5 if local else 50)
            self.assertEqual(rf.summary['read_words'],55 if local else 100)
            self.assertEqual(rf.summary['local_write_words'],45 if local else 0)

    def test_reject_value_corruption_and_forwarding_shortcut(self):
        text,config,cluster=fixture()
        for altered in (text.replace('value=0x1.0000000000000p+0','value=0x1.8000000000000p+0',1),
                        text.replace('ready=0 a=', 'ready=1 a=',1),
                        text.replace('node=5 row=0 id=6 ready=6','node=5 row=0 id=6 ready=0')):
            self.assertNotEqual(altered,text)
            with self.assertRaises(AssertionError):check_lane_mvm(altered,config,cluster)
        with self.assertRaises(AssertionError):
            check_lane_mvm(text,dict(config,**{'--coeff-lane-select-latency':'16'}),cluster)

    def test_reject_missing_lane_duplicate_issue_and_unpaid_local_store(self):
        text,config,cluster=fixture()
        for altered in ('\n'.join(l for l in text.splitlines() if 'event=complete node=9 row=4' not in l),
                        text.replace('local_reads=2','local_reads=1',1),
                        text.replace('local_result=1','local_result=0',1)):
            with self.assertRaises(AssertionError):check_lane_mvm(altered,config,cluster)
        with self.assertRaises(AssertionError):
            check_lane_mvm(text,dict(config,**{'--coeff-lane-mvm':'0'}),cluster)

    def test_local_write_requires_its_own_port_and_ownership(self):
        text,config,cluster=fixture()
        for altered in (text.replace('CFD_LANE_WRITE cycle=5 id=1 row=0','CFD_LANE_WRITE cycle=4 id=1 row=0'),
                        text.replace('CFD_LANE_WRITE cycle=5 id=1 row=0','CFD_LANE_WRITE cycle=5 id=1 row=1'),
                        '\n'.join(l for l in text.splitlines() if 'CFD_LANE_WRITE cycle=5 ' not in l)):
            with self.assertRaises(AssertionError):replay_register_ports(altered,config,cluster)

    def test_selection_width_and_local_read_ports_are_real_limits(self):
        text,config,cluster=fixture()
        def move_issue(text,identity,old,new,latency):
            lines=[]
            for line in text.splitlines():
                if (line.startswith('CFD_RF_ISSUE ') or 'event=issue' in line) and f'id={identity} ' in line:
                    line=line.replace(f'cycle={old} ',f'cycle={new} ')
                    if line.startswith('CFD_RF_ISSUE '):
                        line=line.replace(f'ready={old+latency} ',f'ready={new+latency} ')
                lines.append(line)
            return '\n'.join(sorted(lines,key=lambda line:int(re.search(r'cycle=(\d+)',line)[1])
                if 'cycle=' in line else 10000))
        altered=move_issue(text,2,10,2,3)
        physical=[dict(r) for r in cluster];physical[1].update(cycle='2',complete_cycle='5')
        with self.assertRaisesRegex(AssertionError,'selection width'):
            check_lane_mvm(altered,dict(config,**{'--coeff-lane-issue-width':'1'}),physical)
        # Independent ADD nodes 5 and 6 could share the five ALUs, but their
        # one row would demand three local reads from a two-read bank.
        altered=move_issue(text,7,50,42,4)
        physical=[dict(r) for r in cluster];physical[6].update(cycle='42',complete_cycle='46')
        with self.assertRaisesRegex(AssertionError,'local row read ports'):
            replay_register_ports(altered,config,physical)

    def test_finite_graph_and_lookahead_are_added_to_existing_budget(self):
        from reporting.test_single_line_config_audit import fixture as capacity_fixture
        resolved,_=capacity_fixture(1024)
        resolved['options']['coeff_line_cache_allowed']=False
        base=capacity_audit(Effective(resolved))
        resolved['options'].update(coeff_lane_mvm=True,coeff_back_operand_depth=4)
        new=capacity_audit(Effective(resolved))
        self.assertEqual(new['reservation_bytes']-base['reservation_bytes'],4096+2*512)
        self.assertEqual(base['logical_tile_bytes'],new['logical_tile_bytes'])

if __name__=='__main__':unittest.main()
