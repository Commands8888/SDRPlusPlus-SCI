#include <atomic>
#include <array>
#include <cmath>
#include <cstdio>
#include <thread>
#include <dsp/loop/agc.h>

template<class T> void check() {
    dsp::stream<T> input;
    dsp::loop::AGC<T> agc;
    agc.init(&input, 1.0, 0.01, 0.001, 1000.0, 10.0, 1.0);
    std::atomic<bool> ready{false}, done{false};
    std::thread processing([&] {
        std::array<T, 256> in{}, out{};
        for (size_t i = 0; i < in.size(); ++i) {
            const float level = 0.1f + (i % 11) * 0.01f;
            if constexpr (std::is_same_v<T,float>) in[i] = level;
            else in[i] = {level, -level};
        }
        ready = true;
        while (!done.load()) agc.process(in.size(), in.data(), out.data());
    });
    while (!ready.load()) std::this_thread::yield();
    for (int i = 0; i < 5000; ++i) {
        const float gain = agc.getGain();
        if (!std::isfinite(gain)) std::abort();
        agc.setAttack(i % 2 ? 0.01 : 0.02);
        agc.setDecay(i % 2 ? 0.001 : 0.002);
        agc.setMaxGain(1000.0);
        agc.setSetPoint(1.0);
        agc.setMaxOutputAmp(10.0);
        agc.setInitialGain(1.0);
        if (i % 50 == 0) {
            agc.setEnabled(false);
            agc.setGain(2.0f);
            agc.setEnabled(true);
        }
    }
    done = true;
    processing.join();
    std::puts("OK: AGC processing, UI controls and gain telemetry are synchronized");
}
int main() { check<float>(); check<dsp::complex_t>(); }
