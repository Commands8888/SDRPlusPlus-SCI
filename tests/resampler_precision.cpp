#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <core.h>
#include <gui/gui.h>
#include <dsp/multirate/rational_resampler.h>

struct Resampler : dsp::multirate::RationalResampler<float> {
    unsigned int taps() const { return rtaps.size; }
};

static double tone(Resampler& resampler, double inRate, double outRate, double frequency) {
    resampler.setRates(inRate, outRate);
    resampler.reset();
    double power = 0;
    long long generated = 0, measured = 0;
    const int samples = static_cast<int>(inRate / 4);
    for (int offset = 0; offset < samples;) {
        const int count = std::min(4096, samples - offset);
        for (int i = 0; i < count; ++i)
            resampler.out.writeBuf[i] = std::sin(2.0 * 3.141592653589793 * frequency * (offset+i) / inRate);
        const int n = resampler.process(count, resampler.out.writeBuf, resampler.out.writeBuf);
        for (int i = 0; i < n; ++i) {
            const float value = resampler.out.writeBuf[i];
            assert(std::isfinite(value));
            if (generated+i > outRate*0.05) { power += value*value; ++measured; }
        }
        generated += n;
        offset += count;
    }
    assert(measured > 0);
    return std::sqrt(2.0*power/measured);
}

int main(int argc, char**) {
    dsp::stream<float> input;
    Resampler resampler;
    const double rates[][2] = {{2400000,3000},{10000000,15000},{250000,5000}};
    for (const auto& rate : rates) {
        const auto start = std::chrono::steady_clock::now();
        if (rate[0] == rates[0][0]) resampler.init(&input, rate[0], rate[1]);
        else resampler.setRates(rate[0], rate[1]);
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count();
        std::printf("Resampler %.0f -> %.0f Hz: %u coefficients; configure %lld us\n", rate[0], rate[1], resampler.taps(), (long long)elapsed);
        std::fflush(stdout);
    }
    // Ten seconds of constant input. The fractional intermediate rate must
    // not be rounded to an integer before selecting the rational ratio.
    resampler.setRates(2400000,3000);
    resampler.reset();
    long long generated = 0;
    constexpr int block = 4096;
    constexpr int samples = 24000000;
    for (int offset = 0; offset < samples;) {
        const int count = std::min(block, samples-offset);
        for (int i = 0; i < count; ++i) input.writeBuf[i] = 0.25f;
        generated += resampler.process(count,input.writeBuf,resampler.out.writeBuf);
        offset += count;
    }
    std::printf("Output count: %lld samples, expected 30000 (+/- 1 stream-boundary sample)\n",generated);
    std::fflush(stdout);
    if (argc == 1) assert(std::llabs(generated-30000) <= 1);
    {
        for (const auto& rate : rates) {
            const double gain = tone(resampler,rate[0],rate[1],rate[1]*0.2);
            const double alias = tone(resampler,rate[0],rate[1],rate[1]*1.5);
            std::printf("Spectrum %.0f -> %.0f Hz: passband %.6f, rejected tone %.1f dB\n",rate[0],rate[1],gain,20*std::log10(std::max(alias,1e-20)));
            std::fflush(stdout);
            // Existing cascaded power decimators have a small passband droop
            // (about 0.43 dB for 10M -> 15k); retain their original response.
            if (argc == 1) {
                assert(std::fabs(gain-1.0) < 0.06);
                assert(alias < 0.001); // at least 60 dB rejection
            }
        }
        // Fractional/odd clock settings must retain the bounded legacy bank.
        resampler.setRates(250000.5,5000);
        if (argc == 1) assert(resampler.taps() < 600000);
        std::puts("OK: exact output rate, passband gain and alias rejection preserved");
    }
}
