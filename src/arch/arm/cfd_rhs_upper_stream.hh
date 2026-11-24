#ifndef CFD_RHS_UPPER_STREAM_HH
#define CFD_RHS_UPPER_STREAM_HH
#include <cstdint>
namespace gem5 { namespace ArmISA {
struct CfdRhsUpperStream {
    static constexpr uint64_t ControlBytes=512;
    static bool ready(unsigned column,unsigned normalized,bool pending) {
        if(column<1 || column>4 || pending || normalized>31)return false;
        const unsigned wanted=(1u<<(column+1))-1;
        return (normalized&wanted)==wanted;
    }
};
} }
#endif
