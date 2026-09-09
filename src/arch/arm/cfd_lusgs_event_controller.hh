#ifndef __ARCH_ARM_CFD_LUSGS_EVENT_CONTROLLER_HH__
#define __ARCH_ARM_CFD_LUSGS_EVENT_CONTROLLER_HH__

#include <cstdint>

#include "base/types.hh"

namespace gem5
{

class ExecContext;

namespace ArmISA
{

bool cfdLusgsEventEnabled();
bool cfdLusgsEventBusy();
uint64_t cfdLusgsEventLaunch(ExecContext *xc, Addr descriptor_addr);
uint64_t cfdLusgsEventWait(uint64_t token);

} // namespace ArmISA
} // namespace gem5

#endif // __ARCH_ARM_CFD_LUSGS_EVENT_CONTROLLER_HH__
