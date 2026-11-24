#include "arch/arm/cfd_rhs_upper_stream.hh"
#include <array>
#include <algorithm>
#include <vector>
#include "arch/arm/cfd_register_ports.hh"
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <random>
// Numerical prototype only. No native gem5 timing/resource path uses this yet.
#ifndef CFD_RHS_UPPER_INVERSE_PROTOTYPE_HH
#define CFD_RHS_UPPER_INVERSE_PROTOTYPE_HH
#include <array>
#include <cassert>
#include "arch/arm/cfd_rhs_fma.hh"
namespace gem5 { namespace ArmISA {
struct CfdRhsUpperInverse {
    // Store V_ij=-(U^-1)_ij only for the six nonadjacent entries. Adjacent
    // entries alias U_ij; the unit diagonal needs neither storage nor work.
    std::array<double,6> value{};
    static unsigned slot(unsigned i,unsigned j) {
        assert(i+1<j && j<5);
        return i==0 ? j-2 : i==1 ? j : 5;
    }
    double get(const std::array<double,25> &lu,unsigned i,unsigned j) const {
        assert(i<j && j<5);
        return j==i+1 ? lu[5*i+j] : value[slot(i,j)];
    }
    void prepare(const std::array<double,25> &lu,unsigned &fmas) {
        fmas=0;
        for(unsigned j=2;j<5;++j)
            for(int i=int(j)-2;i>=0;--i) {
                double v=lu[5*i+j];
                // Reverse k makes the dataflow depth three FMA latencies
                // after U is ready, independent of the numerical loop order.
                for(unsigned k=j-1;k>unsigned(i);--k) {
                    v=CfdRhsFma::evaluate(lu[5*i+k],get(lu,k,j),v);
                    ++fmas;
                }
                value[slot(i,j)]=v;
            }
    }
    void apply(const std::array<double,25> &lu,std::array<double,5> &y) const {
        // Updates affect only i<j. The broadcast y_j remains unchanged until
        // its column has been consumed, so no additional RHS vector is needed.
        for(unsigned j=1;j<5;++j) {
            const double scalar=y[j];
            for(unsigned i=0;i<j;++i)
                y[i]=CfdRhsFma::evaluate(get(lu,i,j),scalar,y[i]);
        }
    }
};
} }
#endif

using namespace gem5::ArmISA;
using Matrix=std::array<double,25>;
using Vector=std::array<double,5>;
struct LuWork {uint64_t divide=0,normalize=0,fma=0;};
static Matrix factor_fused(const Matrix &a,LuWork &work) {
    Matrix lu=a;
    for(unsigned c=0;c<5;++c) {
        const double reciprocal=1.0/lu[5*c+c];
        ++work.divide;
        for(unsigned j=c+1;j<5;++j) {
            volatile double value=lu[5*c+j]*reciprocal;lu[5*c+j]=value;
            ++work.normalize;
        }
        for(unsigned row=c+1;row<5;++row)
            for(unsigned j=c+1;j<5;++j) {
                lu[5*row+j]=CfdRhsFma::evaluate(lu[5*row+c],lu[5*c+j],lu[5*row+j]);
                ++work.fma;
            }
    }
    return lu;
}
static Matrix factor_separate(const Matrix &a) {
    Matrix lu=a;
    for(unsigned c=0;c<5;++c) {
        const double reciprocal=1.0/lu[5*c+c];
        for(unsigned j=c+1;j<5;++j) {
            volatile double value=lu[5*c+j]*reciprocal;lu[5*c+j]=value;
        }
        for(unsigned row=c+1;row<5;++row)for(unsigned j=c+1;j<5;++j) {
            volatile double product=lu[5*row+c]*lu[5*c+j];
            volatile double value=lu[5*row+j]-product;lu[5*row+j]=value;
        }
    }
    return lu;
}
static uint64_t bits(double value) {uint64_t out;std::memcpy(&out,&value,8);return out;}

