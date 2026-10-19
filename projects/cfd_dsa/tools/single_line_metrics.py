"""Accounting helpers for cumulative snapshots and overlapping wait counters."""
from collections import Counter


def bank_utilization(snapshots, cycles, banks, read_ports, write_ports):
    if cycles <= 0 or min(banks, read_ports, write_ports) <= 0:
        raise ValueError("positive window and bank resources required")
    latest = {int(row['bank']): row for row in snapshots}
    if set(latest) != set(range(banks)):
        raise ValueError("missing bank snapshots")
    values = {}
    for kind, ports in [('read', read_ports), ('write', write_ports)]:
        words = sum(int(row[kind+'_words']) for row in latest.values())
        slots = cycles*banks*ports
        if not 0 <= words <= slots:
            raise ValueError("bank accesses exceed physical port slots")
        values[kind+'_words'] = words
        values[kind+'_pct'] = 100*words/slots
    return values


def phase_breakdown(start, end, intervals, tile_wait=0, output_wait=0):
    """Only active/inactive are additive; wait observations may overlap work."""
    if end < start:
        raise ValueError("reversed phase window")
    events = Counter()
    for lo, hi in intervals:
        lo, hi = max(start, lo), min(end, hi)
        if hi > lo:
            events[lo] += 1
            events[hi] -= 1
    live = active = 0
    previous = start
    for point, change in sorted(events.items()):
        if live:
            active += point-previous
        live += change
        previous = point
    return dict(cycles=end-start, math_active_cycles=active,
                math_inactive_cycles=end-start-active,
                wait_counters=dict(tile_wait=int(tile_wait), output_queue_wait=int(output_wait)),
                wait_counters_additive=False)
