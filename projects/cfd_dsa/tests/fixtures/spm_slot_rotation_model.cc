#include "arch/arm/cfd_spm_slot_rotation.hh"
#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>
using gem5::ArmISA::CfdSpmSlotRotation;
struct Result { unsigned cycles;std::vector<uint64_t> values; };
Result run(unsigned mode,bool rhs,unsigned words) {
    const unsigned stride=rhs ? 128 : 8192;
    std::vector<uint64_t> memory(8*stride/8,~uint64_t(0)),output(8*words,0);
    unsigned totalCycles=0;
    for(bool write:{true,false}) {
        std::array<unsigned,8> next{};unsigned done=0,cycles=0;
        while(done!=8*words) {
            std::array<unsigned,4> ports{1,1,1,1};
            for(unsigned slot=0;slot<8;++slot) {
                const uint64_t base=rhs ? CfdSpmSlotRotation::rhs(mode,slot) : CfdSpmSlotRotation::input(mode,slot);
                assert(base>=slot*stride && base+words*8<=uint64_t(slot+1)*stride);
                while(next[slot]<words) {
                    const uint64_t address=base+next[slot]*8;const auto bank=(address/8)%4;
                    if(!ports[bank])break;
                    --ports[bank];const uint64_t value=slot*1024+next[slot]+1;
                    if(write)memory[address/8]=value;
                    else {assert(memory[address/8]==value);output[slot*words+next[slot]]=memory[address/8];}
                    ++next[slot];++done;
                }
            }
            ++cycles;assert(cycles<10000);
        }
        totalCycles+=cycles+2;
    }
    return {totalCycles,output};
}
int main() {
    const auto oldR=run(0,true,5),newR=run(1,true,5);
    assert(oldR.values==newR.values && oldR.cycles==36 && newR.cycles==24);
    for(unsigned mode=0;mode<4;++mode)for(bool rhs:{false,true})for(unsigned words:{1,5,25,50,80}) {
        if(rhs && words>5)continue;
        assert(run(mode,rhs,words).values==run(0,rhs,words).values);
    }
    for(unsigned slot=0;slot<64;++slot) {
        assert(CfdSpmSlotRotation::input(1,slot)==uint64_t(slot)*8192);
        assert(CfdSpmSlotRotation::rhs(2,slot)==uint64_t(slot)*128);
        assert((CfdSpmSlotRotation::rhs(3,slot)/8)%4==slot%4);
        assert((CfdSpmSlotRotation::input(3,slot)/8)%4==slot%4);
    }
    std::cout << "SPM_SLOT_ROTATION_PASS all_ready_8_rhs_service_cycles=16_to_10 same_words=1 same_ports=1 control_bytes=128\n";
}
