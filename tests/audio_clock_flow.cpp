#include <dsp/sink/audio_buffer.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

int main() {
    using Clock=std::chrono::steady_clock;
    for(unsigned rate : {44100u,48000u,96000u})
    for(bool mono : {false,true})
    for(unsigned batch : {1u,2u,4u}) {
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
                unsigned count=uint64_t(++tick)*rate/200-written;
                for(unsigned i=0;i<count;++i) {
                    float value=float(written+i+1);
                    input.writeBuf[i]={value,value*0.5f};
                }
                if(!input.swap(count)) break;
                written+=count;
                next+=std::chrono::milliseconds(5);
                std::this_thread::sleep_until(next);
            }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::vector<dsp::stereo_t> output(period);
        std::vector<float> single(period);
        unsigned shortCalls=0,missing=0;
        uint64_t received=0;
        auto next=Clock::now();
        for(int cycle=0;cycle<12;++cycle) {
            for(unsigned cb=0;cb<batch;++cb) {
                unsigned got=mono?audio.renderMono(single.data(),period):audio.renderStereo(output.data(),period);
                for(unsigned i=0;i<got;++i) {
                    float value=float(received+i+1);
                    if(mono) assert(single[i]==value*0.75f);
                    else assert(output[i].l==value && output[i].r==value*0.5f);
                }
                received+=got;
                if(cycle>=2) {shortCalls+=got!=period;missing+=period-got;}
            }
            next+=std::chrono::nanoseconds(uint64_t(period)*batch*1000000000/rate);
            std::this_thread::sleep_until(next);
        }
        done=true;input.stopWriter();audio.stop();source.join();
        std::printf("rate=%u mono=%d batch=%u capacity=%u short callbacks=%u missing frames=%u\n",rate,mono,batch,audio.capacity(),shortCalls,missing);
        std::fflush(stdout);
        assert(missing==0);
    }
    std::puts("OK: continuous mono/stereo audio at 44.1/48/96 kHz, 1/2/4-period device bursts, zero inserted silence and exact sample order after startup");
}
