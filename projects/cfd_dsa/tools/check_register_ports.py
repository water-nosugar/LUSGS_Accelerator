"""Independent RF credit, bounded-result and actual-writeback trace replay."""
from collections import Counter, defaultdict
from dataclasses import dataclass
import math
import re
from trace_text import prefixed_lines


def rf_enabled(config):
    return any(int(config.get(key, '0')) for key in
               ('--coeff-rf-read-words', '--coeff-rf-write-words'))


def _rows(text):
    for match in re.finditer(r'^(?:CFD_RF_|CFD_LANE_WRITE|CFD_LOCAL_WRITE)[^\n]*',text,re.M):
        line=match[0]
        prefix = line.split(' ', 1)[0]
        if prefix in ('CFD_RF_ISSUE', 'CFD_RF_WRITE', 'CFD_RF_COMPLETE', 'CFD_RF_RELEASE', 'CFD_LANE_WRITE', 'CFD_LOCAL_WRITE'):
            yield prefix, dict(re.findall(r'(\w+)=([^\s]+)', line))


@dataclass
class RegisterReplay:
    summary: dict
    completions: Counter
    actual_cluster: list


def replay_register_ports(text, config, cluster):
    """Join every physical issue to a conserved, possibly delayed RF write.

    Cluster complete_cycle stays nominal. Returned actual_cluster is a copy
    suitable for residence metrics; completions[(pool,caller,issue,write,lanes)]
    independently anchors logical dependency-completion traces.
    """
    events = list(_rows(text))
    from check_rhs_pair import replay_rhs_pairs
    pairing=replay_rhs_pairs(text,config,events)
    from check_lu_rhs_pipeline import replay_rhs_factors, replay_lu_products
    factors=replay_rhs_factors(text,config,events)
    from check_rhs_factor_fanout import replay_rhs_factor_fanout
    factor_fanout=replay_rhs_factor_fanout(text,config,events,factors)
    from check_rhs_fma import replay_rhs_fma
    from check_rhs_upper import replay_rhs_upper
    rhs_upper=replay_rhs_upper(text,config,events)
    from check_rhs_upper_stream import replay_rhs_upper_stream
    rhs_stream=replay_rhs_upper_stream(text,config,events,rhs_upper)
    rhs_fma=replay_rhs_fma(text,config,events,rhs_upper,rhs_stream)
    from check_rhs_tree_rf_temp import replay_rhs_tree_rf_temp
    tree_rf_temp=replay_rhs_tree_rf_temp(text,config,events)
    from check_lu_upper import replay_lu_upper
    upper=replay_lu_upper(text,config,events)
    from check_lu_fma import replay_lu_fma
    lu_fma=replay_lu_fma(text,config,events)
    from check_lu_rank1 import replay_lu_rank1
    rank1=replay_lu_rank1(text,config,events)
    from check_lu_pivot_prefetch import replay_lu_pivot_prefetch
    pivot_prefetch=replay_lu_pivot_prefetch(text,config,events)
    lu_products=replay_lu_products(text,config,events,upper['operation_ids']|rank1['product_ids'])
    from check_lu_control import replay_lu_control
    lu_control=rank1['control_summary'] if config.get('--coeff-lu-rank1','0')=='1' else replay_lu_control(text,config,events)
    if not rf_enabled(config):
        assert not events, 'RF trace emitted while the timing model is disabled'
        return None
    reads = int(config.get('--coeff-rf-read-words', '0'))
    writes = int(config.get('--coeff-rf-write-words', '0'))
    split = config.get('--coeff-rf-split-writeback','0') == '1'
    lane_mode = config.get('--coeff-lane-mvm','0') == '1'
    local_mode = config.get('--coeff-lane-local','0') == '1'
    broadcast = config.get('--coeff-rf-broadcast','0') == '1'
    preprocessing = config.get('--coeff-preprocess-broadcast','0') == '1'
    assert reads == 0 or reads >= 10
    assert writes == 0 or writes >= (1 if split else 5)
    assert events and cluster, 'enabled RF model has no conserved arithmetic trace'
    latencies = {'mul': int(config['--coeff-cluster-mul-lat']),
        'add': int(config['--coeff-cluster-add-lat']),
        'div': int(config['--coeff-cluster-div-lat']),
        'sub': int(config.get('--forward-combine-latency', '1'))}
    ii = {'mul': 1, 'add': 1,
          'div': int(config['--coeff-cluster-div-ii']),
          'sub': int(config.get('--forward-combine-ii', '1'))}
    counts = {'mul': int(config.get('--coeff-cluster-mul-count', '5')),
              'add': int(config.get('--coeff-cluster-add-count','5')), 'div': int(config.get('--coeff-cluster-div-count', '1')),
              'sub': 5}
    fma_count=int(config.get('--coeff-rhs-fma-count','0'))
    if fma_count:
        latencies['fma']=5;ii['fma']=1;counts['fma']=fma_count
    lu_fma_count=int(config.get('--coeff-lu-fma-count','0'))
    if lu_fma_count:
        latencies['lu_fma']=5;ii['lu_fma']=1;counts['lu_fma']=lu_fma_count
    capacities = {pool: counts[pool]*math.ceil(latencies[pool]/ii[pool])
                  for pool in counts}
    divider_model = config.get('--coeff-divider-model', 'timing')
    if divider_model in ('pipelined', 'iterative'):
        core = int(config.get('--coeff-divider-inflight', '0')) or (
            1 if divider_model == 'iterative' else capacities['div'])
        capacities['div'] = core+int(config.get('--coeff-divider-result-depth', '2'))
    issue_counts = Counter(); completions = Counter()
    active = {}; issues = {}; complete = {}; live = Counter(); peak = Counter()
    released = set()
    read_usage = Counter(); write_usage = Counter()
    read_usage.update(pairing['loads'])
    assert not reads or all(n<=reads for n in read_usage.values()),'RF pair ingress read bandwidth oversubscribed'
    written = Counter(); last_write = {}
    local_read_usage = Counter(); local_write_usage = Counter()
    bank_reads = Counter(); bank_writes = Counter(); bank_words_read = bank_words_written = 0
    bank_live = bank_peak = 0
    bank_state = {}
    bank_enabled = config.get('--coeff-vector-local','0')=='1' or config.get('--coeff-local-products','0')=='1'
    product_only = config.get('--coeff-vector-product-only','0')=='1'
    assert not product_only or config.get('--coeff-vector-local','0')=='1', 'products-only MVM lacks local storage'
    bank_count = 27+int(config.get('--lu5-count','4'))+int(config.get('--coeff-rhs-product-depth','16'))
    if config.get('--coeff-lu-product-ahead','0')=='1':bank_count+=int(config.get('--lu5-count','4'))
    if config.get('--coeff-rhs-factor-reuse','0')=='1':bank_count+=int(config.get('--coeff-rhs-factor-slots','2'))
    if config.get('--coeff-lu-upper-overlap','0')=='1':bank_count+=2*int(config.get('--lu5-count','4'))
    for fill in factors['fills']:
        bank_writes[int(fill['cycle']),int(fill['bank'])]+=1
        bank_words_written+=int(fill['width'])
    route = int(config.get('--coeff-local-route-latency','1'))
    grouped = defaultdict(list)
    previous_cycle = -1
    for prefix, row in events:
        cycle = int(row['cycle'])
        assert cycle >= previous_cycle, 'RF events are not in cycle order'
        previous_cycle = cycle
        grouped[cycle].append((prefix, row))
    previous_cycle = None
    delayed = total_delay = max_delay = 0
    for cycle, rows in grouped.items():
        if previous_cycle is not None and cycle > previous_cycle+1:
            assert not any(op['pool'] != 'div' and max(int(op['ready']), last_write.get(op['id'],int(op['ready']))+int(op.get('bank_route','0'))) < cycle
                           for op in active.values()), 'ready RF result vanished through an idle trace gap'
        due = sorted((op for op in active.values() if op['pool'] != 'div' and op.get('local_result','0') != '1' and int(op.get('bank_dst','-1'))<0
                      and int(op['ready']) <= cycle),
                     key=lambda op: (int(op['ready']), int(op['id'])))
        written_before = {op['id']:written[op['id']] for op in due}
        actual_nondiv = []
        saw_issue = False
        divider_words = 0
        for prefix, row in rows:
            identity = row['id']
            if prefix == 'CFD_LOCAL_WRITE':
                assert not saw_issue and identity in active
                op=active[identity];bank=int(row['bank']);words=int(row['words'])
                assert bank_enabled and op.get('bank_access','0')=='1'
                assert bank==int(op['bank_dst']) and 0<=bank<bank_count
                assert row['owner']==op['bank_owner'] and row['epoch']==op['bank_epoch']
                assert cycle>=int(op['ready']) and words==int(op['lanes'])
                bank_writes[cycle,bank]+=1
                assert bank_writes[cycle,bank]<=1,'local vector write bank oversubscribed'
                assert not written[identity],'duplicated local vector result'
                written[identity]=words;last_write[identity]=cycle;bank_words_written+=words
                bank_state[bank]=(op['bank_owner'],op['bank_epoch'],cycle+int(op['bank_route']))
                continue
            if prefix == 'CFD_LANE_WRITE':
                assert not saw_issue and identity in active
                op=active[identity]
                assert local_mode and op.get('local_result','0')=='1'
                assert int(op['ready']) <= cycle and row['row']==op['row']
                local_write_usage[cycle,row['row']] += 1
                assert local_write_usage[cycle,row['row']] <= 1, 'local row write port oversubscribed'
                written[identity] += 1
                assert written[identity] == 1, 'local result duplicated'
                last_write[identity]=cycle
                continue
            pool = row['pool']; lanes = int(row['lanes'])
            assert pool in latencies and lanes > 0
            if prefix == 'CFD_RF_RELEASE':
                assert not saw_issue and config.get('--coeff-local-credit-release','0')=='1'
                assert identity in active and identity not in released, 'duplicate/unknown local FU release'
                op=active[identity]
                assert op['pool']==pool and op['lanes']==row['lanes'] and int(op.get('bank_dst','-1'))>=0
                assert written[identity]==lanes and last_write[identity]==cycle, 'FU released before local storage'
                released.add(identity);live[pool]-=lanes
                assert live[pool]>=0
                continue
            if prefix == 'CFD_RF_ISSUE':
                saw_issue = True
                assert identity not in issues, 'duplicate RF operation identity'
                shared_scalar = row.get('broadcast','0') == '1'
                assert not shared_scalar or (broadcast and ((pool=='lu_fma' and lu_fma_count and preprocessing and row['caller']=='15' and 2<=lanes<=4) or (pool=='fma' and fma_count and row['caller']=='4' and 2<=lanes<=4) or (pool == 'mul' and (
                    ((lanes == 5 or config.get('--coeff-arithmetic-mode','legacy') != 'legacy') and row['caller'] in ('6','8')) or
                    (preprocessing and 2 <= lanes <= 4 and row['caller'] in ('1','4')))))), 'invalid scalar broadcast source'
                local_reads=int(row.get('local_reads','0'))
                local_result=row.get('local_result','0')=='1'
                assert 0<=local_reads<=2
                if local_reads or local_result:
                    assert lane_mode and local_mode and lanes==1 and row['caller'] in ('7','8','9')
                    assert 0<=int(row['row'])<5
                    assert not shared_scalar
                local_read_usage[cycle,row.get('row','0')]+=local_reads
                assert local_read_usage[cycle,row.get('row','0')]<=2, 'local row read ports oversubscribed'
                demand = (2*lanes+1 if shared_scalar else 3*lanes) if pool in ('fma','lu_fma') else lanes+1 if shared_scalar else 2*lanes-local_reads
                if identity in pairing['children']:
                    assert not shared_scalar and not local_reads and not local_result
                    demand=0
                bank_access=row.get('bank_access','0')=='1'
                if product_only and row['caller'] in ('8','9'):
                    assert bank_access and lanes == 5, 'products-only MVM bypassed bounded operand placement'
                    a,b,dst=(int(row['bank_'+s]) for s in ('a','b','dst'))
                    if row['caller']=='8':
                        assert pool=='mul' and a==b==-1 and 0<=dst<27 and dst%9<5, 'MVM product bypassed local storage'
                    else:
                        assert pool=='add' and dst==-1, 'MVM tree partial paid the wrong destination'
                        assert all(x==-1 or (0<=x<27 and x%9<5) for x in (a,b)), 'products-only tree read a local partial'
                if product_only and row['caller']=='7' and bank_access:
                    assert pool=='add' and lanes==5
                    assert all(int(row['bank_'+s])==-1 for s in ('a','b','dst')), 'feedback final subtraction bypassed global partials'
                if bank_access:
                    assert bank_enabled and pool in ('mul','add','fma')
                    a,b,dst=(int(row['bank_'+s]) for s in ('a','b','dst'))
                    assert all(-1<=x<bank_count for x in (a,b,dst))
                    assert a<0 or a!=b,'same vector bank read twice'
                    local_words=0
                    for key in ('a','b'):
                        bank=int(row['bank_'+key]);words=int(row['bank_words_'+key])
                        assert (bank<0 and words==0) or (bank>=0 and 1<=words<=5)
                        if bank>=0:
                            if identity in factors['uses'] and bank==int(factors['uses'][identity]['bank']):
                                bound=factors['uses'][identity]
                                owner,epoch,available=bound['token'],bound['generation'],cycle
                            else:
                                assert bank in bank_state,'uninitialized local vector read'
                                owner,epoch,available=bank_state[bank]
                            assert (owner,epoch)==(row['bank_owner'],row['bank_epoch']) and available<=cycle,'local vector owner/route not ready'
                            bank_reads[cycle,bank]+=1
                            assert bank_reads[cycle,bank]<=1,'local vector read bank oversubscribed'
                            local_words+=words
                    demand-=local_words
                    assert demand>=0 and int(row['bank_route'])==(route if dst>=0 else 0)
                    bank_words_read+=local_words;bank_live+=1;bank_peak=max(bank_peak,bank_live)
                    assert bank_live<=32,'local vector operation tags unbounded'
                else:
                    assert all(int(row.get('bank_'+s,'-1'))==-1 for s in ('a','b','dst'))
                if identity in factor_fanout['uses']:
                    demand-=factor_fanout['uses'][identity]['width']
                    assert demand>=0
                assert int(row['read_words']) == demand, 'arithmetic read demand differs from operand organization'
                assert int(row['ready']) == cycle+latencies[pool], 'RF nominal latency changed'
                assert int(row['capacity']) == capacities[pool], 'RF finite capacity differs from FU pipelines'
                assert live[pool]+lanes <= capacities[pool], 'RF pending result storage is unbounded'
                read_usage[cycle] += demand
                assert not reads or read_usage[cycle] <= reads, 'RF read bandwidth oversubscribed'
                live[pool] += lanes; peak[pool] = max(peak[pool], live[pool])
                active[identity] = issues[identity] = row
                issue_counts[pool, row['caller'], cycle, lanes, int(row['ready'])] += 1
            elif prefix == 'CFD_RF_WRITE':
                assert split and pool != 'div' and not saw_issue
                assert identity in active and identity not in complete
                op = active[identity]
                assert row['pool'] == op['pool']
                assert int(row['nominal']) == int(op['ready']) <= cycle
                assert int(row['offset']) == written[identity], 'split RF lane offset duplicated or skipped'
                written[identity] += lanes
                assert written[identity] <= int(op['lanes']), 'split RF wrote too many lanes'
                last_write[identity] = cycle
                actual_nondiv.append((identity,lanes))
                write_usage[cycle] += lanes
                assert not writes or write_usage[cycle] <= writes, 'RF result write bandwidth oversubscribed'
            else:
                assert not saw_issue, 'writeback occurred after new issue in a controller cycle'
                assert identity in active and identity not in complete, 'unknown or duplicate RF completion'
                op = active.pop(identity)
                assert row['pool'] == op['pool'] and row['lanes'] == op['lanes']
                assert int(row['nominal']) == int(op['ready']) <= cycle, 'RF result returned before ready'
                if op.get('bank_access','0')=='1':
                    bank_live-=1
                if int(op.get('bank_dst','-1'))>=0:
                    assert written[identity]==lanes and cycle==last_write[identity]+int(op['bank_route']), 'local vector route/store bypassed'
                elif op.get('local_result','0')=='1':
                    assert written[identity]==1 and last_write[identity]==cycle, 'local result published before storage'
                elif pool == 'div':
                    # Divider credit is reserved before nondivider callbacks,
                    # but its callback/trace may occur later in the stable
                    # nominal-time order. Reconstruct the cycle's reservation
                    # from all its events, not from fprintf interleaving.
                    divider_words += lanes
                    write_usage[cycle] += lanes
                elif split:
                    assert written[identity] == lanes and last_write[identity] == cycle, 'RF published a partially written vector'
                else:
                    actual_nondiv.append((identity,lanes))
                    write_usage[cycle] += lanes
                assert not writes or write_usage[cycle] <= writes, 'RF result write bandwidth oversubscribed'
                assert (identity in released)==(config.get('--coeff-local-credit-release','0')=='1' and int(op.get('bank_dst','-1'))>=0), 'local FU credit release missing'
                if identity not in released: live[pool] -= lanes
                assert live[pool] >= 0
                complete[identity] = cycle
                completions[pool, op['caller'], int(op['cycle']), cycle, lanes] += 1
                delay = cycle-int(op['ready'])
                delayed += delay > 0; total_delay += delay; max_delay = max(max_delay, delay)
        # Replay the declared oldest-first, stop-on-block nondivider writeback
        # arbitration. Divider readiness is checked by its own finite replay.
        remaining = writes-divider_words if writes else math.inf
        expected = []
        for op in due:
            lanes = int(op['lanes'])-written_before[op['id']]
            if not split and lanes > remaining:
                break
            grant = min(lanes,remaining) if split else lanes
            if grant:
                expected.append((op['id'],grant)); remaining -= grant
            if grant < lanes:
                break
        assert actual_nondiv == expected, 'RF writeback skipped an older ready result or left usable credits idle'
        previous_cycle = cycle
    assert not active and not any(live.values()) and bank_live==0, 'RF operations/results did not drain'
    if bank_enabled and config.get('--coeff-resource-stats','0')=='1':
        from trace_text import fields
        summaries=fields(text,'CFD_LOCAL_VECTOR')
        assert summaries,'missing bounded local-storage summary'
        last=summaries[-1]
        expected_bytes=4608+(1536 if config.get('--coeff-vector-local','0')=='1' else 0)
        if config.get('--coeff-local-products','0')=='1': expected_bytes+=128*int(config.get('--lu5-count','4'))+256
        if config.get('--coeff-rhs-pair','0')=='1': expected_bytes+=768
        if config.get('--coeff-lu-product-ahead','0')=='1': expected_bytes+=128*int(config.get('--lu5-count','4'))+128
        if config.get('--coeff-rhs-factor-reuse','0')=='1': expected_bytes+=128*int(config.get('--coeff-rhs-factor-slots','2'))+256
        if config.get('--coeff-rhs-fma-factor-fanout','0')=='1': expected_bytes+=128
        if config.get('--coeff-rhs-factor-fanout','0')=='1': expected_bytes+=256
        if fma_count: expected_bytes+=192*5*fma_count+256
        if lu_fma_count: expected_bytes+=192*5*lu_fma_count+256
        if config.get('--coeff-rhs-frontier-reserve','0')=='1':expected_bytes+=256
        if config.get('--coeff-rhs-upper-inverse','0')=='1': expected_bytes+=256*int(config.get('--coeff-rhs-upper-slots','16'))+256
        if config.get('--coeff-rhs-upper-stream','0')=='1':expected_bytes+=512
        if config.get('--coeff-lu-pivot-prefetch','0')=='1':expected_bytes+=128
        if config.get('--coeff-input-r-before-u','0')=='1':expected_bytes+=128
        if int(config.get('--coeff-spm-slot-rotation','0')):expected_bytes+=128
        if config.get('--coeff-rhs-tree-rf-temp','0')=='1': expected_bytes+=16*int(config.get('--coeff-rhs-product-depth','60'))+128
        if config.get('--coeff-lu-control-fold','0')=='1': expected_bytes+=32*int(config.get('--lu5-count','4'))+128
        if config.get('--coeff-lu-upper-overlap','0')=='1': expected_bytes+=256*int(config.get('--lu5-count','4'))+128
        if config.get('--coeff-lu-rank1','0')=='1': expected_bytes+=512*int(config.get('--lu5-count','4'))+128
        assert int(last['banks'])==bank_count and int(last['tags'])==32 and last['live']=='0'
        assert int(last['peak'])==bank_peak and int(last['bytes'])==expected_bytes
        assert int(last['route'])==route and last['read_groups']==last['write_groups']=='1'
        assert int(last.get('factor_fanout','0'))==int(config.get('--coeff-rhs-factor-fanout','0'))
        assert int(last.get('factor_fanout_feedback','0'))==int(config.get('--coeff-rhs-factor-fanout-feedback','0'))
        assert int(last.get('factor_fanout_bytes','0'))==(256 if config.get('--coeff-rhs-factor-fanout','0')=='1' else 0)
        assert int(last.get('tree_rf_temp','0'))==int(config.get('--coeff-rhs-tree-rf-temp','0'))
        assert int(last.get('tree_rf_first_column','0'))==int(config.get('--coeff-rhs-tree-rf-first-column','0'))
        assert int(last.get('tree_rf_temp_bytes','0'))==(16*int(config.get('--coeff-rhs-product-depth','60'))+128 if config.get('--coeff-rhs-tree-rf-temp','0')=='1' else 0)
    physical = Counter((row['pool'], row['caller'], int(row['cycle']),
        int(row['lanes']), int(row['complete_cycle'])) for row in cluster)
    assert physical == issue_counts, 'RF issues do not exactly match the shared arithmetic issue trace'
    by_signature = defaultdict(list)
    for identity, op in issues.items():
        by_signature[op['pool'], op['caller'], int(op['cycle']), int(op['lanes'])].append((int(identity), complete[identity]))
    for rows in by_signature.values():
        rows.sort()
    used = Counter(); actual_cluster = []
    for row in cluster:
        key = row['pool'], row['caller'], int(row['cycle']), int(row['lanes'])
        _, actual = by_signature[key][used[key]]; used[key] += 1
        actual_cluster.append(dict(row, complete_cycle=str(actual)))
    return RegisterReplay(dict(operations=len(issues), read_words=sum(read_usage.values()),
        write_words=sum(write_usage.values()), local_read_words=sum(local_read_usage.values()),
        local_write_words=sum(local_write_usage.values()), read_words_per_cycle=reads,
        write_words_per_cycle=writes, live_peak=dict(peak), capacities=capacities,
        delayed_operations=delayed, total_writeback_delay=total_delay,
        max_writeback_delay=max_delay, bank_read_words=bank_words_read,
        bank_write_words=bank_words_written, bank_tag_peak=bank_peak,
        bank_count=bank_count if bank_enabled else 0,rhs_pair=pairing['summary'],
        lu_fma=lu_fma,lu_pivot_prefetch=pivot_prefetch,rhs_upper=rhs_upper['summary'],rhs_upper_stream=rhs_stream['summary'],rhs_fma=rhs_fma,rhs_factors=factors['summary'],rhs_factor_fanout=factor_fanout['summary'],rhs_tree_rf_temp=tree_rf_temp,lu_products=lu_products,lu_control=lu_control,
        lu_upper_overlap=upper['summary'],lu_rank1=rank1['summary']), completions, actual_cluster)


