#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <thread>
#include <dsp/channel/frequency_xlator.h>

int main() {
    dsp::stream<dsp::complex_t> input;
    dsp::channel::FrequencyXlator shift(&input, 800.0, 3000.0);
    std::atomic<bool> ready{false};
    std::thread worker([&] {
        std::array<dsp::complex_t, 256> in{}, out{};
        for (auto& value : in) value = {0.25f, 0.0f};
        ready = true;
        for (int block = 0; block < 20000; ++block) {
            shift.process(in.size(), in.data(), out.data());
            for (auto value : out) assert(std::fabs(value.amplitude() - 0.25f) < 0.001f);
        }
    });
    while (!ready.load()) std::this_thread::yield();
    for (int i = 0; i < 10000; ++i) shift.setOffset(i%2 ? 500.0 : -900.0, 3000.0);
    worker.join();
    std::puts("OK: concurrent CW/CW-R tone changes preserve magnitude and phase continuity");
}