static std::array<long double,5> reference(const Matrix &a,const Vector &b) {
    long double m[5][6]{};
    for(unsigned i=0;i<5;++i) {
        for(unsigned j=0;j<5;++j)m[i][j]=a[5*i+j];
        m[i][5]=b[i];
    }
    for(unsigned k=0;k<5;++k) {
        unsigned pivot=k;
        for(unsigned i=k+1;i<5;++i)if(fabsl(m[i][k])>fabsl(m[pivot][k]))pivot=i;
        for(unsigned j=k;j<6;++j)std::swap(m[k][j],m[pivot][j]);
        for(unsigned i=k+1;i<5;++i) {
            long double scale=m[i][k]/m[k][k];
            for(unsigned j=k;j<6;++j)m[i][j]-=scale*m[k][j];
        }
    }
    std::array<long double,5> x{};
    for(int i=4;i>=0;--i) {
        long double v=m[i][5];for(unsigned j=i+1;j<5;++j)v-=m[i][j]*x[j];
        x[i]=v/m[i][i];
    }
    return x;
}

// Component experiment only: one RHS, ready LU/upper coefficients, no other
// contexts. All operands and actual returns still pay finite RF/FU credits.
#include "arch/arm/cfd_register_ports.hh"
#include <vector>
struct StreamResult {Vector value;unsigned cycles=0,fmas=0,norms=0,reads=0,writes=0,peak=0;};
static StreamResult solve_stream(const Matrix &lu,const CfdRhsUpperInverse &upper,
                                const Vector &input,bool overlap) {
    StreamResult out{input};
    CfdRegisterPorts rf;rf.configure({32,16});
    std::array<uint64_t,10> mul{},fma{};
    struct Event {uint64_t ready,id;unsigned kind,k,width;Vector result;};
    std::vector<Event> events;
    unsigned fw=0,back=1,normalized=0,live=0;
    bool normalize=true,fwWaiting=false,backWaiting=false,fwDone=false;
    uint64_t identity=0;
    auto reserve=[&](uint64_t now,unsigned kind,unsigned width,unsigned reads) {
        auto &lanes=kind==0 ? mul : fma;
        const unsigned available=std::count_if(lanes.begin(),lanes.end(),[now](auto t){return t<=now;});
        if(available<width || (kind && live+width>50) || !rf.reserveReads(now,reads))return false;
        unsigned used=0;for(auto &t:lanes)if(t<=now && used<width){t=now+1;++used;}
        out.reads+=reads;
        if(kind){live+=width;out.peak=std::max(out.peak,live);out.fmas+=width;}
        else out.norms+=width;
        return true;
    };
    for(uint64_t now=0;now<200;++now) {
        unsigned destinations=0;
        for(size_t i=0;i<events.size();) {
            const auto event=events[i];
            if(event.ready>now || !rf.reserveWrite(now,event.width)){++i;continue;}
            events.erase(events.begin()+i);out.writes+=event.width;
            if(event.kind==0) {
                assert(!(destinations&(1u<<event.k)));destinations|=1u<<event.k;
                out.value[event.k]=event.result[event.k];normalized|=1u<<event.k;
                fwWaiting=false;
                if(event.k==4)fwDone=true;else normalize=false;
            }else {
                live-=event.width;
                const unsigned first=event.kind==1 ? event.k+1 : 0;
                for(unsigned row=first;row<first+event.width;++row) {
                    assert(!(destinations&(1u<<row)));destinations|=1u<<row;
                    out.value[row]=event.result[row];
                }
                if(event.kind==1){++fw;normalize=true;fwWaiting=false;}
                else {++back;backWaiting=false;}
            }
        }
        if(fwDone && back==5) {
            assert(events.empty() && !live && normalized==31 && out.fmas==20 && out.norms==5);
            assert(out.reads==58 && out.writes==25 && out.peak<=50);
            out.cycles=now;return out;
        }
        if(!fwDone && !fwWaiting) {
            if(normalize) {
                if(reserve(now,0,1,2)) {
                    Vector result{};volatile double value=out.value[fw]*(1.0/lu[5*fw+fw]);
                    result[fw]=value;events.push_back({now+3,++identity,0,fw,1,result});fwWaiting=true;
                }
            }else {
                const unsigned width=4-fw;
                if(reserve(now,1,width,2*width+1)) {
                    Vector result{};
                    for(unsigned row=fw+1;row<5;++row)
                        result[row]=CfdRhsFma::evaluate(lu[5*row+fw],out.value[fw],out.value[row]);
                    events.push_back({now+5,++identity,1,fw,width,result});fwWaiting=true;
                }
            }
        }
        if(!backWaiting && back<5 && (overlap || fwDone) &&
           CfdRhsUpperStream::ready(back,normalized,backWaiting)) {
            if(reserve(now,2,back,2*back+1)) {
                Vector result{};
                for(unsigned row=0;row<back;++row)
                    result[row]=CfdRhsFma::evaluate(upper.get(lu,row,back),out.value[back],out.value[row]);
                events.push_back({now+5,++identity,2,back,back,result});backWaiting=true;
            }
        }
    }
    assert(false);return {};
}

