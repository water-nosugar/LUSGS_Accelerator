// Reuse the existing ordered admission metadata; reserve one context credit
// only while the oldest unfinished direct base has not acquired a context.
#ifndef CFD_RHS_ADMISSION_HH
#define CFD_RHS_ADMISSION_HH
#include <cstdint>
#include <stdexcept>
namespace gem5 { namespace ArmISA {
struct CfdRhsAdmission {
    static constexpr uint64_t Reservation=256;
    static bool permit(unsigned active,unsigned capacity,bool protectedWaiting,
                       bool candidateIsProtected) {
        if(!capacity || active>capacity)throw std::logic_error("invalid RHS context ledger");
        if(active==capacity)return false;
        return !protectedWaiting || candidateIsProtected || active+1<capacity;
    }
};
} }
#endif
