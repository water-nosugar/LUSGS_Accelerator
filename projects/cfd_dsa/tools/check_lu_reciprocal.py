"""Verify diagonal reciprocal ownership, physical returns and upper scaling."""
from collections import Counter
import struct
from trace_text import fields


def bits(value):
    return int.from_bytes(struct.pack('>d', value), 'big')


def value(encoded):
    return struct.unpack('>d', int(encoded, 16).to_bytes(8, 'big'))[0]


def check_lu_reciprocal(text, config, trace, rf, divider):
    scales = fields(text, 'CFD_LU_SCALE')
    enabled = config.get('--coeff-lu-reciprocal-share', '0') == '1'
    if not enabled:
        assert not scales, 'LU scale emitted while reciprocal sharing is disabled'
        return {}
    assert rf is not None, 'LU reciprocal sharing requires physical RF proof'
    cold = {r['requestId'] for r in trace if r['stage'] == 'LU_COMPLETE'}
    produced = {int(r['accepted']): r for r in divider if r['event'] == 'result'}
    issued = {}; ready = {}; groups = {}; finished = Counter()
    for r in trace:
        key = r['requestId'], int(r['k'])
        if r['stage'] in ('LU_RECIP_DIV_ISSUE', 'LU_RECIP_READY', 'LU_UPPER_SCALE_ISSUE'):
            target = issued if r['stage'] == 'LU_RECIP_DIV_ISSUE' else ready if r['stage'] == 'LU_RECIP_READY' else groups
            assert key not in target, 'duplicate LU reciprocal/scale producer'
            target[key] = int(r['cycle'])
        if r['stage'] == 'RECIP_DIV_ISSUE' and r['requestId'] in cold:
            raise AssertionError('RHS recomputed a LU-owned reciprocal')
    expected = {(request, k) for request in cold for k in range(5)}
    assert set(issued) == set(ready) == expected, 'missing or extra LU reciprocal'
    assert set(groups) == {(request, k) for request in cold for k in range(4)}, 'missing upper scale group'
    reciprocal = {}; physical_used = Counter()
    for key, cycle in issued.items():
        assert cycle in produced, 'LU reciprocal has no physical divider result'
        r = produced[cycle]
        assert int(r['a'], 16) == bits(1.0), 'LU reciprocal numerator is not one'
        returned = int(r['cycle'])
        assert ready[key] == returned and rf.completions['div', '0', cycle, returned, 1] == 1, 'LU reciprocal published before actual RF return'
        physical_used['div','0',cycle,returned,1] += 1
        reciprocal[key] = value(r['result'])
    returned_groups = {}
    for r in scales:
        key = r['request'], int(r['k']); column = int(r['column'])
        assert key in groups and int(r['k']) < column < 5, 'orphan or invalid upper scale'
        assert ready[key] <= groups[key] <= int(r['cycle']), 'upper scale used an unready reciprocal'
        assert bits(float.fromhex(r['reciprocal'])) == bits(reciprocal[key]), 'upper scale used a different reciprocal'
        expected_value = float.fromhex(r['numerator']) * reciprocal[key]
        assert bits(float.fromhex(r['result'])) == bits(expected_value), 'incorrect separately rounded upper scaling'
        finished[key, column] += 1
        assert returned_groups.setdefault(key, int(r['cycle'])) == int(r['cycle']), 'upper scale rows did not share a physical return'
    assert finished == Counter({((request, k), j): 1 for request in cold for k in range(4) for j in range(k+1, 5)}), 'missing or duplicated upper quotient'
    for key, cycle in groups.items():
        width = 4-key[1]
        physical_used['mul','1',cycle,returned_groups[key],width] += 1
    assert all(n <= rf.completions[key] for key,n in physical_used.items()), 'reciprocal/upper scaling bypassed or reused physical arithmetic'
    return dict(cold_requests=len(cold), reciprocal_divides=len(issued),
                upper_multiply_groups=len(groups), upper_quotients=sum(finished.values()),
                eliminated_divisions=10*len(cold), additional_persistent_operand_slots=0)
