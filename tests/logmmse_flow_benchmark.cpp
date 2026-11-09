// Sustained full-bandwidth NR: report deadlines and allocation churn after
// the noise history is full, rather than only an average during warmup.
#include "logmmse.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <new>
#include <time.h>

static bool tracking = false;
static size_t allocationCount = 0, allocationBytes = 0;
void* operator new(std::size_t n) {
    if (tracking) { ++allocationCount; allocationBytes += n; }
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
static double threadSeconds() {
    timespec t{}; clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t);
    return t.tv_sec+t.tv_nsec*1e-9;
}
int main() {
    using dsp::arrays::ComplexArray;
    using MMSE=dsp::logmmse::LogMMSE;
    uint32_t random=179;
    uint64_t hash=14695981039346656037ULL;
    auto noise=[&] { random^=random<<13; random^=random>>17; random^=random<<5; return (int32_t)random/2147483648.0f*0.04f; };
    for (int rate : {192000,1000000,2400000}) {
        MMSE::SavedParamsC params;
        ComplexArray input(rate*28/100);
        for (auto& x:input) x={noise(),noise()};
        MMSE::logmmse_sample(input,rate,&params,12);
        input.resize(params.Slen+params.len2);
        ComplexArray fixedOutput(params.len2);
        std::vector<double> timings;
        double cpu=0; size_t calls=0,bytes=0;
        for (int frame=0;frame<500;++frame) {
            for (size_t i=0;i<input.size();++i) {
                float phase=2.0*M_PI*1700.0*(frame*params.len2+i)/rate;
                input[i]={0.1f*std::cos(phase)+noise(),0.1f*std::sin(phase)+noise()};
            }
            tracking=frame>=220; allocationCount=0; allocationBytes=0;
            auto start=std::chrono::steady_clock::now();
            double c0=threadSeconds();
#ifdef SDRPP_NR_DIRECT_BENCH
            int outputSize=MMSE::processInto(input.data(),input.size(),&params,fixedOutput.data(),fixedOutput.size());
            const auto* outputData=fixedOutput.data();
#else
            auto output=MMSE::logmmse_all(input,&params);
            int outputSize=output.size();
            const auto* outputData=output.data();
#endif
            double elapsed=threadSeconds()-c0;
            double micros=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count();
            tracking=false;
            if(frame>=220) { cpu+=elapsed; timings.push_back(micros); calls+=allocationCount; bytes+=allocationBytes; }
            // Untimed hash also prevents the optimizer eliminating output.
            auto raw=(const unsigned char*)outputData;
            for(size_t i=0;i<outputSize*sizeof(outputData[0]);++i) hash=(hash^raw[i])*1099511628211ULL;
        }
        std::sort(timings.begin(),timings.end());
        std::printf("rate=%d cpu_ms_per_signal_second=%.3f p50_us=%.1f p95_us=%.1f p99_us=%.1f max_us=%.1f allocations_per_frame=%.2f allocation_bytes_per_frame=%.0f hash=%016llx\n",rate,
            cpu*1000/(timings.size()*params.len2/(double)rate),timings[timings.size()/2],timings[timings.size()*95/100],timings[timings.size()*99/100],timings.back(),
            calls/(double)timings.size(),bytes/(double)timings.size(),(unsigned long long)hash);
    }
}
