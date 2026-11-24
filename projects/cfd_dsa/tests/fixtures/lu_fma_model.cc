#include "arch/arm/cfd_lu_fma.hh"
#include "arch/arm/cfd_rhs_admission.hh"
#include "arch/arm/cfd_lu_rank1.hh"
#include "arch/arm/cfd_register_ports.hh"
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>
#include <random>
#include <vector>
using namespace gem5::ArmISA;
using Matrix=std::array<double,25>;
template<class F> void rejects(F call) {
    bool caught=false;try{call();}catch(const std::logic_error &){caught=true;}assert(caught);
}
static Matrix reference(const Matrix &d) {
    Matrix lu{};
    for(unsigned c=0;c<5;++c) {
        for(unsigned row=c;row<5;++row) {
            double value=d[5*row+c];
            for(unsigned k=0;k<c;++k)value=std::fma(-lu[5*row+k],lu[5*k+c],value);
            lu[5*row+c]=value;
        }
        const double reciprocal=1.0/lu[5*c+c];
        for(unsigned j=c+1;j<5;++j) {
            double value=d[5*c+j];
            for(unsigned k=0;k<c;++k)value=std::fma(-lu[5*c+k],lu[5*k+j],value);
            volatile double normalized=value*reciprocal;lu[5*c+j]=normalized;
        }
    }
    return lu;
}
static Matrix run(const Matrix &d,unsigned jitter,bool folded) {
    CfdLuRank1 state;state.bind(7,9,0,folded);
    CfdRegisterPorts rf;rf.configure({24,12});
    Matrix lu=d;double reciprocal=0;
    std::array<uint64_t,4> lanes{};
    struct Event{uint64_t at,id;unsigned kind,c,row,width;std::array<double,5> value;};
    std::vector<Event> events;
    uint64_t identity=0;unsigned live=0,fmas=0,normalizations=0,pivots=0;
    for(uint64_t now=0;now<2000;++now) {
        for(size_t i=0;i<events.size();) {
            auto e=events[i];
            if(e.at>now || !rf.reserveWrite(now,e.width)){++i;continue;}
            events.erase(events.begin()+i);
            if(e.kind==0){assert(state.reciprocal(e.id,now));reciprocal=e.value[0];}
            if(e.kind==1){assert(state.scaled(e.id,now));for(unsigned j=e.c+1;j<5;++j)lu[5*e.c+j]=e.value[j];}
            if(e.kind==2){assert(state.subtracted(e.row,e.id,now));live-=e.width;for(unsigned j=e.c+1;j<5;++j)lu[5*e.row+j]=e.value[j];}
        }
        if(state.canPivot(now)) {
            const unsigned c=state.column;assert(rf.reserveReads(now,2));state.pivot(++identity,now);++pivots;
            events.push_back({now+12+(jitter+identity)%3,identity,0,c,0,1,{1.0/lu[5*c+c]}});
        }else if(state.canScale()) {
            const unsigned c=state.column,w=4-c;std::array<double,5> value{};
            assert(rf.reserveReads(now,2*w));
            for(unsigned j=c+1;j<5;++j){volatile double v=lu[5*c+j]*reciprocal;value[j]=v;}
            state.scale(++identity);normalizations+=w;
            events.push_back({now+3+(jitter+identity)%3,identity,1,c,0,w,value});
        }else if(state.canBeginRows(now))state.beginRows(now);
        else if(state.canFinish(now))state.finish(now);
        if(state.canFma(now)) {
            const unsigned c=state.column,row=state.nextSub,w=4-c;
            const unsigned available=std::count_if(lanes.begin(),lanes.end(),[now](auto t){return t<=now;});
            if(available>=w && live+w<=20 && rf.reserveReads(now,CfdLuFma::reads(w,true))) {
                std::array<double,5> value{};unsigned used=0;
                for(auto &t:lanes)if(t<=now && used<w){t=now+1;++used;}
                for(unsigned j=c+1;j<5;++j)value[j]=CfdLuFma::evaluate(lu[5*row+c],lu[5*c+j],lu[5*row+j]);
                state.fused(row,++identity,now);assert(!state.canFma(now));live+=w;fmas+=w;
                events.push_back({now+5+(jitter+identity)%3,identity,2,c,row,w,value});
            }
        }
        for(const auto &p:state.products)assert(!p.operation);
        assert(live<=20);
        if(state.phase==CfdLuRank1::Phase::Done && !state.inflight) {
            assert(!live && events.empty() && fmas==30 && normalizations==10 && pivots==5);
            assert(state.fusedLanes==30);return lu;
        }
    }
    assert(false);return {};
}
int main() {
    assert(CfdRhsAdmission::Reservation==256);
    assert(!CfdRhsAdmission::permit(5,6,true,false)); // Background cell 6 at t=363.
    assert(CfdRhsAdmission::permit(5,6,true,true));  // Oldest b is ready at t=371.
    assert(!CfdRhsAdmission::permit(6,6,false,true));
    assert(CfdRhsAdmission::permit(5,6,false,false));
    assert(CfdRhsAdmission::permit(0,6,true,false));
    rejects([]{CfdRhsAdmission::permit(7,6,true,true);});

    CfdLuRank1 state;state.bind(7,9,0,true);state.pivot(1,0);state.reciprocal(1,12);
    state.scale(2);state.scaled(2,15);state.beginRows(15);state.fused(1,3,15);
    rejects([&]{state.fused(2,4,15);});state.fused(2,4,16);state.cancel();
    rejects([&]{state.bind(8,10,17,true);});assert(!state.subtracted(2,4,21));
    rejects([&]{state.bind(8,10,21,true);});assert(!state.subtracted(1,3,22));
    state.bind(8,10,23,true);assert(state.canPivot(23));
    std::mt19937_64 rng(213907);std::uniform_real_distribution<double> random(-.35,.35);
    long double residual=0;
    for(unsigned sample=0;sample<3000;++sample) {
        Matrix d{};for(auto &v:d)v=random(rng);for(unsigned i=0;i<5;++i)d[5*i+i]+=8;
        const auto wanted=reference(d),a=run(d,sample,false),b=run(d,sample,true);
        assert(std::memcmp(wanted.data(),a.data(),sizeof(a))==0);
        assert(std::memcmp(wanted.data(),b.data(),sizeof(b))==0);
        for(unsigned i=0;i<5;++i)for(unsigned j=0;j<5;++j) {
            long double value=-d[5*i+j],scale=1+std::abs(d[5*i+j]);
            for(unsigned k=0;k<=std::min(i,j);++k) {
                const long double term=static_cast<long double>(a[5*i+k])*(k==j ? 1 : a[5*k+j]);
                value+=term;scale+=std::abs(term);
            }
            residual=std::max(residual,std::abs(value)/scale);
        }
    }
    assert(residual<1e-13L && CfdLuFma::reservation(4)==4096 && sizeof(CfdLuRank1)<=512);
    const double x=0x1.0000002p0,y=0x1.ffffffcp-1;
    assert(CfdLuFma::evaluate(x,y,1.0)==0x1p-54 && 1.0-x*y==0.0);
    std::cout<<"LU_FMA_MODEL_PASS matrices=3000 control_variants=2 pending_limit=20 once_rounding=1 cancellation=1 residual="<<residual<<"\n";
}
