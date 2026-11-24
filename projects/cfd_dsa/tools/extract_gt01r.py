#!/usr/bin/env python3
"""Derive reproducible single-line SGS tests from the official GT01R archive.

This is a projection onto block paths, not a solve of the original sparse
system.  No scaling, diagonal shifts, RHS replacement or node duplication is
performed.  Path-external blocks are explicitly counted as omitted.
"""
from __future__ import annotations

import argparse
import io
import json
import math
from pathlib import Path
import tarfile

from single_line_dataset import oracle, sha256, unpivoted_diagnostics, write_dataset

SOURCE_URL = "https://sparse.tamu.edu/Fluorem/GT01R"
DOWNLOAD_URL = "https://www.cise.ufl.edu/research/sparse/MM/Fluorem/GT01R.tar.gz"
ARCHIVE_SHA256 = "223c7a6a949dd2c4093dc5a8879320d3d3d58002ded8d8bccce1a83f297137d7"


def market(stream):
    """Read real/general Matrix Market; validate counts and sum duplicates."""
    header = stream.readline().strip().lower().split()
    if len(header) != 5 or header[:2] != ["%%matrixmarket", "matrix"] or header[3:] != ["real", "general"]:
        raise ValueError("expected a real/general Matrix Market matrix")
    kind = header[2]
    lines = (line.strip() for line in stream)
    lines = (line for line in lines if line and not line.startswith("%"))
    shape = list(map(int, next(lines).split()))
    if any(v <= 0 for v in shape[:2]):
        raise ValueError("invalid matrix dimensions")
    rows, cols = shape[:2]
    values = {}
    if kind == "coordinate" and len(shape) == 3:
        count = 0
        for line in lines:
            fields = line.split()
            if len(fields) != 3:
                raise ValueError("invalid coordinate record")
            i, j, value = int(fields[0])-1, int(fields[1])-1, float(fields[2].replace('D','E').replace('d','e'))
            if not (0 <= i < rows and 0 <= j < cols and math.isfinite(value)):
                raise ValueError("invalid matrix coordinate/value")
            values[i, j] = values.get((i, j), 0.0)+value
            count += 1
        if count != shape[2]:
            raise ValueError("matrix entry count mismatch")
    elif kind == "array" and len(shape) == 2:
        flat = [float(v.replace('D','E').replace('d','e')) for line in lines for v in line.split()]
        if len(flat) != rows*cols or not all(math.isfinite(v) for v in flat):
            raise ValueError("invalid matrix array count/value")
        values = {(i % rows, i // rows): v for i, v in enumerate(flat)}
    else:
        raise ValueError("unsupported Matrix Market storage")
    if not all(math.isfinite(v) for v in values.values()):
        raise ValueError("duplicate entries overflow")
    return rows, cols, values


def read_archive(path):
    if sha256(path) != ARCHIVE_SHA256:
        raise ValueError("archive does not match the pinned official GT01R snapshot")
    with tarfile.open(path, "r:gz") as source:
        matrices = []
        member_hashes = {}
        for name in ("GT01R/GT01R.mtx", "GT01R/GT01R_b.mtx"):
            member = source.getmember(name)
            if not member.isfile() or member.size > 32*1024*1024:
                raise ValueError("invalid archive member")
            data = source.extractfile(member).read()
            import hashlib
            member_hashes[name] = hashlib.sha256(data).hexdigest()
            matrices.append(market(io.StringIO(data.decode("ascii"))))
    (rows, cols, matrix), (rhs_rows, rhs_cols, rhs) = matrices
    if (rows, cols, rhs_rows, rhs_cols, len(matrix)) != (7980, 7980, 7980, 1, 430909):
        raise ValueError("GT01R shape/nonzero count mismatch")
    blocks = {}
    for (i, j), value in matrix.items():
        block = blocks.setdefault((i//5, j//5), [0.0]*25)
        block[(i % 5)*5+j % 5] = value
    return blocks, [rhs[i, 0] for i in range(rows)], member_hashes


def graph(blocks, count):
    norms = {key: math.sqrt(math.fsum(v*v for v in value))
             for key, value in blocks.items()}
    neighbors = [[] for _ in range(count)]
    for (i, j), norm in norms.items():
        if i != j and norm and norms.get((j, i), 0):
            neighbors[i].append(j)
    for row in neighbors:
        row.sort()
    return neighbors, norms


def block_path(start, length, neighbors, norms, rule="index-nearest"):
    """Follow actual edges; preserve original index locality by default."""
    path, seen = [start], {start}
    while len(path) < length:
        i = path[-1]
        candidates = [j for j in neighbors[i] if j not in seen]
        if not candidates:
            raise ValueError(f"path from {start} ends after {len(path)} distinct nodes")
        if rule == 'index-nearest':
            j = min(candidates, key=lambda j: (abs(j-i), j))
        elif rule == 'strongest':
            j = min(candidates, key=lambda j: (
                -(norms[i, j]+norms[j, i])/(norms[i, i]+norms[j, j]), j))
        else:
            raise ValueError('unknown path rule')
        path.append(j)
        seen.add(j)
    return path


def records_for_path(path, blocks, rhs):
    raw = []
    for index, node in enumerate(path):
        d = blocks[node, node]
        lower = blocks[node, path[index-1]] if index else [0.0]*25
        upper = blocks[node, path[index+1]] if index+1 < len(path) else [0.0]*25
        raw.append(list(d)+list(lower)+list(upper)+rhs[5*node:5*(node+1)])
    return raw


def write_json(path, value):
    data = json.dumps(value, indent=2, sort_keys=True, allow_nan=False)+"\n"
    if path.exists() and path.read_text() != data:
        raise FileExistsError(f"refusing to replace different manifest: {path}")
    path.write_text(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--lengths", type=int, nargs="+", default=[17,64,65,98,99,128,256])
    parser.add_argument("--starts", type=int, nargs="+")
    parser.add_argument("--path-rule", choices=('index-nearest','strongest'), default='index-nearest')
    args = parser.parse_args()
    if any(n < 1 or n > 1596 for n in args.lengths):
        parser.error("length must be 1..1596")
    blocks, rhs, member_hashes = read_archive(args.archive)
    neighbors, norms = graph(blocks, len(rhs)//5)
    eligible = [i for i in range(len(neighbors))
                if neighbors[i] and any(rhs[5*i:5*(i+1)])]
    starts = args.starts or [eligible[0], eligible[len(eligible)//2], eligible[-1]]
    if any(s not in eligible for s in starts):
        parser.error("start nodes must have nonzero original RHS and bidirectional neighbors")
    args.output.mkdir(parents=True, exist_ok=True)
    summary = dict(source=SOURCE_URL, download_url=DOWNLOAD_URL,
        archive_sha256=sha256(args.archive), member_sha256=member_hashes,
        matrix_order=7980, matrix_nonzeros=430909, block_size=5,
        mesh_nodes=1596, block_entries=len(blocks),
        rhs_nonzero_nodes=len(eligible), starts=starts,
        derivation="block-path projection; path-external couplings omitted; not a full GT01R solve",
        path_rule=("minimum original node-ID distance along bidirectional nonzero edges; ascending ID breaks ties; no repeats"
                   if args.path_rule == 'index-nearest' else
                   "strongest normalized bidirectional Frobenius edge; ascending node ID breaks ties; no repeats"),
        scaling="none", diagonal_shift=0, rhs="original sensitivity RHS", cases=[], rejected=[])
    for start in starts:
        for length in args.lengths:
            name = f"gt01r-s{start}-n{length}"
            try:
                path = block_path(start, length, neighbors, norms, args.path_rule)
                raw = records_for_path(path, blocks, rhs)
                diagnostic = unpivoted_diagnostics(raw)
                high = oracle(raw, 200)
                higher = oracle(raw, 240)
                agreement = max(abs(a-b)/max(abs(a),abs(b),1e-14)
                    for row,other in zip(high,higher) for a,b in zip(row,other))
                if agreement > 1e-30:
                    raise ValueError("oracle precision cross-check exceeds 1e-30 scaled error")
                record = write_dataset(args.output/(name+".cfd5"), raw, high, precision=200)
                path_set = set(path)
                retained = {(node,node) for node in path}
                retained.update((a,b) for a,b in zip(path,path[1:]))
                retained.update((b,a) for a,b in zip(path,path[1:]))
                omitted = sum(any(block) and i in path_set and (i,j) not in retained
                              for (i,j), block in blocks.items())
                record.update(name=name, start=start, node_ids_zero_based=path,
                              omitted_block_entries_in_selected_rows=omitted,
                              oracle_scaled_difference_200_240=agreement,
                              max_abs_oracle_output=max(abs(v) for row in high for v in row),
                              **diagnostic)
                write_json(args.output/(name+".json"), dict(record,
                    source=SOURCE_URL, archive_sha256=summary['archive_sha256'],
                    derivation=summary['derivation'], path_rule=summary['path_rule']))
                summary['cases'].append(record)
                print(name, "written", diagnostic, flush=True)
            except ValueError as exc:
                summary['rejected'].append(dict(name=name, reason=str(exc)))
                print(name, "REJECTED", str(exc), flush=True)
    write_json(args.output/"manifest.json", summary)
    return 0 if summary['cases'] else 1


if __name__ == "__main__":
    raise SystemExit(main())
