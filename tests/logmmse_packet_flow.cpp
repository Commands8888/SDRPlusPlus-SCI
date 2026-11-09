#include "if_nr.h"
#include <cassert>
#include <cstdio>

int main() {
    constexpr int rate=2400000;
    dsp::stream<dsp::complex_t> source;
    sigpath::iqFrontEnd.init(&source,rate,false,1,false,1024,10,IQFrontEnd::RECTANGULAR,
        [](void*) -> float* { return nullptr; },[](void*) {},nullptr);
    std::vector<dsp::complex_t> input(rate*15/10),reference,output(STREAM_BUFFER_SIZE);
    uint32_t random=179;
    for(size_t i=0;i<input.size();++i) {
        auto noise=[&] { random^=random<<13;random^=random>>17;random^=random<<5;return (int32_t)random/2147483648.0f*0.03f; };
        float phase=2.0*M_PI*1700.0*i/rate;
        input[i]={0.1f*std::cos(phase)+noise(),0.1f*std::sin(phase)+noise()};
    }
    for(int packet : {8192,65536,333}) {
        dsp::IFNRLogMMSE nr;
        size_t received=0;
        double error=0,energy=0;
        for(size_t offset=0;offset<input.size();) {
            int count=std::min<size_t>(packet,input.size()-offset);
            int produced;
            nr.process(input.data()+offset,count,output.data(),produced);
            offset+=count;
            do {
                assert(produced<=rate/100);
                if(packet==8192) reference.insert(reference.end(),output.begin(),output.begin()+produced);
                else {
                    assert(received+produced<=reference.size());
                    for(int i=0;i<produced;++i) {
                        auto expected=reference[received+i];
                        auto difference=output[i]-expected;
                        assert(std::isfinite(output[i].re) && std::isfinite(output[i].im));
                        error+=difference.re*difference.re+difference.im*difference.im;
                        energy+=expected.re*expected.re+expected.im*expected.im;
                    }
                }
                received+=produced;
                if(!nr.hasPendingFrame()) break;
                nr.process(nullptr,0,output.data(),produced);
            } while(true);
            assert(nr.worker1c.size()<=static_cast<size_t>(std::max(rate*28/100+packet,packet+3*(rate/100))));
        }
        assert(received==static_cast<size_t>((input.size()/(rate/100)-2)*(rate/100)));
        assert(error<=4.0e-10*std::max(energy,1.0e-20)); // 2e-5 RMS across FFT/SIMD implementations
    }
    std::puts("OK: full-bandwidth LogMMSE output matches for 333/8192/65536-sample input packets; model advances per hop and every publication is at most 10 ms");
}
