#include "af_nr.h"
#include "if_nr.h"
#include <dsp/sink/handler_sink.h>
#include <cassert>
#include <cstdio>
#include <thread>

int main() {
    dsp::stream<dsp::complex_t> input;
    sigpath::iqFrontEnd.init(&input,192000,false,1,false,1024,10,IQFrontEnd::RECTANGULAR,
        [](void*) -> float* { return nullptr; },[](void*) {},nullptr);
    dsp::IFNRLogMMSE nr;
    nr.init(&input);
    // Two sub-frame packets must not publish zero-length output or wait for
    // a nonexistent consumer of those empty packets.
    for (int i = 0; i < 2; ++i) {
        input.writeBuf[0] = {0,0};
        assert(input.swap(1));
        assert(nr.run() == 1);
    }
    dsp::sink::Handler<dsp::complex_t> sink;
    std::atomic<int> delivered{0};
    sink.init(&nr.out,[](dsp::complex_t* samples,int count,void* ctx) {
        for (int i = 0; i < count; ++i) assert(std::isfinite(samples[i].re) && std::isfinite(samples[i].im));
        static_cast<std::atomic<int>*>(ctx)->fetch_add(count);
    },&delivered);
    sink.start();
    nr.start(); nr.start(); // start is idempotent, including event registration
    std::thread producer([&] {
        for (int block = 0; block < 96; ++block) {
            for (int i = 0; i < 8192; ++i) input.writeBuf[i] = {0.01f,0.02f};
            if (!input.swap(8192)) break;
        }
    });
    for (int i = 0; i < 600; ++i) {
        nr.setDisableCpuDeactivation(i%2);
        if (i%64 == 0) nr.reset();
        sigpath::txState.emit(i%3 == 0);
        assert(nr.percentUsage.load() >= -1);
        (void)nr.stopReason.load();
        std::this_thread::yield();
    }
    producer.join();
    nr.stop(); nr.stop();
    sink.stop();
    assert(delivered.load() > 0);
    {
        dsp::AFNR_OMLSA_MCRA audio;
        audio.allowed = true;
        std::atomic<bool> started{false};
        std::thread worker([&] {
            std::vector<dsp::stereo_t> in(8192,{0.05f,0.05f}),out(STREAM_BUFFER_SIZE);
            started = true;
            for (int i = 0; i < 150; ++i) {
                int wrote;
                audio.process(in.data(),in.size(),out.data(),wrote);
                for (int j = 0; j < wrote; ++j) assert(std::isfinite(out[j].l));
            }
        });
        while (!started.load()) std::this_thread::yield();
        for (int i = 0; i < 600; ++i) {
            audio.allowed = i%2;
            audio.setAudioSampleRate(i%3 ? 48000 : 44100);
            assert(audio.scaled.load() > 0);
            std::this_thread::yield();
        }
        worker.join();
    }
    std::puts("OK: NR controls/status/reset are synchronized; empty packets and duplicate start/stop terminate cleanly");
}
