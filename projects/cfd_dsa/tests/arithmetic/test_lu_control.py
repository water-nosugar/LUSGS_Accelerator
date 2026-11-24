"""Owned physical returns, bounded LU control chains, cancellation and budget."""
import _support
import unittest
from check_lu_control import replay_lu_control
from check_register_ports import _rows


def fixture(token=7,epoch=9,offset=0):
    lines=[];parent=0;returned=0
    def fold(cycle,c,phase,target,index):
        nxt=c+1 if phase==10 else c
        lines.append((cycle,f'CFD_LU_CONTROL cycle={cycle} event=fold token={token} generation={epoch} request=11 cell=0 column={c} next_column={nxt} from={phase} to={target} index={index} parent={parent} returned={returned}'))
    def result(cycle,c,upper,identity):
        nonlocal parent,returned
        parent=identity+offset;returned=cycle+(3 if upper else 4)
        pool,caller=('mul',1) if upper else ('add',2)
        lines.append((cycle,f'CFD_RF_ISSUE cycle={cycle} id={parent} pool={pool} lanes={5-c-int(upper)} caller={caller} bank_dst=-1 bank_owner={token} bank_epoch={epoch}'))
        lines.append((cycle,f'CFD_RF_OWNER cycle={cycle} id={parent} owner={token} frontier=0'))
        lines.append((returned,f'CFD_RF_COMPLETE cycle={returned} id={parent} pool={pool} lanes={5-c-int(upper)}'))
    fold(10,0,0,3,1);fold(10,0,3,4,2);fold(11,0,5,8,1)
    for c in range(4):
        result(12+40*c,c,True,2*c+1)
        fold(returned,c,9,10,1);fold(returned,c,10,0,2);fold(returned,c+1,0,1,3)
        result(returned+6,c+1,False,2*c+2)
        fold(returned,c+1,3,4,1)
        if c<3:fold(returned+1,c+1,5,6,1)
        else:fold(returned+1,4,10,11,1)
    lines.sort(key=lambda row:row[0])
    return '\n'.join(line for _,line in lines),{'--coeff-lu-control-fold':'1','--lu5-count':'4'}


def replay(text,cfg):
    return replay_lu_control(text,cfg,list(_rows(text)))


class LuControlTests(unittest.TestCase):
    def test_complete_path_commits_only_actual_owned_returns(self):
        text,cfg=fixture();result=replay(text,cfg)
        self.assertEqual(result['folds'],23)
        self.assertEqual(result['actual_return_commits'],8)
        self.assertEqual(result['completed_contexts'],1)
        self.assertEqual(result['context_peak'],1)
        self.assertEqual(result['extra_reservation_bytes'],256)

    def test_reject_unpaid_early_wrong_owner_and_wrong_width_commits(self):
        text,cfg=fixture()
        for before,after in [('parent=1 returned=15','parent=99 returned=15'),
            ('returned=15','returned=14'),('CFD_RF_COMPLETE cycle=15','CFD_RF_COMPLETE cycle=16'),
            ('owner=7','owner=8'),('lanes=4 caller=1','lanes=3 caller=1')]:
            with self.subTest(before=before),self.assertRaises((AssertionError,KeyError)):
                replay(text.replace(before,after),cfg)
        missing='\n'.join(line for line in text.splitlines() if 'CFD_RF_OWNER' not in line)
        with self.assertRaisesRegex(AssertionError,'owned RF'):replay(missing,cfg)

    def test_reject_arithmetic_folding_repeated_visits_and_missing_drain(self):
        text,cfg=fixture()
        for before,after in [('from=5 to=8','from=8 to=9'),('index=3','index=4'),
            ('column=0 next_column=1 from=10','column=0 next_column=0 from=10')]:
            with self.subTest(before=before),self.assertRaises(AssertionError):
                replay(text.replace(before,after),cfg)
        missing='\n'.join(line for line in text.splitlines() if 'from=10 to=11' not in line)
        with self.assertRaisesRegex(AssertionError,'drain'):replay(missing,cfg)

    def test_context_capacity_is_shared_across_cells(self):
        a,cfg=fixture();b,_=fixture(8,10,100)
        lines=(a+'\n'+b).splitlines()
        lines.sort(key=lambda row:int(row.split('cycle=',1)[1].split()[0]))
        with self.assertRaisesRegex(AssertionError,'capacity'):
            replay('\n'.join(lines),dict(cfg,**{'--lu5-count':'1'}))

    def test_cancel_releases_owner_and_rejects_later_folds(self):
        text,cfg=fixture()
        prefix='\n'.join(line for line in text.splitlines() if 'cycle=10 ' in line)
        cancelled=prefix+'\nCFD_LU_CONTROL cycle=11 event=cancel token=7 generation=9 request=11 cell=0'
        self.assertEqual(replay(cancelled,cfg)['cancel_events'],1)
        later=next(line for line in text.splitlines() if 'CFD_LU_CONTROL cycle=11 ' in line)
        with self.assertRaisesRegex(AssertionError,'cancellation'):replay(cancelled+'\n'+later,cfg)

    def test_disabled_path_has_no_folding_evidence(self):
        text,_=fixture()
        self.assertEqual(replay('',{}),{})
        with self.assertRaisesRegex(AssertionError,'disabled'):replay(text,{})

    def test_no_lu_work_needs_no_fold_but_cannot_hide_lu_arithmetic(self):
        cfg={'--coeff-lu-control-fold':'1'}
        self.assertEqual(replay('',cfg)['context_peak'],0)
        text,_=fixture()
        hidden='\n'.join(line for line in text.splitlines() if 'CFD_LU_CONTROL ' not in line)
        with self.assertRaisesRegex(AssertionError,'lacks control-fold evidence'):replay(hidden,cfg)

    def test_independent_capacity_ledgers_charge_control_only(self):
        from reporting.test_single_line_config_audit import fixture as config_fixture
        from single_line_config_audit import Effective,capacity_audit
        from check_single_line_storage import lifecycle_reserve
        resolved,_=config_fixture(1024);resolved['options']['coeff_line_cache_allowed']=False
        base=capacity_audit(Effective(resolved))['reservation_bytes']
        resolved['options'].update(coeff_lu_control_fold=True,lu5_count=4)
        self.assertEqual(capacity_audit(Effective(resolved))['reservation_bytes']-base,256)
        cfg={'--coeff-spm-outstanding':'128','--coeff-line-opt-level':'0','--lu5-count':'4'}
        self.assertEqual(lifecycle_reserve(dict(cfg,**{'--coeff-lu-control-fold':'1'}))-lifecycle_reserve(cfg),256)
