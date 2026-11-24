"""Independent replay of one same-cycle LU-factor bus and its two receivers."""
from collections import Counter
import re


def _fields(line):
    return dict(re.findall(r'(\w+)=([^\s]+)', line))


def replay_rhs_factor_fanout(text, config, operations, factors):
    enabled = config.get('--coeff-rhs-factor-fanout', '0') == '1'
    feedback_only = config.get('--coeff-rhs-factor-fanout-feedback', '0') == '1'
    fma_enabled=config.get('--coeff-rhs-fma-factor-fanout','0')=='1'
    caps=re.findall(r'^CFD_RHS_FMA_FACTOR_FANOUT_CAPACITY ([^\n]*)',text,re.M)
    if fma_enabled:
        assert enabled and int(config.get('--coeff-rhs-fma-count','0'))>0 and config.get('--coeff-rhs-upper-inverse','0')=='1'
        assert caps
        for cap in caps:
            r=_fields(cap);assert tuple(int(r[k]) for k in ['control_bytes','bus_bytes','receivers','vectors_per_cycle','extra_fma','extra_ports'])==(128,256,2,1,0,0)
    else:assert not caps
    assert enabled or not feedback_only, 'feedback factor bus policy requires an enabled bus'
    physical = {row['id']: row for prefix, row in operations
                if prefix == 'CFD_RF_ISSUE' and (
                    int(row.get('factor_source', '0')) or int(row.get('factor_words', '0')))}
    messages = list(re.finditer(r'^CFD_RHS_FACTOR_FANOUT [^\n]*', text, re.M))
    if not enabled:
        assert not physical and not messages, 'disabled factor bus emitted reuse'
        return dict(uses={}, summary={})
    assert config.get('--coeff-rhs-factor-reuse', '0') == '1'
    assert config.get('--coeff-local-products', '0') == '1'
    assert config.get('--coeff-rhs-pair', '0') == '0'
    assert int(config.get('--coeff-rf-read-words', '0')) > 0
    assert int(config.get('--coeff-rf-write-words', '0')) > 0
    wanted = set(physical)
    for match in messages:
        row = _fields(match[0])
        wanted.update((row['source'], row['id']))
    issued = {row['id']: row for prefix,row in operations
              if prefix == 'CFD_RF_ISSUE' and row['id'] in wanted}
    positions = {}
    for match in re.finditer(r'^CFD_RF_ISSUE [^\n]*', text, re.M):
        row = _fields(match[0])
        if row['id'] in wanted:
            assert row['id'] not in positions
            positions[row['id']] = match.start()
    # Match offer payloads to the actual registered cache use, independently
    # of the new bus's own self-reported fields.
    source_uses = {}
    source_positions = {}
    for match in re.finditer(r'^CFD_RHS_FACTOR [^\n]*', text, re.M):
        row = _fields(match[0])
        if row['event'] == 'use' and row['id'] in wanted:
            assert row['id'] not in source_uses
            source_uses[row['id']] = row
            source_positions[row['id']] = match.start()
    key_fields = ('token', 'generation', 'cache', 'slot', 'bank', 'k', 'backward', 'width')
    payload_fields = tuple('v'+str(i) for i in range(5))
    uses = {}
    summary = Counter()
    bus = None
    current = -1
    for match in re.finditer(r'^CFD_RHS_FACTOR(?:_FANOUT)? [^\n]*', text, re.M):
        line = match[0]
        row = _fields(line)
        cycle = int(row['cycle'])
        assert cycle >= current, 'factor bus trace moved backwards'
        if cycle > current:
            current = cycle
            bus = None
        if line.startswith('CFD_RHS_FACTOR '):
            if row['event'] == 'clear' and bus is not None:
                source = bus['row']
                if all(source[k] == row[k] for k in ('token','generation','cache','slot','bank')):
                    bus['valid'] = False
                    summary['invalidations'] += 1
            continue
        assert 0 <= int(row['k']) < 4 and int(row['width']) == 4-int(row['k'])
        assert row['backward'] in ('0', '1') and 0 <= int(row['rhs']) < 15
        assert int(row['bank']) == factors['base']+int(row['slot'])
        assert 0 <= int(row['slot']) < int(config.get('--coeff-rhs-factor-slots','2'))
        assert row['source'] in issued
        source_op = issued[row['source']]
        if row['event'] == 'offer':
            assert bus is None, 'factor bus offered more than one vector/cycle'
            assert row['id'] == row['source'] and row['source'] in factors['uses']
            assert row['source'] in source_uses, 'factor bus source lacks a paid cache capture'
            original = source_uses[row['source']]
            assert original['cycle'] == row['cycle'] == source_op['cycle']
            assert all(original[k] == row[k] for k in key_fields+payload_fields)
            assert original['rhs'] == row['rhs'] and original['request'] == row['request']
            assert original['cell'] == row['cell']
            assert source_op['caller'] == '4' and (source_op['pool']=='mul' or (fma_enabled and source_op['pool']=='fma'))
            width=int(row['width'])
            expected=((2*width+1 if source_op.get('broadcast','0')=='1' else 3*width)
                      if source_op['pool']=='fma' else
                      (width+1 if source_op.get('broadcast','0')=='1' else 2*width))-width
            assert int(source_op['read_words'])==expected, 'factor source removed another operand read'
            assert source_op['factor_id'] == row['cache']
            assert source_op['bank_a'] == row['bank']
            assert int(source_op['bank_words_a']) == int(row['width'])
            assert not int(source_op.get('factor_source','0'))
            assert not int(source_op.get('factor_words','0'))
            assert positions[row['source']] < source_positions[row['source']] < match.start()
            bus = dict(row=row, position=match.start(), valid=True, receivers=1)
            summary['offers'] += 1
            continue
        assert row['event'] == 'use' and bus is not None and bus['valid'], 'stale factor bus use'
        assert not feedback_only or int(row['rhs']) < 10, 'feedback factor bus admitted Ubar'
        assert bus['receivers'] == 1, 'factor bus exceeded two consumers'
        source = bus['row']
        assert row['source'] == source['source'] and row['rhs'] != source['rhs']
        assert row['request'] == source['request'] and row['cell'] == source['cell']
        assert all(source[k] == row[k] for k in key_fields+payload_fields)
        child = issued[row['id']]
        assert row['id'] in physical and row['id'] not in uses
        assert int(row['id']) > int(row['source'])
        assert positions[row['source']] < bus['position'] < positions[row['id']] < match.start()
        assert child['cycle'] == row['cycle'] and child['pool']==source_op['pool'] and child['caller']=='4'
        assert child['pool']=='mul' or (fma_enabled and child['pool']=='fma' and int(child['bank_dst'])==-1)
        width = int(row['width'])
        assert int(child['lanes']) == width and child.get('bank_access') == '1'
        assert child['bank_owner'] == row['token'] and child['bank_epoch'] == row['generation']
        assert child.get('factor_id') == row['cache'] and child.get('factor_source') == row['source']
        assert int(child.get('factor_words','0')) == width
        assert child['bank_a'] == child['bank_b'] == '-1'
        assert int(child['bank_words_a']) == int(child['bank_words_b']) == 0
        assert not int(child.get('pair_id','0'))
        expected = ((2*width+1 if child.get('broadcast','0')=='1' else 3*width) if child['pool']=='fma' else (width+1 if child.get('broadcast','0')=='1' else 2*width))-width
        assert int(child['read_words']) == expected, 'factor bus removed another operand read'
        bus['receivers'] += 1
        uses[row['id']] = dict(source=row['source'], width=width,
                              token=row['token'], generation=row['generation'])
        summary['uses'] += 1
        if child['pool']=='fma':summary['fma_uses']+=1
        summary['saved_read_words'] += width
    assert set(physical) == set(uses), 'unbound factor-bus RF operand'
    return dict(uses=uses, summary=dict(summary))
