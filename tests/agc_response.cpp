#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <dsp/loop/agc.h>

template<class T> void check() {
    dsp::stream<T> input;
    dsp::loop::AGC<T> agc(&input, 0.5, 0.1, 0.01, 100.0, 1.0, 1.0);
    std::array<T, 256> in{}, out{};
    auto fill = [&](float level) {
        for (auto& value : in) {
            if constexpr (std::is_same_v<T, float>) value = level;
            else value = {level * 0.6f, level * 0.8f};
        }
    };
    auto amplitude = [](T value) {
        if constexpr (std::is_same_v<T, float>) return std::fabs(value);
        else return value.amplitude();
    };
    fill(0.01f);
    for (int i = 0; i < 40; ++i) agc.process(in.size(), in.data(), out.data());
    assert(std::fabs(amplitude(out.back()) - 0.5f) < 0.001f);
    assert(std::fabs(agc.getGain() - 50.0f) < 0.1f);
    // A sudden stronger carrier is bounded and reaches the same set point.
    fill(0.9f);
    agc.process(in.size(), in.data(), out.data());
    for (auto value : out) assert(amplitude(value) <= 1.00001f);
    for (int i = 0; i < 10; ++i) agc.process(in.size(), in.data(), out.data());
    assert(std::fabs(amplitude(out.back()) - 0.5f) < 0.001f);
    agc.setEnabled(false);
    agc.setGain(2.0f);
    fill(0.2f);
    agc.process(in.size(), in.data(), out.data());
    assert(std::fabs(amplitude(out.back()) - 0.4f) < 0.00001f);
    fill(0.9f);
    agc.process(in.size(), in.data(), out.data());
    assert(std::fabs(amplitude(out.back()) - 1.0f) < 0.00001f);
    agc.setEnabled(true);
    agc.reset();
    fill(0);
    agc.process(in.size(), in.data(), out.data());
    for (auto value : out) assert(amplitude(value) == 0.0f);
    assert(std::isfinite(agc.getGain()));
    fill(0.1f);
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 100000; ++i) agc.process(in.size(), in.data(), out.data());
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count();
    std::printf("OK: %s AGC set point, attack, recovery, manual gain, limiter and silence; 25.6M samples in %lld us\n",
        std::is_same_v<T,float> ? "real" : "complex", (long long)us);
}

int main() { check<float>(); check<dsp::complex_t>(); }
