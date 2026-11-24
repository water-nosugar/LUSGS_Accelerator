"""Portable, checksummed inputs and an independent oracle for 5x5 SGS.

The oracle uses Decimal Gaussian elimination with partial pivoting.  It does
not form Lbar/Ubar and does not reuse the guest's unpivoted LU implementation.
Files contain one line: original D/L/U/R followed by rounded oracle y/dq.
"""
from __future__ import annotations

from decimal import Decimal, localcontext
import math
from pathlib import Path
import struct

from artifact_hash import file_sha256 as sha256

N = 5
MAGIC = b"CFD5IN01"
HEADER = struct.Struct("<8sIIQQQQ")
RECORD = struct.Struct("<90d")
FNV_OFFSET = 14695981039346656037
FNV_PRIME = 1099511628211


def fnv64(data):
    value = FNV_OFFSET
    for byte in data:
        value = ((value ^ byte) * FNV_PRIME) & ((1 << 64) - 1)
    return value


def solve_decimal(a, b):
    """Solve a fresh 5x5 system using pivoted elimination at caller precision."""
    a = [list(a[r * N:(r + 1) * N]) + [b[r]] for r in range(N)]
    for col in range(N):
        pivot = max(range(col, N), key=lambda r: abs(a[r][col]))
        if not a[pivot][col]:
            raise ValueError("singular diagonal block in Decimal oracle")
        a[col], a[pivot] = a[pivot], a[col]
        for row in range(col + 1, N):
            factor = a[row][col] / a[col][col]
            for j in range(col + 1, N + 1):
                a[row][j] -= factor * a[col][j]
            a[row][col] = Decimal(0)
    x = [Decimal(0)] * N
    for row in range(N - 1, -1, -1):
        x[row] = (a[row][N] - sum(a[row][j] * x[j]
                  for j in range(row + 1, N))) / a[row][row]
    return x


def oracle(records, precision=80):
    """Evaluate D*y + L*y_prev=R; D*(dq-y)+U*dq_next=0."""
    with localcontext() as context:
        context.prec = precision
        raw = [[Decimal.from_float(float(v)) for v in row] for row in records]
        y = []
        for i, row in enumerate(raw):
            rhs = row[75:80]
            if i:
                rhs = [rhs[r] - sum(row[25+r*N+c] * y[i-1][c]
                       for c in range(N)) for r in range(N)]
            y.append(solve_decimal(row[:25], rhs))
        dq = [None] * len(raw)
        for i in range(len(raw)-1, -1, -1):
            if i + 1 == len(raw):
                dq[i] = list(y[i])
            else:
                product = [sum(raw[i][50+r*N+c] * dq[i+1][c]
                           for c in range(N)) for r in range(N)]
                correction = solve_decimal(raw[i][:25], product)
                dq[i] = [y[i][r] - correction[r] for r in range(N)]
        values = [[float(v) for v in a+b] for a, b in zip(y, dq)]
    if not all(math.isfinite(v) for row in values for v in row):
        raise ValueError("oracle output overflows FP64")
    return values


def validate_records(records):
    if not records:
        raise ValueError("empty dataset")
    if any(len(row) != 90 or not all(math.isfinite(v) for v in row)
           for row in records):
        raise ValueError("records must contain 90 finite FP64 values")
    if any(records[0][25:50]) or any(records[-1][50:75]):
        raise ValueError("nonzero coefficients outside line endpoints")


def write_dataset(path, raw, expected=None, precision=80):
    expected = oracle(raw, precision) if expected is None else expected
    if len(raw) != len(expected):
        raise ValueError("oracle length mismatch")
    records = [list(a)+list(b) for a, b in zip(raw, expected)]
    validate_records(records)
    body = b"".join(RECORD.pack(*row) for row in records)
    checksum = fnv64(body)
    data = HEADER.pack(MAGIC, 1, N, 1, len(records), len(body), checksum)+body
    path = Path(path)
    if path.exists():
        if path.read_bytes() != data:
            raise FileExistsError(f"refusing to replace different dataset: {path}")
    else:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    return dict(cells=len(records), lines=1, payload_bytes=len(body),
                fnv64=f"{checksum:016x}", sha256=sha256(path),
                format="CFD5IN01", oracle_precision=precision)


def read_dataset(path):
    data = Path(path).read_bytes()
    if len(data) < HEADER.size:
        raise ValueError("truncated dataset header")
    magic, version, n, lines, cells, length, checksum = HEADER.unpack_from(data)
    body = data[HEADER.size:]
    if (magic != MAGIC or version != 1 or n != N or lines != 1 or
            not cells or length != cells*RECORD.size or len(body) != length):
        raise ValueError("invalid dataset dimensions, format or length")
    if fnv64(body) != checksum:
        raise ValueError("dataset checksum mismatch")
    records = [list(row) for row in RECORD.iter_unpack(body)]
    validate_records(records)
    return records


def unpivoted_diagnostics(raw, epsilon=1e-12):
    pivots, growth = [], []
    for index, row in enumerate(raw):
        a = list(row[:25])
        original = max(map(abs, a))
        for k in range(N):
            pivot = a[k*N+k]
            if not math.isfinite(pivot) or abs(pivot) <= epsilon:
                raise ValueError(f"cell {index}: unsupported unpivoted LU pivot {pivot}")
            pivots.append(abs(pivot))
            for r in range(k+1, N):
                a[r*N+k] /= pivot
                for c in range(k+1, N):
                    a[r*N+c] -= a[r*N+k]*a[k*N+c]
        growth.append(max(abs(a[r*N+c]) for r in range(N)
                          for c in range(r, N))/original)
    nonzeros = [abs(v) for row in raw for v in row[:75] if v]
    return dict(min_abs_pivot=min(pivots), max_abs_pivot=max(pivots),
                max_lu_growth=max(growth),
                coefficient_abs_min=min(nonzeros), coefficient_abs_max=max(nonzeros),
                nonzero_rhs_components=sum(v != 0 for row in raw for v in row[75:80]))
