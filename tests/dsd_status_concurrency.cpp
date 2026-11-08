#include <atomic>
#include <cassert>
#include <cstdio>
#include <memory>
#include <thread>
#include "dsd.h"

static void newDecoderStatus() {
    dsp::stream<uint8_t> input;
    auto decoder = std::make_unique<dsp::NewDSD>(&input);
    std::atomic<bool> started{false}, done{false};
    std::thread observer([&] {
        started = true;
        while (!done.load()) {
            auto frame = decoder->getFrameSyncStatus();
            auto mbe = decoder->getMBEStatus();
            auto dmr = decoder->getDMRStatus();
            auto p25 = decoder->getP25Status();
            (void)frame; (void)mbe; (void)dmr; (void)p25;
        }
    });
    while (!started.load()) std::this_thread::yield();
    uint8_t data[24];
    short output[2048];
    for (int i = 0; i < 24; ++i) data[i] = DMR_BS_DATA_SYNC[i] == '1' ? 1 : 3;
    for (int i = 0; i < 5000; ++i) decoder->process(24, data, output);
    done = true;
    observer.join();
}

static void oldDecoderStatus() {
    dsp::stream<short> input;
    auto decoder = std::make_unique<dsp::DSD>(&input);
    std::atomic<bool> done{false};
    std::thread observer([&] {
        while (!done.load()) {
            auto status = decoder->getStatus();
            (void)status;
        }
    });
    std::thread output([&] {
        while (decoder->out.read() >= 0) decoder->out.flush();
    });
    decoder->start();
    for (int block = 0; block < 600; ++block) {
        for (int i = 0; i < 120; ++i) input.writeBuf[i] = (block + i) % 2 ? 12000 : -12000;
        if (!input.swap(120)) break;
    }
    decoder->stop();
    decoder->out.stopReader();
    output.join();
    done = true;
    observer.join();
}

int main() {
    newDecoderStatus();
    oldDecoderStatus();
    std::puts("OK: DSD and oldDSD status concurrency (ThreadSanitizer)");
}
