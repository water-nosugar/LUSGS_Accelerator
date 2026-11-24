#!/usr/bin/env python3
"""Independent single-line blocked affine scan: numerical/cost prototype.

This is not a gem5 timing model. Every matrix composition is explicitly
counted; tree storage is external backing in the proposed bounded design.
"""
from collections import Counter
import argparse
import json
import math
from pathlib import Path

import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))

from single_line_dataset import N, read_dataset, sha256


class Arithmetic:
    def __init__(self):
        self.work = Counter()

    def mul(self,a,b):
        self.work['mul'] += 1
        return a*b

    def add(self,a,b):
        self.work['add_sub'] += 1
        return a+b

    def sub(self,a,b):
        self.work['add_sub'] += 1
        return a-b

    def div(self,a,b):
        self.work['div'] += 1
        return a/b

    def mv(self,a,x):
        result=[]
        for row in range(N):
            value=self.mul(a[row*N],x[0])
            for col in range(1,N):
                value=self.add(value,self.mul(a[row*N+col],x[col]))
            result.append(value)
        return result

    def mm(self,a,b):
        result=[]
        for row in range(N):
            for col in range(N):
                value=self.mul(a[row*N],b[col])
                for k in range(1,N):
                    value=self.add(value,self.mul(a[row*N+k],b[k*N+col]))
                result.append(value)
        return result

    def apply(self,pair,x):
        return [self.add(a,b) for a,b in zip(self.mv(pair[0],x),pair[1])]

    def compose(self,earlier,later):
        if earlier is None: return later
        if later is None: return earlier
        self.work['affine_compositions'] += 1
        return self.mm(later[0],earlier[0]),self.apply(later,earlier[1])


def transformed_coefficients(records):
    arithmetic=Arithmetic(); forward=[];upper=[]
    for i,record in enumerate(records):
        lu=list(record[:25])
        for k in range(N):
            if not math.isfinite(lu[k*N+k]) or abs(lu[k*N+k]) <= 1e-12:
                raise ValueError(f'unsupported unpivoted LU at cell {i}')
            for row in range(k+1,N):
                lu[row*N+k]=arithmetic.div(lu[row*N+k],lu[k*N+k])
                for col in range(k+1,N):
                    lu[row*N+col]=arithmetic.sub(lu[row*N+col],
                        arithmetic.mul(lu[row*N+k],lu[k*N+col]))
        inv=[arithmetic.div(1.0,lu[k*N+k]) for k in range(N)]
        def solve(rhs):
            x=list(rhs)
            for row in range(N):
                for col in range(row):
                    x[row]=arithmetic.sub(x[row],arithmetic.mul(lu[row*N+col],x[col]))
            for row in range(N-1,-1,-1):
                for col in range(row+1,N):
                    x[row]=arithmetic.sub(x[row],arithmetic.mul(lu[row*N+col],x[col]))
                x[row]=arithmetic.mul(x[row],inv[row])
            return x
        b=solve(record[75:80]);a=[0.0]*25;u=[0.0]*25
        for offset,matrix,needed in [(25,a,i>0),(50,u,i+1<len(records))]:
            if needed:
                for col in range(N):
                    x=solve([record[offset+row*N+col] for row in range(N)])
                    for row in range(N): matrix[row*N+col]=-x[row]
        forward.append((a,b));upper.append(u)
    return forward,upper,dict(arithmetic.work)


def sequential_scan(pairs,arithmetic):
    x=[0.0]*N;result=[]
    for pair in pairs:
        x=arithmetic.apply(pair,x);result.append(x)
    return result


