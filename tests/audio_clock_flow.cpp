#include <dsp/sink/audio_buffer.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

static void run(unsigned rate, bool mono, unsigned batch, unsigned chunkFrames) {
    using Clock=std::chrono::steady_clock;
        dsp::stream<dsp::stereo_t> input;
        dsp::sink::AudioBuffer audio;
        unsigned period=rate/60;
        audio.init(&input);audio.configure(rate,period);audio.start();
        std::atomic<bool> done{false};
        std::thread source([&] {
            auto next=Clock::now();
            uint64_t written=0;
            unsigned tick=0;
            while(!done.load()) {
                unsigned count;
                if(chunkFrames) {
                    next+=std::chrono::nanoseconds(uint64_t(chunkFrames)*1000000000/rate);
                    std::this_thread::sleep_until(next);
                    if(done.load()) break;
                    // Match the NR's shorter first output and later full blocks.
                    count=chunkFrames-(tick++==0?chunkFrames/9:0);
                }
                else count=uint64_t(++tick)*rate/200-written;
                for(unsigned i=0;i<count;++i) {
                    float value=float(written+i+1);
                    input.writeBuf[i]={value,value*0.5f};
                }
                if(!input.swap(count)) break;
                written+=count;
                if(!chunkFrames) {
                    next+=std::chrono::milliseconds(5);
                    std::this_thread::sleep_until(next);
                }
            }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::vector<dsp::stereo_t> output(period);
        std::vector<float> single(period);
        unsigned shortCalls=0,missing=0;
        uint64_t received=0;
        auto next=Clock::now();
        const auto deadline=next+std::chrono::seconds(2);
        bool ready=false;
        unsigned measured=0;
        for(int cycle=0;cycle<64 && measured<12;++cycle) {
            const bool measuring=ready;
            unsigned burstGot=0;
            for(unsigned cb=0;cb<batch;++cb) {
                unsigned got=mono?audio.renderMono(single.data(),period):audio.renderStereo(output.data(),period);
                for(unsigned i=0;i<got;++i) {
                    float value=float(received+i+1);
                    if(mono) assert(single[i]==value*0.75f);
                    else assert(output[i].l==value && output[i].r==value*0.5f);
                }
                received+=got;
                burstGot+=got;
                if(measuring) {shortCalls+=got!=period;missing+=period-got;}
            }
            if(measuring) ++measured;
            else if(burstGot==batch*period) ready=true;
            else assert(Clock::now()<deadline);
            next+=std::chrono::nanoseconds(uint64_t(period)*batch*1000000000/rate);
            std::this_thread::sleep_until(next);
        }
        done=true;input.stopWriter();audio.stop();source.join();
        std::printf("rate=%u mono=%d batch=%u DSP packet=%u capacity=%u short callbacks=%u missing frames=%u\n",rate,mono,batch,chunkFrames,audio.capacity(),shortCalls,missing);
        std::fflush(stdout);
        assert(measured==12 && missing==0);
}

int main() {
    for(unsigned rate : {44100u,48000u,96000u})
        for(bool mono : {false,true})
            for(unsigned batch : {1u,2u,4u}) run(rate,mono,batch,0);
    for(unsigned rate : {8000u,16000u,24000u,44100u,48000u,96000u})
        for(bool mono : {false,true}) run(rate,mono,4,rate<23000?1152:4608);
    std::puts("OK: continuous mono/stereo audio at 8/16/24/44.1/48/96 kHz with short or NR-sized DSP packets, device bursts, zero inserted silence and exact sample order after startup");
}
