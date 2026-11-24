"""Cold upper transforms and cached single-RHS solves conserve different fanout."""
import _support
import unittest
from check_register_ports import check_preprocess_broadcast


def fixture(transformed=True, warm=True):
    config={'--coeff-preprocess-broadcast':'1','--coeff-rf-broadcast':'1',
            '--coeff-rf-read-words':'24','--coeff-rf-write-words':'12',
            '--coeff-rhs-fma-count':'10','--coeff-rhs-upper-inverse':'1'}
    lines=[];trace=[];identity=0
    for request,upper in [(11,transformed)]+([(12,False)] if warm else []):
        trace.extend([dict(requestId=str(request),stage='REQUEST_ALLOC',generation='9',cell='0'),
                      dict(requestId=str(request),stage='RHS_COMPLETE')])
        if upper:
            lines.append(f'CFD_RHS_UPPER event=allocate request={request}')
        for phase in ['forward']+(['upper'] if upper else []):
            for k in range(3):
                identity+=1;width=4-k;cycle=10*identity
                lines.append(f'CFD_RF_ISSUE cycle={cycle} id={identity} pool=fma caller=4 lanes={width} read_words={2*width+1} broadcast=1')
                lines.append(f'CFD_PRE_BROADCAST cycle={cycle} id={identity} request={request} generation=9 cell=0 family=rhs phase={phase} k={k} column=-1 rhs=0 lanes={width} factor=0x1p+0')
    return '\n'.join(lines),config,trace


class UpperBroadcastTests(unittest.TestCase):
    def test_mixed_cold_upper_and_cached_warm_topology(self):
        result=check_preprocess_broadcast(*fixture())
        self.assertEqual(result['groups'],9)
        self.assertEqual(result['rhs_saved_words'],18)

    def test_untransformed_rhs_keeps_six_saved_reads(self):
        result=check_preprocess_broadcast(*fixture(False,False))
        self.assertEqual(result['groups'],3)
        self.assertEqual(result['rhs_saved_words'],6)

    def test_upper_broadcast_without_transformed_owner_rejected(self):
        text,config,trace=fixture()
        text='\n'.join(line for line in text.splitlines()
                       if not line.startswith('CFD_RHS_UPPER '))
        with self.assertRaisesRegex(AssertionError,'group count'):
            check_preprocess_broadcast(text,config,trace)
