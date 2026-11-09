#include <dsp/sink/audio_buffer.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <thread>

static thread_local bool inCallback = false;
static std::atomic<int> callbackAllocations { 0 };
void* operator new(std::size_t n) {
    if (inCallback) { ++callbackAllocations; }
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

int main() {
    for (unsigned rate : {44100U,48000U,96000U}) {
        for (bool mono : {false,true}) {
            dsp::stream<dsp::stereo_t> input;
            dsp::sink::AudioBuffer audio;
            audio.init(&input);
            audio.configure(rate,rate/60);
            assert(audio.capacity() <= 8192); // bounded milliseconds, not seconds
            std::vector<dsp::stereo_t> stereo(4096);
            std::vector<float> single(4096);
            audio.start();
            auto render=[&](unsigned n) {
                inCallback = true;
                unsigned got = mono ? audio.renderMono(single.data(),n) : audio.renderStereo(stereo.data(),n);
                inCallback = false;
                return got;
            };
            assert(render(0) == 0);
            assert(render(512) == 0);
            // Irregular input packets, variable callback requests, and a final
            // short packet must preserve every sample and both channels.
            const int total=100003;
            std::thread producer([&] {
                int position=0;
                const int packets[]={1,127,8192,509,33};
                for(int iteration=0;position<total;++iteration) {
                    int n=std::min(packets[iteration%5],total-position);
                    for(int i=0;i<n;++i) input.writeBuf[i]={(float)(position+i+1),(float)(position+i+1)*0.5f};
                    if(!input.swap(n)) break;
                    position+=n;
                }
            });
            int received=0,iteration=0;
            auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(8);
            while(received<total) {
                assert(std::chrono::steady_clock::now()<deadline);
                const unsigned requests[]={127,511,1024,800,259};
                unsigned n=requests[iteration++%5];
                unsigned got=render(n);
                assert(got<=n && audio.queued()<=audio.capacity());
                for(unsigned i=0;i<n;++i) {
                    if(i<got) {
                        float value=received+i+1;
                        if(mono) assert(single[i]==value*0.75f);
                        else assert(stereo[i].l==value && stereo[i].r==value*0.5f);
                    } else {
                        if(mono) assert(single[i]==0);
                        else assert(stereo[i].l==0 && stereo[i].r==0);
                    }
                }
                received+=got;
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
            producer.join();
            audio.stop();
            assert(render(512)==0);
            // Native device is stopped here: reconfigure/restart with fresh
            // indices, including a stop while the bounded producer is full.
            audio.configure(48000,800);
            audio.start();
            std::fill_n(input.writeBuf,16384,dsp::stereo_t{7,9});
            assert(input.swap(16384));
            auto limit=std::chrono::steady_clock::now()+std::chrono::seconds(1);
            while(audio.queued()<audio.capacity()) {
                assert(std::chrono::steady_clock::now()<limit);
                std::this_thread::yield();
            }
            audio.stop();
            assert(render(512)==0);
            audio.start();
            std::fill_n(input.writeBuf,127,dsp::stereo_t{123,321});
            assert(input.swap(127));
            limit=std::chrono::steady_clock::now()+std::chrono::seconds(1);
            unsigned got=0;
            while(!got) {
                assert(std::chrono::steady_clock::now()<limit);
                got=render(800);
                std::this_thread::yield();
            }
            assert(got==127);
            for(unsigned i=0;i<got;++i) {
                if(mono) assert(single[i]==222);
                else assert(stereo[i].l==123 && stereo[i].r==321);
            }
            audio.stop();
        }
    }
    assert(callbackAllocations.load()==0);
    std::puts("OK: audio callbacks preserve samples at 44.1/48/96 kHz, mono/stereo and variable sizes; zero allocations, bounded queue, silence on starvation and clean full-queue stop");
}
