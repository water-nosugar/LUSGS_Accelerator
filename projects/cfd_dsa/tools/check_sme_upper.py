"""Replay private Ubar staging, SPM reads and reverse-panel consumption."""

from collections import Counter
import csv
from decimal import Decimal, localcontext
import math

from single_line_dataset import read_dataset, solve_decimal
from trace_text import iter_fields


def unique(rows, *keys):
    result = {}
    for row in rows:
        key = tuple(row[k] for k in keys)
        if key in result:
            raise ValueError(f'duplicate private upper event {key}')
        result[key] = row
    return result


def check_sme_upper(directory, text, cells, array, sweeps, aborts, input_file=None):
    configs = {r['token']: {k: int(v) for k, v in r.items() if k != 'token'}
               for r in array}
    successful = {str(s['token']): s for s in sweeps}
    ranges = unique(iter_fields(text, 'CFD_UBAR_OUTPUT_RANGE'), 'generation')
    if len(ranges) != len(configs) or any(int(r['bytes']) != cells * 200
            or not int(r['base'], 0) or int(r['base'], 0) % 8 for r in ranges.values()):
        raise ValueError('missing or invalid private Ubar destination ranges')
    with (directory / 'coeff_preprocess_trace.csv').open() as source:
        for row in csv.DictReader(source):
            if row['stage'] == 'DRAIN_UBAR_DONE':
                raise ValueError('private Ubar published an external drain')
            if row['stage'] != 'SPM_PACKET_ISSUE' or row['requester'] != 'CoeffDrain':
                continue
            generation = str(int(row['generation']) - int(row['cell']))
            region = ranges[(generation,)]
            address = int(row['address'], 0)
            base = int(region['base'], 0)
            if address < base + cells*200 and address + int(row['size']) > base:
                raise ValueError('DMA wrote the private Ubar destination')
    for row in iter_fields(text, 'CFD_TILE_DMA'):
        if row['region'] != '0':
            raise ValueError('private Ubar reloaded an external matrix tile')

    transfers = unique(iter_fields(text, 'CFD_SME_UPPER_MEMORY'), 'token', 'cell', 'event')
    uses = unique((r for r in iter_fields(text, 'CFD_BACK_OPERAND')
                   if r['event'] in ('read', 'use') and r['column'] != '-1'),
                  'token', 'cell', 'event', 'column')
    work_ids = {r['job'] for r in (*transfers.values(), *uses.values())}
    work_ids.update(r['rewrite'] for r in transfers.values() if r.get('rewrite', '0') != '0')
    jobs = unique((r for r in iter_fields(text, 'CFD_ONCHIP_START')
                   if r['id'] in work_ids), 'id')
    returns = unique((r for r in iter_fields(text, 'CFD_ONCHIP_COMPLETE')
                      if r['id'] in work_ids), 'id')
    if set(jobs) != set(returns) or {key[0] for key in jobs} != work_ids:
        raise ValueError('private Ubar has a missing SPM request or return')

    def check_job(job, token, address, words, stride, operation, first, last):
        issued, done = jobs[(job,)], returns[(job,)]
        if (issued['owner'], done['owner'], int(issued['base'], 0),
                int(issued['words']), int(issued['stride']), issued['op']) != (
                    token, token, address, words, stride, operation):
            raise ValueError('SPM job has incorrect private Ubar ownership or extent')
        start, end = int(issued['cycle']), int(done['cycle'])
        if not first <= start < end <= last:
            raise ValueError('private Ubar read before staging or incomplete SPM write')
        return end

    feedback = unique(iter_fields(text, 'CFD_LINE_FEEDBACK'), 'token', 'cell')
    counts = Counter()
    lead = {}
    for (token, cell_text, event), row in transfers.items():
        cfg = configs[token]
        cell = int(cell_text)
        end = int(row['cycle'])
        stop = int(aborts[token]['cycle']) if token in aborts else cfg['done']
        stride = cfg['upper_bytes'] // (cells - 1)
        address = cfg['upper_base'] + stride * cell
        if not 0 <= cell < cells - 1 or not cfg['start'] <= end <= stop:
            raise ValueError('private Ubar memory event outside its sweep')
        if int(row['words']) != stride // 8:
            raise ValueError('private Ubar memory event has the wrong word count')
        if event == 'store':
            check_job(row['job'], token, address, stride//8, 8, 'W', cfg['start'], end)
        elif event == 'finish':
            if not cfg['defer_upper'] or row['job'] != uses[(token, cell_text, 'read', '-2')]['job']:
                raise ValueError('reverse panel finished with the wrong staged input')
            start = int(transfers[(token, cell_text, 'store')]['cycle'])
            read_done = check_job(row['job'], token, address, stride//8, 8, 'R', start, end)
            rewrite = row['rewrite']
            if token in successful and bool(successful[token]['storage']['cache_bytes']) != (rewrite != '0'):
                raise ValueError('missing or unnecessary reverse Ubar cache update')
            if rewrite != '0':
                retire = cfg['done'] if 'upper_split' in cfg else end
                check_job(rewrite, token, address, 25, 8, 'W', read_done, retire)
                if int(jobs[(rewrite,)]['cycle']) > end:
                    raise ValueError('reverse Ubar exported before its cache write was admitted')
            if token in successful:
                entry = (int(feedback[(token, str(cell+1))]['finish']) if cell+1 < cells-1
                         else cfg['start'] + successful[token]['phase_cycles']['preprocess_and_forward'])
                lead.setdefault(token, []).append(entry - end)
        else:
            raise ValueError(f'unknown private upper event {event}')
        counts[token, event] += 1

    for (token, cell_text, event, column_text), row in uses.items():
        cfg = configs[token]
        cell, column = int(cell_text), int(column_text)
        if not 0 <= cell < cells-1:
            raise ValueError('boundary cell used a private upper panel')
        stop = int(aborts[token]['cycle']) if token in aborts else cfg['done']
        now = int(row['cycle'])
        if not cfg['start'] <= now <= stop:
            raise ValueError('private upper operand used outside its owner sweep')
        stride = cfg['upper_bytes'] // (cells - 1)
        address = cfg['upper_base'] + stride * cell
        if event == 'read':
            if (column == -2) != bool(cfg['defer_upper']) or column not in (-2, 0, 1, 2, 3, 4):
                raise ValueError('unexpected private upper read layout')
            stage = transfers.get((token, cell_text, 'store'))
            warm = (token in successful and successful[token]['cache_hit'])
            if not stage and token in successful and not warm:
                raise ValueError('cold private Ubar read lacks a staged matrix')
            first = int(stage['cycle']) if stage else cfg['start']
            if column == -2:
                check_job(row['job'], token, address, stride//8, 8, 'R', first, cfg['done'])
            else:
                check_job(row['job'], token, address+column*8, 5, 40, 'R', first, cfg['done'])
        else:
            if column not in range(5):
                raise ValueError('invalid private upper column use')
            read_col = '-2' if cfg['defer_upper'] else column_text
            read = uses[(token, cell_text, 'read', read_col)]
            available = int(returns[(read['job'],)]['cycle'])
            if cfg['defer_upper']:
                available = int(transfers[(token, cell_text, 'finish')]['cycle'])
            if row['job'] != read['job'] or now < available:
                raise ValueError('private upper column consumed before its result arrived')
        counts[token, event] += 1

    reports = {}
    for token, cfg in configs.items():
        stored, finished = counts[token, 'store'], counts[token, 'finish']
        if (stored, finished) != (cfg['upper_stored'], cfg['reverse_done']):
            raise ValueError('private upper event counts differ from the controller')
        if token in successful and (counts[token, 'use'] != (cells-1)*5 or
                counts[token, 'read'] != (cells-1)*(1 if cfg['defer_upper'] else 5)):
            raise ValueError('private upper reverse operands were skipped or duplicated')
        leads = lead.get(token, ())
        reports[token] = dict(stored=stored, finished=finished, read_jobs=counts[token, 'read'],
                              columns_used=counts[token, 'use'],
                              panels_ready_by_frontier=sum(x >= 0 for x in leads),
                              panel_head_wait_cycles=sum(max(0, -x) for x in leads))
    result = dict(external_bytes=0, sweeps=reports)
    if all('upper_split' in cfg for cfg in configs.values()):
        result['matrix_oracle'] = check_matrices(text, configs, transfers, input_file)
    return result


def check_matrices(text, configs, transfers, input_file):
    staged = {(token, cell) for token, cell, event in transfers if event == 'store'}
    finished = {(token, cell) for token, cell, event in transfers if event == 'finish' or
                (event == 'store' and not configs[token]['defer_upper'])}
    rows = unique(iter_fields(text, 'CFD_SME_UPPER_DATA'), 'token', 'cell', 'kind')
    expected = {(t, c, kind) for t, c in staged for kind in ('d', 'u')}
    expected.update((t, c, 'ubar') for t, c in finished)
    if set(rows) != expected:
        raise ValueError('private Ubar matrix snapshots do not match the staged panels')
    matrices = {key: [float.fromhex(x) for x in row['values'].split(',')]
                for key, row in rows.items()}
    if any(len(m) != 25 or not all(math.isfinite(x) for x in m) for m in matrices.values()):
        raise ValueError('private Ubar matrix contains non-finite or missing values')
    if input_file:
        records = read_dataset(input_file)
        for t, c in staged:
            raw = records[int(c)]
            if matrices[t, c, 'd'] != raw[:25] or matrices[t, c, 'u'] != raw[50:75]:
                raise ValueError('staged Ubar operands differ from the original dataset')
    max_abs = max_residual = Decimal(0)
    with localcontext() as context:
        context.prec = 80
        for t, c in finished:
            d, u, ubar = ([Decimal.from_float(x) for x in matrices[t, c, kind]]
                          for kind in ('d', 'u', 'ubar'))
            for col in range(5):
                ref = solve_decimal(d, u[col::5])
                for row in range(5):
                    error = abs(ubar[row*5+col] - ref[row])
                    if error > Decimal('1e-12') and error > abs(ref[row]) * Decimal('1e-12'):
                        raise ValueError('private Ubar differs from the independent Decimal solve')
                    residual = abs(sum(d[row*5+k] * ubar[k*5+col] for k in range(5)) -
                                   u[row*5+col])
                    if residual > Decimal('1e-9'):
                        raise ValueError('private Ubar exceeds the original residual tolerance')
                    max_abs = max(max_abs, error)
                    max_residual = max(max_residual, residual)
    return dict(matrices=len(finished), values=25*len(finished), precision=80,
                max_abs=float(max_abs), max_residual=float(max_residual))
