// Bounded, reproducible CPU/allocation comparison. Run the same executable
// source against the before/after headers; optional output is raw IQ/audio.
#include <dsp/noise_reduction/fm_if.h>
#include "logmmse.h"
#include "af_nr.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <new>
#include <string>
#include <vector>

static size_t allocations = 0;
static bool trackAllocations = false;
void* operator new(std::size_t n) {
    if (trackAllocations) { ++allocations; }
    if (void* p = std::malloc(n ? n : 1)) { return p; }
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

static uint32_t rng = 0x48da291b;
static float noise() {
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return (static_cast<int32_t>(rng) / 2147483648.0f) * 0.04f;
}
static FILE* output = nullptr;
static void emit(const void* data, size_t bytes) {
    if (output && std::fwrite(data, 1, bytes, output) != bytes) { std::abort(); }
}

int main(int argc, char** argv) {
    if (argc > 1) { output = std::fopen(argv[1], "wb"); if (!output) { return 2; } }
    dsp::stream<dsp::complex_t> stream;
    std::vector<dsp::complex_t> input(8192), result(STREAM_BUFFER_SIZE);
    double checksum = 0;
    for (auto& x : input) { x = {noise(), noise()}; }
    for (int bins : {9, 15, 31, 32}) {
        dsp::noise_reduction::FMIF filter(&stream, bins);
        auto t = std::clock();
        for (int i = 0; i < 32; ++i) {
            filter.process(input.size(), input.data(), result.data());
            checksum += result[i].re;
            emit(result.data(), input.size() * sizeof(result[0]));
        }
        std::printf("FMIF bins=%d cpu_ms=%.3f samples=262144\n", bins, 1000.0 * (std::clock()-t)/CLOCKS_PER_SEC);
    }
    using dsp::logmmse::LogMMSE;
    using dsp::arrays::ComplexArray;
    for (int rate : {192000, 1000000, 2400000}) {
        LogMMSE::SavedParamsC params;
        ComplexArray pending;
        const int total = rate * 22 / 10;
        double milliseconds = 0;
        size_t measuredSamples = 0, measuredAllocations = 0;
        for (int offset = 0; offset < total; offset += input.size()) {
            const int count = std::min<int>(input.size(), total-offset);
            for (int j = 0; j < count; ++j) {
                float phase = 2.0 * M_PI * 1700.0 * (offset+j) / rate;
                input[j] = {0.1f * std::cos(phase) + noise(), 0.1f * std::sin(phase) + noise()};
            }
            const bool measure = offset >= rate * 14 / 10;
            allocations = 0; trackAllocations = measure;
            auto t = std::clock();
            pending.insert(pending.end(), input.begin(), input.begin()+count);
            if (params.Xk_prev.empty() && pending.size() >= static_cast<size_t>(rate * 28 / 100)) {
                LogMMSE::logmmse_sample(pending, rate, &params, 12);
            }
            if (!params.Xk_prev.empty() && pending.size() >= static_cast<size_t>(params.Slen)) {
                auto block = LogMMSE::logmmse_all(pending, &params);
                pending.erase(pending.begin(), pending.begin()+block.size());
                if (!block.empty()) { checksum += block[block.size()/2].re; }
                emit(block.data(), block.size()*sizeof(block[0]));
            }
            if (measure) {
                milliseconds += 1000.0 * (std::clock()-t)/CLOCKS_PER_SEC;
                measuredSamples += count; measuredAllocations += allocations;
            }
            trackAllocations = false;
        }
        std::printf("LogMMSE rate=%d cpu_ms_per_signal_second=%.3f allocations_per_signal_second=%.0f\n", rate,
            milliseconds * rate / measuredSamples, static_cast<double>(measuredAllocations)*rate/measuredSamples);
    }
    {
        dsp::AFNR_OMLSA_MCRA filter;
        filter.allowed = true;
        std::vector<dsp::stereo_t> audio(4608), filtered(STREAM_BUFFER_SIZE);
        auto t = std::clock();
        allocations = 0; trackAllocations = true;
        size_t samples = 0;
        for (int i = 0; i < 105; ++i) {
            for (auto& x : audio) { x = {noise(), noise()}; }
            int wrote;
            filter.process(audio.data(), audio.size(), filtered.data(), wrote);
            samples += audio.size();
            if (wrote) { checksum += filtered[wrote/2].l; }
            emit(filtered.data(), wrote*sizeof(filtered[0]));
        }
        trackAllocations = false;
        std::printf("AudioNR rate=48000 cpu_ms_per_signal_second=%.3f allocations=%zu pending_samples=%zu\n",
            (1000.0*(std::clock()-t)/CLOCKS_PER_SEC)*48000/samples, allocations, filter.buffer.size());
    }
    if (output) { std::fclose(output); }
    std::printf("checksum=%.9f\n", checksum);
}
