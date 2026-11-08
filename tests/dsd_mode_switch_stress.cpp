#include <atomic>
#include <chrono>
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
#include <dsp/sink/handler_sink.h>
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
    const int modes[] = {1, 0, 1, 2, 0, 2, 3, 4, 5};
    for (int i = 0; i < 270; ++i) {
        std::unique_ptr<demod::Demodulator> next;
        switch (modes[i % 9]) {
        case 0: next = std::make_unique<demod::CWR>(); break;
        case 1: next = std::make_unique<demod::DSD>(); break;
        case 2: next = std::make_unique<demod::OldDSD>(); break;
        case 3: next = std::make_unique<demod::NFM>(); break;
        case 4: next = std::make_unique<demod::LSB>(); break;
        case 5: next = std::make_unique<demod::USB>(); break;
        }
        // RadioModule prepares the new demodulator while the old one runs.
        next->init("Radio test", &config, &input, next->getDefaultBandwidth(), 48000.0);
        audio.setInput(&dummy);
        if (current) current->stop();
        current = std::move(next);
        std::printf("switch %d: %s\n", i, current->getName());
        std::fflush(stdout);
        audio.setInput(current->getOutput());
        current->start();
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    audio.setInput(&dummy);
    current->stop();
    current.reset();
    audio.stop();
    input.stopWriter();
    producer.join();
    std::printf("OK: 270 live mode switches; %d audio buffers\n", buffers.load());
}