int main() {
    static_assert(CfdRhsUpperStream::ControlBytes==512);
    assert(!CfdRhsUpperStream::ready(1,1,false));
    assert(CfdRhsUpperStream::ready(1,3,false));
    assert(!CfdRhsUpperStream::ready(1,3,true));
    assert(CfdRhsUpperStream::ready(4,31,false));
    assert(!CfdRhsUpperStream::ready(5,31,false));

    std::mt19937_64 rng(0x5550504552535452ULL);
    std::uniform_real_distribution<double> random(-.35,.35);
    long double error=0,residual=0;
    unsigned legacy_cycles=0,stream_cycles=0;
    uint64_t solves=0;LuWork work;
    for(bool fused:{false,true})for(unsigned sample=0;sample<10000;++sample) {
        Matrix a{};
        for(unsigned i=0;i<5;++i) {
            double sum=0;
            for(unsigned j=0;j<5;++j)if(i!=j){a[5*i+j]=random(rng);sum+=std::abs(a[5*i+j]);}
            a[5*i+i]=2+sum;
        }
        const auto lu=fused ? factor_fused(a,work) : factor_separate(a);
        CfdRhsUpperInverse upper;unsigned count=0;upper.prepare(lu,count);assert(count==10);
        for(unsigned rhs=0;rhs<11;++rhs) {
            Vector input{};for(auto &v:input)v=random(rng);
            const auto legacy=solve_stream(lu,upper,input,false),stream=solve_stream(lu,upper,input,true);
            assert(legacy.cycles==55 && stream.cycles==40);
            legacy_cycles=legacy.cycles;stream_cycles=stream.cycles;
            assert(legacy.reads==stream.reads && legacy.writes==stream.writes);
            for(unsigned i=0;i<5;++i)assert(bits(legacy.value[i])==bits(stream.value[i]));
            const auto exact=reference(a,input);++solves;
            for(unsigned i=0;i<5;++i) {
                error=std::max(error,fabsl(stream.value[i]-exact[i])/(1+fabsl(exact[i])));
                long double r=-input[i],scale=1+fabsl(input[i]);
                for(unsigned j=0;j<5;++j){r+=a[5*i+j]*static_cast<long double>(stream.value[j]);scale+=fabsl(a[5*i+j]*static_cast<long double>(stream.value[j]));}
                residual=std::max(residual,fabsl(r)/scale);
            }
        }
    }
    assert(solves==220000 && error<1e-13L && residual<1e-13L);
    std::cout<<std::setprecision(18)<<"UPPER_STREAM_PROTOTYPE_PASS matrices=20000 rhs="<<solves
        <<" LU_modes=2 bitwise_same=1 legacy_component_cycles="<<legacy_cycles
        <<" stream_component_cycles="<<stream_cycles<<" fma_per_rhs=20 norm_per_rhs=5 rf_reads=58 rf_writes=25"
        <<" max_scaled_error="<<error<<" max_scaled_residual="<<residual<<"\n";
}
