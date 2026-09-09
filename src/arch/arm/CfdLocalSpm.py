from m5.SimObject import SimObject
from m5.params import Param, RequestPort
from m5.proxy import Parent


class CfdLocalSpm(SimObject):
    type = "CfdLocalSpm"
    cxx_class = "gem5::ArmISA::CfdLocalSpm"
    cxx_header = "arch/arm/cfd_local_spm.hh"

    system = Param.System(Parent.any, "System used for DMA requestor IDs")
    dma_port = RequestPort(
        "Stage 6b timing DMA port for guest-memory coefficient traffic"
    )
