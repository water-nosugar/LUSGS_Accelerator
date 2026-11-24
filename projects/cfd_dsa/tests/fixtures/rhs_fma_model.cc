// Numerical and single-RHS dependency/RF prototype, not a gem5 speed result.
#include <array>
#include <cassert>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <random>
#include "arch/arm/cfd_register_ports.hh"
#include "arch/arm/cfd_rhs_fma.hh"
using namespace gem5::ArmISA;
using Matrix=std::array<double,25>;
using Vector=std::array<double,5>;
static Matrix factor(const Matrix &a) {
    Matrix lu{};
    for(unsigned k=0;k<5;++k) {
        for(unsigned i=k;i<5;++i) {
            double v=a[5*i+k];
            for(unsigned j=0;j<k;++j) {
                volatile double p=lu[5*i+j]*lu[5*j+k];
                volatile double s=v-p;v=s;
            }
            lu[5*i+k]=v;
        }
        const double reciprocal=1.0/lu[5*k+k];
        for(unsigned j=k+1;j<5;++j) {
            double v=a[5*k+j];
            for(unsigned i=0;i<k;++i) {
                volatile double p=lu[5*k+i]*lu[5*i+j];
                volatile double s=v-p;v=s;
            }
            volatile double u=v*reciprocal;lu[5*k+j]=u;
        }
    }
    return lu;
}
static Vector solve(const Matrix &lu,Vector b,bool fused,uint64_t &cycles) {
    CfdRegisterPorts rf;rf.configure({24,12});
    CfdRegisterPorts productBank;productBank.configure({1,1});
    cycles=0;unsigned reads=0,writes=0,fmas=0,multiplies=0;
    auto op=[&](unsigned r,unsigned w,unsigned latency,bool localResult=false,bool localRead=false) {
        if(localRead)assert(productBank.reserveReads(cycles,1));
        assert(rf.reserveReads(cycles,r));reads+=r;
        cycles+=latency;
        if(localResult)assert(productBank.reserveWrite(cycles-1,1));
        else {assert(rf.reserveWrite(cycles,w));writes+=w;}
    };
    for(unsigned k=0;k<5;++k) {
        op(2,1,3);++multiplies;
        volatile double normalized=b[k]*(1.0/lu[5*k+k]);b[k]=normalized;
        if(k==4)continue;
        unsigned width=4-k;
        if(fused) {
            op(CfdRhsFma::reads(width,true),width,CfdRhsFma::Latency);
            for(unsigned i=k+1;i<5;++i)b[i]=CfdRhsFma::evaluate(lu[5*i+k],b[k],b[i]);
            fmas+=width;
        } else {
            // Existing local product write includes its one-cycle publication.
            op(width+1,width,3+1,true);op(width,width,4,false,true);
            for(unsigned i=k+1;i<5;++i) {
                volatile double p=lu[5*i+k]*b[k];
                volatile double s=b[i]-p;b[i]=s;
            }
        }
    }
    for(int k=3;k>=0;--k) {
        unsigned width=4-k;
        if(!fused)op(2*width,width,3+1,true);
        for(unsigned j=k+1;j<5;++j) {
            if(fused) {
                op(3,1,CfdRhsFma::Latency);++fmas;
                b[k]=CfdRhsFma::evaluate(lu[5*k+j],b[j],b[k]);
            } else {
                op(1,1,4,false,true);
                volatile double p=lu[5*k+j]*b[j];
                volatile double s=b[k]-p;b[k]=s;
            }
        }
    }
    if(fused)assert(cycles==85 && reads==64 && writes==25 && fmas==20 && multiplies==5);
    else assert(cycles==103);
    return b;
}
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
int main() {
    assert(CfdRhsFma::reservation(5)==5056);
    const double a=1+std::ldexp(1.0,-27),b=1-std::ldexp(1.0,-27);
    volatile double product=a*b;
    assert(1-product==0 && CfdRhsFma::evaluate(a,b,1)==std::ldexp(1.0,-54));
    std::mt19937_64 generator(0x4c554647);std::uniform_real_distribution<double> random(-.125,.125);
    long double error=0,residual=0;
    for(unsigned sample=0;sample<10000;++sample) {
        Matrix matrix{};
        for(unsigned i=0;i<5;++i) {
            double sum=0;
            for(unsigned j=0;j<5;++j)if(i!=j){matrix[5*i+j]=random(generator);sum+=std::abs(matrix[5*i+j]);}
            matrix[5*i+i]=2+sum;
        }
        Matrix lu=factor(matrix);
        for(unsigned rhs=0;rhs<11;++rhs) {
            Vector input{};for(auto &x:input)x=random(generator);
            uint64_t old_cycles,new_cycles;
            auto original=solve(lu,input,false,old_cycles);
            auto result=solve(lu,input,true,new_cycles);
            auto exact=reference(matrix,input);
            for(unsigned i=0;i<5;++i) {
                error=std::max(error,fabsl(result[i]-exact[i])/(1+fabsl(exact[i])));
                assert(std::isfinite(original[i]) && std::isfinite(result[i]));
                long double r=-input[i],scale=1+fabsl(input[i]);
                for(unsigned j=0;j<5;++j){r+=matrix[5*i+j]*static_cast<long double>(result[j]);scale+=fabsl(matrix[5*i+j]*static_cast<long double>(result[j]));}
                residual=std::max(residual,fabsl(r)/scale);
            }
        }
    }
    assert(error<1e-13L && residual<1e-13L);
    std::cout<<std::setprecision(18)<<"PROTOTYPE_NUMERIC_PASS matrices=10000 rhs=110000 max_scaled_error="
        <<error<<" max_scaled_residual="<<residual<<"\n";
    std::cout<<"READY_LU_SINGLE_RHS_COMPONENT old=103 fma=85 rf_reads=64 rf_writes=25 fma_lanes=5 lat=5 ii=1 extra_bytes=5056; not end-to-end speedup\n";
}
