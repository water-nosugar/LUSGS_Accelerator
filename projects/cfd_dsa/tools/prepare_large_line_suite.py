#!/usr/bin/env python3
"""Prepare real-coefficient projections and explicitly synthetic scale tests."""
import argparse
import json
from pathlib import Path

from extract_gt01r import (SOURCE_URL, block_path, graph, read_archive,
                          records_for_path, write_json)
from prepare_gt01r_suite import baseline_records
from single_line_dataset import oracle, sha256, unpivoted_diagnostics, write_dataset

REPO = Path(__file__).resolve().parents[3]


def natural_band_records(blocks, rhs, cells):
    """Retain original diagonal/adjacent blocks; absent blocks remain zero.

    This is an original-order block-tridiagonal projection, not a geometric
    grid line. Nonadjacent couplings, even within the selected rows, are omitted.
    """
    if len(rhs) % 5 or not 1 <= cells <= len(rhs)//5:
        raise ValueError('natural-band length exceeds the original node count')
    zero = [0.0]*25
    raw = [list(blocks[i, i]) +
           list(blocks.get((i, i-1), zero) if i else zero) +
           list(blocks.get((i, i+1), zero) if i+1 < cells else zero) +
           list(rhs[5*i:5*(i+1)]) for i in range(cells)]
    return raw


def store(directory, name, raw, metadata):
    expected = oracle(raw, 200)
    higher = oracle(raw, 240)
    agreement = max(abs(a-b)/max(abs(a), abs(b), 1e-14)
                    for row, other in zip(expected, higher) for a, b in zip(row, other))
    if agreement > 1e-30:
        raise ValueError('200/240-digit oracle cross-check failed')
    result = write_dataset(directory/(name+'.cfd5'), raw, expected, precision=200)
    result.update(name=name, max_abs_oracle_output=max(abs(v) for row in expected for v in row),
                  oracle_scaled_difference_200_240=agreement,
                  **unpivoted_diagnostics(raw), **metadata)
    write_json(directory/(name+'.json'), result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archive', type=Path, required=True)
    parser.add_argument('--dataset-dir', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--campaign', default='single-line-scale-20261022')
    args = parser.parse_args()
    directory = args.dataset_dir.resolve()
    directory.mkdir(parents=True, exist_ok=True)
    blocks, rhs, hashes = read_archive(args.archive)
    provenance = dict(source=SOURCE_URL, archive_sha256=sha256(args.archive),
                      member_sha256=hashes, original_mesh_nodes=len(rhs)//5,
                      scaling='none', diagonal_shift=0, rhs='original sensitivity RHS')
    cases = []
    rejected = []
    for cells in (256, 512, 1024, 2048, 4096):
        cases.append(store(directory, f'synth-n{cells}', baseline_records(cells), dict(
            source='original guest init_problem baseline, synthetic scale control',
            projection='synthetic', derivation='synthetic coefficients; not an official CFD mesh')))
    neighbors, norms = graph(blocks, len(rhs)//5)
    for cells in (256, 512):
        name = f'gt01r-path-s1595-n{cells}'
        nodes = block_path(1595, cells, neighbors, norms)
        try:
            cases.append(store(directory, name, records_for_path(nodes, blocks, rhs), dict(
                **provenance, projection='connected-path', node_ids_zero_based=nodes,
                derivation='original-index-nearest connected block path; nonpath couplings omitted')))
        except ValueError as exc:
            rejected.append(dict(name=name, reason=str(exc)))
    for cells in (512, 1024, 1596):
        raw = natural_band_records(blocks, rhs, cells)
        retained = {(i, j) for i in range(cells) for j in range(max(0, i-1), min(cells, i+2))}
        omitted = sum(i < cells and (i, j) not in retained and any(v)
                      for (i, j), v in blocks.items())
        cases.append(store(directory, f'gt01r-band-n{cells}', raw, dict(
            **provenance, projection='natural-band', node_ids_zero_based=list(range(cells)),
            derivation='original-order block-tridiagonal projection; absent adjacent blocks are zero; not a geometric grid line',
            absent_upper_neighbor_blocks=sum(not any(r[50:75]) for r in raw[:-1]),
            absent_lower_neighbor_blocks=sum(not any(r[25:50]) for r in raw[1:]),
            omitted_block_entries_in_selected_rows=omitted)))
    manifest = dict(cases=cases, rejected=rejected,
                    notes='Real projections and synthetic memory/size stress are explicitly distinguished.')
    write_json(directory/'manifest.json', manifest)
    reference = json.loads((REPO/'projects/cfd_dsa/suites/single-line-gt01r-recommended.json').read_text())
    exclude = ('--bench-iters=', '--coefficient-update-interval=', '--max-ticks=', '--dram-size=')
    common = [a for a in reference['common_args'] if not a.startswith(exclude)]
    common += ['--bench-iters=1', '--coefficient-update-interval=1',
               '--max-ticks=1000000000000', '--dram-size=128MiB']
    run_cases = [dict(name=r['name'], target='lusgs-single-line', args=[
        f'--lusgs-cells={r["cells"]}', f'--coeff-preprocess-trace-cells={r["cells"]}',
        f'--coeff-input-file={directory/(r["name"]+".cfd5")}']) for r in cases]
    run_cases.sort(key=lambda r: (next(c['cells'] for c in cases if c['name']==r['name']), r['name']))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    write_json(args.output, dict(campaign=args.campaign, common_args=common, cases=run_cases,
        notes=['One cold forward/backward SGS call per case; not nonlinear convergence.',
               'SPM192KiB, four banks, same arithmetic resources; no coefficient reuse cache for these one-call cases.',
               '128MiB external SimpleMemory, 30ns and32GB/s; CPU caches and coherent DMA remain enabled.',
               'Natural-band cases preserve original coefficients/RHS but omit nonadjacent couplings; no geometry claimed.',
               '2048/4096-cell cases are explicitly synthetic; GT01R only has1596 original nodes.']))
    print(f'Prepared {len(run_cases)} cases; {len(rejected)} conversion rejections', flush=True)


if __name__ == '__main__':
    main()