def blocked_scan(pairs,block_cells,arithmetic):
    if block_cells < 1: raise ValueError('block_cells must be positive')
    blocks=[pairs[start:start+block_cells] for start in range(0,len(pairs),block_cells)]
    leaves=1
    while leaves<len(blocks): leaves*=2
    tree=[None]*(2*leaves)
    for i,block in enumerate(blocks):
        value=None
        for pair in block: value=arithmetic.compose(value,pair)
        tree[leaves+i]=value
    for i in range(leaves-1,0,-1):
        tree[i]=arithmetic.compose(tree[2*i],tree[2*i+1])
    prefix=[None]*(2*leaves)
    for i in range(1,leaves):
        prefix[2*i]=prefix[i]
        prefix[2*i+1]=arithmetic.compose(prefix[i],tree[2*i])
    result=[]
    for i,block in enumerate(blocks):
        x=[0.0]*N if prefix[leaves+i] is None else list(prefix[leaves+i][1])
        for pair in block:
            x=arithmetic.apply(pair,x);result.append(x)
    return result


def errors(values,expected):
    maximum_absolute=maximum_relative=0.0;mismatches=0
    for actual,reference in zip(values,expected):
        for a,b in zip(actual,reference):
            absolute=abs(a-b);relative=absolute/max(abs(b),1e-14)
            maximum_absolute=max(maximum_absolute,absolute)
            maximum_relative=max(maximum_relative,relative)
            mismatches += not math.isfinite(a) or (absolute>1e-10 and relative>1e-10)
    return dict(max_absolute=maximum_absolute,max_relative=maximum_relative,mismatches=mismatches)


def evaluate(path,block_cells,workers=4):
    records=read_dataset(path);n=len(records)
    pairs,upper,preprocess=transformed_coefficients(records)
    serial=Arithmetic();parallel=Arithmetic()
    sy=sequential_scan(pairs,serial)
    sq=list(reversed(sequential_scan([(upper[i],sy[i]) for i in range(n-1,-1,-1)],serial)))
    py=blocked_scan(pairs,block_cells,parallel)
    pq=list(reversed(blocked_scan([(upper[i],py[i]) for i in range(n-1,-1,-1)],block_cells,parallel)))
    expected=[row[80:90] for row in records]
    serial_error=errors([a+b for a,b in zip(sy,sq)],expected)
    scan_error=errors([a+b for a,b in zip(py,pq)],expected)
    blocks=(n+block_cells-1)//block_cells;leaves=1
    while leaves<blocks:leaves*=2
    compositions=parallel.work['affine_compositions']
    # Conservative non-replicated DRAM tree plan. Python lists represent its
    # external backing, never uncharged whole-line on-chip arrays.
    tree_bytes=4*leaves*240
    local_bytes=workers*(block_cells*280+2*240+128)+512
    return dict(input=str(Path(path).resolve()),input_sha256=sha256(path),cells=n,
        block_cells=block_cells,parallel_block_workers=workers,
        status='passed' if not serial_error['mismatches'] and not scan_error['mismatches'] else 'numeric_failed',
        tolerance='same mixed gate as guest: abs<=1e-10 OR rel<=1e-10; denominator floor=1e-14',
        sequential_error=serial_error,blocked_error=scan_error,
        preprocess_work=preprocess,sequential_recurrence_work=dict(serial.work),
        blocked_recurrence_work=dict(parallel.work),
        cost=dict(external_transformed_coefficient_bytes=440*n,external_output_bytes=80*n,
            reused_tree_backing_bytes=tree_bytes,
            proposed_bounded_worker_storage_bytes=local_bytes,
            extra_coefficient_scan_read_bytes=2*240*n,
            uncached_composition_read_write_payload_bytes=compositions*720,
            note='Conceptual bounded implementation budget; actual cache traffic/ports, stage scheduling and arithmetic latency are not simulated here.'),
        timing_model='none: numerical/work/storage prototype, not a gem5 performance result')


def main():
    parser=argparse.ArgumentParser();parser.add_argument('inputs',type=Path,nargs='+')
    parser.add_argument('--blocks',type=int,nargs='+',default=[8,16,32])
    parser.add_argument('--output',type=Path,required=True);args=parser.parse_args()
    rows=[evaluate(path,block) for path in args.inputs for block in args.blocks]
    args.output.write_text(json.dumps(rows,indent=2)+'\n')
    for row in rows:print(row['cells'],row['block_cells'],row['status'],row['blocked_error'])
    return int(any(row['status']!='passed' for row in rows))


if __name__=='__main__':raise SystemExit(main())
