#include <array>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <random>
#include "arch/arm/cfd_rhs_fma.hh"
#include "arch/arm/cfd_rhs_upper.hh"
using namespace gem5::ArmISA;
using Matrix=std::array<double,25>;
using Vector=std::array<double,5>;
struct CfdRhsUpperInverse : CfdRhsUpper::Slot {
    static unsigned slot(unsigned i,unsigned j){return CfdRhsUpper::index(i,j);}
    double get(const Matrix &lu,unsigned i,unsigned j) const {return CfdRhsUpper::get(*this,lu,i,j);}
    void prepare(const Matrix &lu,unsigned &fmas) {
        fmas=0;done=0;
        for(unsigned j=2;j<5;++j)for(int i=int(j)-2;i>=0;--i) {
            double v=lu[5*i+j];
            for(unsigned k=j-1;k>unsigned(i);--k){v=CfdRhsFma::evaluate(lu[5*i+k],get(lu,k,j),v);++fmas;}
            value[slot(i,j)]=v;done|=1u<<slot(i,j);
        }
    }
    void apply(const Matrix &lu,Vector &y) const {
        for(unsigned j=1;j<5;++j){const double scalar=y[j];for(unsigned i=0;i<j;++i)y[i]=CfdRhsFma::evaluate(get(lu,i,j),scalar,y[i]);}
    }
};
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
    assert(CfdRhsUpper::reservation(16)==4352);
    CfdRhsUpper owners;owners.slots.resize(16);
    for(unsigned i=0;i<16;++i)assert(owners.allocate(i+1,9,i,0)==int(i));
    assert(owners.allocate(99,9,99,0)==-1 && owners.peak==16);
    for(unsigned i=0;i<16;++i){auto &s=owners.slots[i];s.cancelled=true;owners.release(i,s.id);}
    assert(!owners.live && owners.allocate(99,10,99,0)==0 && owners.slots[0].id==17);
    owners.slots[0].cancelled=true;owners.release(0,17);

    std::mt19937_64 rng(0x55494e56524853ULL);
    std::uniform_real_distribution<double> random(-.35,.35);
    long double error=0,residual=0,inverse_residual=0;
    uint64_t precompute_fmas=0;
    for(unsigned sample=0;sample<10000;++sample) {
        Matrix a{};
        for(unsigned i=0;i<5;++i) {
            double sum=0;
            for(unsigned j=0;j<5;++j)if(i!=j){a[5*i+j]=random(rng);sum+=std::abs(a[5*i+j]);}
            a[5*i+i]=2+sum;
        }
        const auto lu=factor(a);
        CfdRhsUpperInverse v;unsigned count=0;v.prepare(lu,count);
        assert(count==10);precompute_fmas+=count;
        for(unsigned i=0;i<5;++i)for(unsigned j=0;j<5;++j) {
            long double product=0;
            for(unsigned k=0;k<5;++k) {
                const long double u=i==k ? 1 : i<k ? lu[5*i+k] : 0;
                const long double t=k==j ? 1 : k<j ? -v.get(lu,k,j) : 0;
                product+=u*t;
            }
            inverse_residual=std::max(inverse_residual,fabsl(product-(i==j)));
        }
        for(unsigned rhs=0;rhs<11;++rhs) {
            Vector input{};for(auto &x:input)x=random(rng);
            Vector y=input;
            for(unsigned k=0;k<5;++k) {
                volatile double norm=y[k]*(1.0/lu[5*k+k]);y[k]=norm;
                for(unsigned i=k+1;i<5;++i)y[i]=CfdRhsFma::evaluate(lu[5*i+k],y[k],y[i]);
            }
            v.apply(lu,y);
            const auto exact=reference(a,input);
            for(unsigned i=0;i<5;++i) {
                assert(std::isfinite(y[i]));
                error=std::max(error,fabsl(y[i]-exact[i])/(1+fabsl(exact[i])));
                long double r=-input[i],scale=1+fabsl(input[i]);
                for(unsigned j=0;j<5;++j){r+=a[5*i+j]*static_cast<long double>(y[j]);scale+=fabsl(a[5*i+j]*static_cast<long double>(y[j]));}
                residual=std::max(residual,fabsl(r)/scale);
            }
        }
    }
    assert(error<1e-13L && residual<1e-13L && inverse_residual<1e-13L);
    std::cout<<std::setprecision(18)<<"UPPER_SHARED_NUMERIC_PASS matrices=10000 rhs=110000 precompute_fmas="
        <<precompute_fmas<<" stored_values=6 max_scaled_error="<<error
        <<" max_scaled_residual="<<residual<<" upper_inverse_residual="<<inverse_residual<<"\n";
}
