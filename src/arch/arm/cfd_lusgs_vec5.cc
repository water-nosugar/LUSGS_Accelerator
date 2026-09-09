#include "arch/arm/cfd_lusgs_vec5.hh"

namespace gem5
{
namespace ArmISA
{

void
cfdVec5Copy(double *dst, const double *src)
{
    for (int i = 0; i < 5; ++i)
        dst[i] = src[i];
}

void
cfdVec5Sub(double *dst, const double *src0, const double *src1)
{
    for (int i = 0; i < 5; ++i)
        dst[i] = src0[i] - src1[i];
}

void
cfdVec5Axpy(double *dst, const double *src0, double scalar,
            const double *src1)
{
    for (int i = 0; i < 5; ++i)
        dst[i] = src0[i] + scalar * src1[i];
}

} // namespace ArmISA
} // namespace gem5
