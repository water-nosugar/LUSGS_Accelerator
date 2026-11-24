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
Matrix reference(const Matrix &d) {
    Matrix lu{};
    for (unsigned c=0;c<5;++c) {
        for (unsigned r=c;r<5;++r) {
            double x=d[5*r+c];
            for (unsigned k=0;k<c;++k) {
                volatile double p=lu[5*r+k]*lu[5*k+c];
                volatile double s=x-p;x=s;
            }
            lu[5*r+c]=x;
        }
        volatile double reciprocal=1.0/lu[5*c+c];
        for (unsigned j=c+1;j<5;++j) {
            double x=d[5*c+j];
            for (unsigned k=0;k<c;++k) {
                volatile double p=lu[5*c+k]*lu[5*k+j];
                volatile double s=x-p;x=s;
            }
            volatile double result=x*reciprocal;lu[5*c+j]=result;
        }
    }
    return lu;
}
Matrix run(const Matrix &d,unsigned jitter,bool folded=false) {
    CfdLuRank1 s;s.bind(7,9,0,folded);
    Matrix lu=d;
    struct Event { uint64_t at,id;unsigned kind,c,row,slot;std::array<double,5> value; };
    std::vector<Event> events;
    uint64_t id=0;double reciprocal=0;
    unsigned muls=0,subs=0,scales=0,pivots=0;
    for (uint64_t now=0;now<2000;++now) {
        for (size_t i=0;i<events.size();) {
            const auto e=events[i];
            if (e.at!=now) { ++i;continue; }
            events.erase(events.begin()+i);
            if (e.kind==0) { assert(s.reciprocal(e.id,now));reciprocal=e.value[0]; }
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
            const auto c=s.column;s.pivot(++id,now);++pivots;
            volatile double value=1.0/lu[5*c+c];
            events.push_back({now+12+(jitter+id)%3,id,0,c,0,0,{value}});
        } else if(s.canScale()) {
            const auto c=s.column;std::array<double,5> value{};
            for(unsigned j=c+1;j<5;++j) { volatile double v=lu[5*c+j]*reciprocal;value[j]=v; }
            s.scale(++id);scales+=4-c;
            events.push_back({now+3+(jitter+id)%3,id,1,c,0,0,value});
        } else if(s.canBeginRows(now))s.beginRows(now);
        else if(s.canFinish(now))s.finish(now);
        if(s.phase==CfdLuRank1::Phase::Rows) {
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
        if(s.phase==CfdLuRank1::Phase::Done && !s.inflight) {
            assert(events.empty() && muls==30 && subs==30 && scales==10 && pivots==5);
            return lu;
        }
    }
    assert(false);return {};
}
int main() {
    CfdLuRank1 s;s.bind(7,9,10);
    rejects([&] { s.pivot(1,10); });s.pivot(1,11);
    rejects([&] { s.scale(2); });s.reciprocal(1,23);s.scale(2);s.scaled(2,26);
    rejects([&] { s.beginRows(26); });s.beginRows(27);
    s.multiply(0,1,3);s.multiply(1,2,4);
    rejects([&] { s.multiply(0,3,5); });
    rejects([&] { s.subtract(0,1,5); });
    std::array<double,5> values{};s.multiplied(1,4,values);
    assert(s.subSlot()==-1);s.multiplied(0,3,values);s.subtract(0,1,5);
    s.cancel();rejects([&] { s.bind(8,10,30); });
    assert(!s.subtracted(1,5,34));s.bind(8,10,35);
    s.pivot(6,36);s.cancel();rejects([&] { s.bind(9,11,40); });
    assert(!s.reciprocal(6,48));s.bind(9,11,49);
    std::mt19937_64 rng(21987);std::uniform_real_distribution<double> dist(-2.0,2.0);
    for(unsigned n=0;n<3000;++n) {
        Matrix d{};
        for(auto &v:d)v=dist(rng);
        for(unsigned i=0;i<5;++i)d[5*i+i]+=16.0;
        const auto a=reference(d),b=run(d,n),fast=run(d,n,true);
        assert(std::memcmp(a.data(),b.data(),sizeof(a))==0);
        assert(std::memcmp(a.data(),fast.data(),sizeof(a))==0);
    }
    assert(CfdLuRank1::reservation(4)==2176 && sizeof(CfdLuRank1)<=512);
    std::cout << "LU_RANK1_MODEL_PASS matrices=3000 control_variants=2 bitwise=1 mul=40 sub=30 div=5 bytes=2176\n";
}
