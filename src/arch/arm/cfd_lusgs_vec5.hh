#ifndef __ARCH_ARM_CFD_LUSGS_VEC5_HH__
#define __ARCH_ARM_CFD_LUSGS_VEC5_HH__

#include <cstdint>

namespace gem5
{
namespace ArmISA
{

enum class CfdLusgsVec5Op : uint8_t
{
    Copy,
    Sub,
    Axpy,
};

void cfdVec5Copy(double *dst, const double *src);
void cfdVec5Sub(double *dst, const double *src0, const double *src1);
void cfdVec5Axpy(double *dst, const double *src0, double scalar,
                 const double *src1);

} // namespace ArmISA
} // namespace gem5

#endif // __ARCH_ARM_CFD_LUSGS_VEC5_HH__
