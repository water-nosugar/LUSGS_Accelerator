#ifndef __ARCH_ARM_CFD_TRSV5_MATH_HH__
#define __ARCH_ARM_CFD_TRSV5_MATH_HH__

namespace gem5
{
namespace ArmISA
{

static inline double
cfdTrsv5MulSub(double acc, double lhs, double rhs)
{
    volatile double product = lhs * rhs;
    return acc - product;
}

static inline void
cfdTrsv5Solve(const double *lu, double *value)
{
    for (int k = 0; k < 4; ++k) {
        value[k] /= lu[k * 5 + k];
        for (int i = k + 1; i < 5; ++i)
            value[i] = cfdTrsv5MulSub(value[i], lu[i * 5 + k], value[k]);
    }
    value[4] /= lu[4 * 5 + 4];
    for (int k = 3; k >= 0; --k) {
        for (int i = k + 1; i < 5; ++i)
            value[k] = cfdTrsv5MulSub(value[k], lu[k * 5 + i], value[i]);
    }
}

} // namespace ArmISA
} // namespace gem5

#endif // __ARCH_ARM_CFD_TRSV5_MATH_HH__
