"""Run a freestanding architectural SME reference on an ordinary Arm SE CPU."""

from pathlib import Path
import sys

import m5
from m5.objects import (
    AddrRange, ArmISA, ArmTimingSimpleCPU, Process, Root, SEWorkload, SimpleMemory,
    SrcClockDomain, System, SystemXBar, VoltageDomain,
)


binary = str(Path(sys.argv[1]).resolve())
system = System()
system.clk_domain = SrcClockDomain(clock="2GHz", voltage_domain=VoltageDomain())
system.mem_mode = "timing"
system.mem_ranges = [AddrRange("32MiB")]
system.bus = SystemXBar()
system.mem = SimpleMemory(range=system.mem_ranges[0])
system.mem.port = system.bus.mem_side_ports
system.system_port = system.bus.cpu_side_ports
system.cpu = ArmTimingSimpleCPU(isa=[ArmISA(sve_vl_se=4, sme_vl_se=4)])
system.cpu.icache_port = system.bus.cpu_side_ports
system.cpu.dcache_port = system.bus.cpu_side_ports
system.cpu.createInterruptController()
system.workload = SEWorkload.init_compatible(binary)
system.cpu.workload = Process(cmd=[binary, *sys.argv[2:]])
system.cpu.createThreads()
root = Root(full_system=False, system=system)
m5.instantiate()
event = m5.simulate(5_000_000_000_000)
if event.getCause() != "exiting with last active thread context" or event.getCode():
    raise RuntimeError(f"SME guest exited with {event.getCause()} ({event.getCode()})")