def case_residence_cycles(path, config, text=None):
    """Replay physical returns before using occupancy statistics as evidence."""
    import csv
    from pathlib import Path
    path = Path(path)
    if not rf_enabled(config):
        return None
    if text is None:
        text = (path/'simout.txt').read_text()
    with (path/'coeff_preprocess_trace.csv.cluster.csv').open(newline='') as source:
        replay = replay_register_ports(text, config, list(csv.DictReader(source)))
    residence = Counter()
    for row in replay.actual_cluster:
        residence['shared_'+row['pool']] += int(row['lanes'])*(int(row['complete_cycle'])-int(row['cycle']))
    return residence


def check_preprocess_broadcast(text, config, trace):
    """Bind every saved RF read to a permitted LU/forward-RHS fanout group."""
    from trace_text import fields
    messages=fields(text,'CFD_PRE_BROADCAST')
    from check_rhs_pair import replay_rhs_pairs,check_pair_schedule
    pairing=replay_rhs_pairs(text,config,list(_rows(text)))
    check_pair_schedule(pairing,trace)
    enabled=config.get('--coeff-preprocess-broadcast','0')=='1'
    if not enabled:
        assert not messages, 'disabled preprocessing fanout emitted transfers'
        return {}
    assert config.get('--coeff-rf-broadcast','0')=='1' and rf_enabled(config)
    physical={row['id']:row for prefix,row in _rows(text)
        if prefix=='CFD_RF_ISSUE' and (row.get('broadcast','0')=='1' or int(row.get('pair_id','0'))) and row['caller'] in ('1','4','15')}
    lu_events=Counter((r['requestId'],r['cycle'],r['k'],r['row']) for r in trace if r['stage'] in ('LU_ROW_MUL_ISSUE','LU_ROW_FMA_ISSUE'))
    owners={r['requestId']:(r['generation'],r['cell']) for r in trace if r['stage']=='REQUEST_ALLOC'}
    lu_complete=Counter(r['requestId'] for r in trace if r['stage']=='LU_COMPLETE')
    rhs_complete=Counter(r['requestId'] for r in trace if r['stage']=='RHS_COMPLETE')
    seen=set();groups=Counter();savings=Counter();lu_needed=Counter();rhs_nodes=set()
    for row in messages:
        identity=row['id'];assert identity in physical and identity not in seen
        seen.add(identity);op=physical[identity]
        width=int(row['lanes']);k=int(row['k']);column=int(row['column'])
        assert row['cycle']==op['cycle'] and width==int(op['lanes']) and 2<=width<=4
        assert op['pool'] in ('mul','fma','lu_fma')
        fma=op['pool'] in ('fma','lu_fma')
        if op['pool']=='fma':
            assert int(config.get('--coeff-rhs-fma-count','0'))>0 and row['family']=='rhs'
        if op['pool']=='lu_fma':
            assert int(config.get('--coeff-lu-fma-count','0'))>0 and row['family']=='lu'
        if identity in pairing['children']:
            bound=pairing['children'][identity]
            assert row['factor']==bound['load']['factor'+str(bound['side'])]
            assert row['rhs']==bound['parent']['rhs'+str(bound['side'])]
            assert row['k']==bound['parent']['k']
            assert op['read_words']=='0'
        else:
            retained=(int(op.get('bank_words_a','0'))+int(op.get('factor_words','0'))) if int(op.get('factor_id','0')) else 0
            assert int(op['read_words'])==(2*width+1 if fma else width+1)-retained
        assert owners[row['request']]==(row['generation'],row['cell'])
        assert math.isfinite(float.fromhex(row['factor']))
        if row['family']=='lu':
            assert row['rhs']=='-1'
            if config.get('--coeff-lu-rank1','0')=='1':
                assert row['phase']==('rank1-fma' if int(config.get('--coeff-lu-fma-count','0')) else 'rank1') and 0<=k<column<5 and width==4-k
            else:
                assert row['phase'] in ('lower','upper')
                assert 0<=k<column<5 and width==5-column-int(row['phase']=='upper')
            assert op['caller']==('15' if op['pool']=='lu_fma' else '1')
            lu_needed[row['request'],row['cycle'],row['k'],row['column']]+=1
        else:
            assert row['family']=='rhs' and row['phase'] in ('forward','upper') and column==-1
            assert row['phase']=='forward' or (fma and config.get('--coeff-rhs-upper-inverse','0')=='1')
            assert 0<=int(row['rhs'])<15 and 0<=k<=2 and width==4-k and op['caller']=='4'
            node=row['request'],row['rhs'],k,row['phase']
            assert node not in rhs_nodes,'duplicated RHS forward fanout'
            rhs_nodes.add(node)
        groups[row['family'],row['request']]+=1
        savings[row['family']]+=width-1
    assert seen==set(physical),'broadcast arithmetic has no permitted common-factor producer'
    assert all(lu_events[key]>=amount for key,amount in lu_needed.items()), 'LU fanout bypassed row issue'
    expected=Counter({('lu',owner):9*count for owner,count in lu_complete.items()})
    upper_owners={r['request'] for r in fields(text,'CFD_RHS_UPPER') if r['event']=='allocate'}
    expected.update({('rhs',owner):(6 if owner in upper_owners else 3)*count for owner,count in rhs_complete.items()})
    assert groups==expected, 'LU/RHS common-factor group count changed'
    assert savings['lu']==(20 if config.get('--coeff-lu-rank1','0')=='1' else 14)*sum(lu_complete.values())
    expected_rhs_saved=sum((12 if owner in upper_owners else 6)*count
                           for owner,count in rhs_complete.items())
    assert savings['rhs']==expected_rhs_saved, 'cold/warm RHS broadcast read savings differ from owned topology'
    return dict(groups=len(messages),saved_global_read_words=sum(savings.values()),
        lu_saved_words=savings['lu'],rhs_saved_words=savings['rhs'],
        additional_arithmetic_operations=0,additional_persistent_operand_slots=0,
        timing_assumption='existing shared MUL fanout/input selection; no extra latency parameter or IP calibration')
