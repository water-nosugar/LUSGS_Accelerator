#include "arch/arm/cfd_lu_rank1.hh"
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>
#include <random>
#include <vector>
using gem5::ArmISA::CfdLuRank1;
using Matrix=std::array<double,25>;
template<class F> void rejects(F call) {
    bool caught=false;
    try { call(); } catch(const std::logic_error &) { caught=true; }
    assert(caught);
}
Matrix reference(const Matrix &d,bool fused) {
    Matrix lu{};
    for (unsigned c=0;c<5;++c) {
        for (unsigned r=c;r<5;++r) {
            double x=d[5*r+c];
            for (unsigned k=0;k<c;++k) {
                volatile double p=lu[5*r+k]*lu[5*k+c];
                volatile double s=fused ? std::fma(-lu[5*r+k],lu[5*k+c],x) : x-p;x=s;
            }
            lu[5*r+c]=x;
        }
        volatile double reciprocal=1.0/lu[5*c+c];
        for (unsigned j=c+1;j<5;++j) {
            double x=d[5*c+j];
            for (unsigned k=0;k<c;++k) {
                volatile double p=lu[5*c+k]*lu[5*k+j];
                volatile double s=fused ? std::fma(-lu[5*c+k],lu[5*k+j],x) : x-p;x=s;
            }
            volatile double result=x*reciprocal;lu[5*c+j]=result;
        }
    }
    return lu;
}
Matrix run(const Matrix &d,unsigned jitter,bool folded,bool fused,bool prefetch) {
    CfdLuRank1 s;s.bind(7,9,0,folded);
    Matrix lu=d;
    struct Event { uint64_t at,id;unsigned kind,c,row,slot;std::array<double,5> value; };
    std::vector<Event> events;
    uint64_t id=0;std::array<double,5> reciprocal{};unsigned early=0;
    unsigned muls=0,subs=0,scales=0,pivots=0;
    for (uint64_t now=0;now<2000;++now) {
        for (size_t i=0;i<events.size();) {
            const auto e=events[i];
            if (e.at!=now) { ++i;continue; }
            events.erase(events.begin()+i);
            if (e.kind==0 || e.kind==4) { assert(e.kind==4 ? s.prefetched(e.id,now) : s.reciprocal(e.id,now));reciprocal[e.c]=e.value[0]; }
            if (e.kind==1) {
                assert(s.scaled(e.id,now));
                for(unsigned j=e.c+1;j<5;++j)lu[5*e.c+j]=e.value[j];
            }
            if (e.kind==2)assert(s.multiplied(e.slot,e.id,e.value));
            if (e.kind==3) {
                assert(s.subtracted(e.row,e.id,now));
                for(unsigned j=e.c+1;j<5;++j)lu[5*e.row+j]=e.value[j];
            }
        }
        if(s.canPivot(now)) {
            const auto c=s.column;
            if(s.prefetchId)s.adoptPrefetch(now);
            else {
                s.pivot(++id,now);++pivots;
                volatile double value=1.0/lu[5*c+c];
                events.push_back({now+12+(jitter+id)%3,id,0,c,0,0,{value}});
            }
        } else if(s.canScale()) {
            const auto c=s.column;std::array<double,5> value{};
            for(unsigned j=c+1;j<5;++j) { volatile double v=lu[5*c+j]*reciprocal[c];value[j]=v; }
            s.scale(++id);scales+=4-c;
            events.push_back({now+3+(jitter+id)%3,id,1,c,0,0,value});
        } else if(s.canBeginRows(now))s.beginRows(now);
        else if(s.canFinish(now))s.finish(now);
        if(s.phase==CfdLuRank1::Phase::Rows && fused && s.canFma(now)) {
            const auto c=s.column,row=s.nextSub;std::array<double,5> value{};
            for(unsigned j=c+1;j<5;++j)value[j]=std::fma(-lu[5*row+c],lu[5*c+j],lu[5*row+j]);
            s.fused(row,++id,now);muls+=4-c;subs+=4-c;
            events.push_back({now+5+(jitter+id)%3,id,3,c,row,0,value});
        }
        if(s.phase==CfdLuRank1::Phase::Rows && !fused) {
            int slot=s.subSlot();
            if(slot>=0) {
                const auto c=s.column,row=s.nextSub;std::array<double,5> value{};
                for(unsigned j=c+1;j<5;++j) {
                    volatile double v=lu[5*row+j]-s.products[slot].value[j];value[j]=v;
                }
                s.subtract(slot,row,++id);subs+=4-c;
                events.push_back({now+4+(jitter+id)%3,id,3,c,row,unsigned(slot),value});
            }
            slot=s.mulSlot();
            if(slot>=0) {
                const auto c=s.column,row=s.nextMul;std::array<double,5> value{};
                for(unsigned j=c+1;j<5;++j) {
                    volatile double v=lu[5*row+c]*lu[5*c+j];value[j]=v;
                }
                s.multiply(slot,row,++id);muls+=4-c;
                events.push_back({now+4+(jitter+id)%3,id,2,c,row,unsigned(slot),value});
            }
        }
        if(prefetch && s.canPrefetch()) {
            const auto c=s.column+1;volatile double value=1.0/lu[5*c+c];
            s.prefetch(++id);++pivots;++early;
            events.push_back({now+12+(jitter+id)%3,id,4,c,0,0,{value}});
        }
        if(s.phase==CfdLuRank1::Phase::Done && !s.inflight) {
            if(prefetch)assert(early>0);
            assert(events.empty() && muls==30 && subs==30 && scales==10 && pivots==5);
            return lu;
        }
    }
    assert(false);return {};
}
void cancellation(bool adopted,bool returnedFirst) {
    CfdLuRank1 s;s.bind(7,9,0,true);s.pivot(1,0);s.reciprocal(1,12);
    s.scale(2);s.scaled(2,15);s.beginRows(15);
    s.fused(1,3,15);s.subtracted(1,3,20);assert(s.canPrefetch());s.prefetch(100);
    rejects([&]{s.prefetch(101);});
    if(returnedFirst)assert(s.prefetched(100,21));
    if(adopted) {
        for(unsigned row=2;row<5;++row){s.fused(row,row+2,20+row);s.subtracted(row,row+2,24+row);}
        assert(s.canPivot(30));s.adoptPrefetch(30);
        if(returnedFirst)assert(s.reciprocalReturned==21);
    }
    s.cancel();
    if(!returnedFirst) {
        rejects([&]{s.bind(8,10,31);});assert(!s.prefetched(100,32));
    }
    s.bind(8,10,33);assert(s.inflight==0);
}
int main() {
    cancellation(false,false);cancellation(true,false);cancellation(true,true);
    CfdLuRank1 invalid;invalid.bind(1,2,0,true);rejects([&]{invalid.prefetch(1);});
    std::mt19937_64 rng(921987);std::uniform_real_distribution<double> dist(-2.0,2.0);
    for(unsigned n=0;n<2000;++n) {
        Matrix d{};for(auto &v:d)v=dist(rng);for(unsigned i=0;i<5;++i)d[5*i+i]+=16.0;
        for(bool fused:{false,true})for(bool folded:{false,true}) {
            const auto a=reference(d,fused),b=run(d,n,folded,fused,false),c=run(d,n,folded,fused,true);
            assert(std::memcmp(a.data(),b.data(),sizeof(a))==0);
            assert(std::memcmp(a.data(),c.data(),sizeof(a))==0);
        }
    }
    assert(sizeof(CfdLuRank1)<=512);
    std::cout << "LU_PIVOT_PREFETCH_MODEL_PASS matrices=2000 variants=8 bitwise=1 cancellation=3 state_bytes=" << sizeof(CfdLuRank1) << "\n";
}
