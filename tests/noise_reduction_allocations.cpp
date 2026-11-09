// Counts owning C++ allocations after the immutable lookup table is warm.
// No RSS/quarantine heuristics: every reset must actually free its old arrays.
#include "omlsa_mcra.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <initializer_list>

static long live = 0;
void* operator new(std::size_t n) {
    if (void* p = std::malloc(n ? n : 1)) { ++live; return p; }
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { if (p) { --live; std::free(p); } }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }

int main() {
    { dsp::omlsa_mcra warm; warm.setSampleRate(48000); }
    long before = live;
    for (int i = 0; i < 40; ++i) {
        dsp::omlsa_mcra nr;
        for (int rate : {48000,8000,44100,96000}) nr.setSampleRate(rate);
    }
    assert(live == before);
    std::puts("OK: 160 denoiser reinitializations release every owning allocation; gain table is shared");
}
