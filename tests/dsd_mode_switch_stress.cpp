#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <thread>
#include <core.h>
#include <gui/gui.h>
#include <dsp/channel/frequency_xlator.h>
#include <dsp/buffer/reshaper.h>
#include <dsp/sink/handler_sink.h>
#include <utils/optionlist.h>
#include "demod.h"

static std::unique_ptr<demod::Demodulator> makeMode(int mode) {
    switch (mode) {
    case 0: return std::make_unique<demod::NFM>();
    case 1: return std::make_unique<demod::WFM>();
    case 2: return std::make_unique<demod::AM>();
    case 3: return std::make_unique<demod::DSB>();
    case 4: return std::make_unique<demod::RAW>();
    case 5: return std::make_unique<demod::LSB>();
    case 6: return std::make_unique<demod::USB>();
    case 7: return std::make_unique<demod::CW>();
    case 8: return std::make_unique<demod::CWR>();
    case 9: return std::make_unique<demod::DSD>();
    case 10: return std::make_unique<demod::OldDSD>();
    }
    std::abort();
}

int main() {
    ConfigManager config;
    config.conf = nlohmann::json::object();
    dsp::stream<dsp::complex_t> input;
    dsp::stream<dsp::stereo_t> dummy;
    dsp::sink::Handler<dsp::stereo_t> audio;
    std::atomic<int> buffers{0};
    audio.init(&dummy, [](dsp::stereo_t*, int, void* ctx) {
        (*static_cast<std::atomic<int>*>(ctx))++;
    }, &buffers);
    audio.start();
    std::thread producer([&] {
        for (;;) {
            for (int i = 0; i < 512; ++i) input.writeBuf[i] = {0.5f, 0.0f};
            if (!input.swap(512)) break;
        }
    });
    std::unique_ptr<demod::Demodulator> current;
    int switches = 0;
    auto select = [&](int mode) {
        auto next = makeMode(mode);
        assert(std::isfinite(next->getDefaultBandwidth()));
        assert(next->getDefaultBandwidth() > 0);
        // Match RadioModule's prepare-new, detach-audio, stop-old sequence.
        next->init("Radio test", &config, &input, next->getDefaultBandwidth(), 48000.0);
        audio.setInput(&dummy);
        if (current) current->stop();
        current = std::move(next);
        audio.setInput(current->getOutput());
        current->start();
        ++switches;
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    };
    for (int round = 0; round < 2; ++round) {
        for (int from = 0; from < 11; ++from) {
            for (int to = 0; to < 11; ++to) {
                if (from == to) continue;
                select(from);
                select(to);
            }
        }
    }
    audio.setInput(&dummy);
    current->stop();
    current.reset();
    audio.stop();
    input.stopWriter();
    producer.join();
    assert(buffers.load() > 0);
    std::printf("OK: %d live switches; all 110 directed mode pairs twice; %d audio buffers\n", switches, buffers.load());
}
