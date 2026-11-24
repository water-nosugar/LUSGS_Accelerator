"""Select stdout records without materializing every line of a large log."""
from functools import lru_cache
from collections.abc import Mapping
import csv
import re


@lru_cache(maxsize=64)
def _prefix_pattern(prefix):
    return re.compile(r'^'+re.escape(prefix)+r'[^\n]*',re.M)


def prefixed_lines(text,prefix):
    """Yield matching LF/CRLF stdout lines, including an unterminated tail."""
    for match in _prefix_pattern(prefix).finditer(text):
        yield match[0].rstrip('\r')


def iter_fields(text,prefix):
    for line in prefixed_lines(text,prefix+' '):
        yield dict(re.findall(r'(\w+)=([^\s]+)',line))


def fields(text,prefix):
    return list(iter_fields(text,prefix))


class TraceRow(Mapping):
    """Read-only CSV row sharing its schema and repeated scalar strings."""
    __slots__=('_values','indices')

    def __init__(self,values,indices):
        self._values=values;self.indices=indices

    def __getitem__(self,key):
        return self._values[self.indices[key]]

    def __iter__(self):
        return iter(self.indices)

    def __len__(self):
        return len(self.indices)

    def __repr__(self):
        return repr(dict(self))


def compact_trace(path,skip_stages=()):
    """Preserve every non-diagnostic column/event without a dict per row."""
    shared_columns={'requestId','generation','line','cell','engine','stage',
        'rhsBatch','rhsColumn','k','row','slot','queueOccupancy','readyToken',
        'completionToken','requester','size','bank','port'}
    rows=[];pool={}
    with path.open(newline='') as source:
        reader=csv.reader(source);header=next(reader)
        if len(set(header))!=len(header) or 'stage' not in header:
            raise ValueError('invalid trace CSV schema')
        indices={name:i for i,name in enumerate(header)};stage=indices['stage']
        shared=[i for i,name in enumerate(header) if name in shared_columns]
        for values in reader:
            if len(values)!=len(header):raise ValueError('malformed trace CSV row')
            if values[stage] in skip_stages:continue
            for i in shared:
                value=values[i]
                if value in pool:values[i]=pool[value]
                elif len(pool)<65536:pool[value]=value
            rows.append(TraceRow(tuple(values),indices))
    return rows
