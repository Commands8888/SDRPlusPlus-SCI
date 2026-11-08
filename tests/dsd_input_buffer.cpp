#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include "dsd.h"
int main(int argc, char** argv) {
    const int packetSize = argc > 1 ? std::atoi(argv[1]) : 120;
    assert(packetSize > 0 && packetSize <= 241);
    dsp::stream<short> input;
    auto decoder = std::make_unique<dsp::DSD>(&input);
    std::atomic<int> consumed{0};
    std::thread output([&] {
        while (decoder->out.read() >= 0) { decoder->out.flush(); consumed++; }
    });
    decoder->start();
    for (int block = 0; block < (packetSize > 240 ? 1 : 8); ++block) {
        for (int i = 0; i < packetSize; ++i) input.writeBuf[i] = 10000;
        if (!input.swap(packetSize)) break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    decoder->stop();
    decoder->out.stopReader();
    output.join();
    if (packetSize <= 240) assert(consumed.load() > 0);
    std::printf("OK: legacy input packet size %d; %d output buffers\n", packetSize, consumed.load());
}

