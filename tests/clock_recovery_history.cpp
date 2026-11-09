// Poison fresh allocations to reproduce non-zero/reused heap pages at startup.
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <array>
#include <dsp/clock_recovery/fd.h>
#include <dsp/clock_recovery/mm.h>

extern "C" __attribute__((visibility("default"))) void* __real_volk_malloc(size_t, size_t);
extern "C" __attribute__((visibility("default"))) void* __wrap_volk_malloc(size_t size, size_t align) {
    void* memory = __real_volk_malloc(size, align);
    assert(memory);
    const float poison = std::numeric_limits<float>::quiet_NaN();
    for (size_t i = 0; i+sizeof(float) <= size; i += sizeof(float))
        std::memcpy(static_cast<char*>(memory)+i, &poison, sizeof(float));
    return memory;
}

template<class Clock, class T> void check() {
    dsp::stream<T> input;
    Clock clock(&input, 2.0, 0.02, 0.1, 0.001, 32, 8);
    std::array<T,128> in{},out{},initial{};
    for (auto& value : in) {
        if constexpr(std::is_same_v<T,float>) value = 0.25f;
        else value = {0.25f,-0.125f};
    }
    auto run = [&] {
        int count = clock.process(in.size(),in.data(),out.data());
        assert(count > 0 && count <= static_cast<int>(out.size()));
        for (int i = 0; i < count; ++i) {
            if constexpr(std::is_same_v<T,float>) assert(std::isfinite(out[i]));
            else assert(std::isfinite(out[i].re) && std::isfinite(out[i].im));
        }
        return count;
    };
    const int first = run();
    initial = out;
    clock.reset();
    assert(run() == first);
    for (int i = 0; i < first; ++i) assert(std::memcmp(&out[i], &initial[i], sizeof(T)) == 0);
    clock.setInterpParams(32,8);
    assert(run() == first);
    for (int i = 0; i < first; ++i) assert(std::memcmp(&out[i], &initial[i], sizeof(T)) == 0);
}

int main() {
    check<dsp::clock_recovery::FD,float>();
    check<dsp::clock_recovery::MM<float>,float>();
    check<dsp::clock_recovery::MM<dsp::complex_t>,dsp::complex_t>();
    std::puts("OK: FD/MM start/reset/reconfigure use clean history even on poisoned/reused heap pages");
}
