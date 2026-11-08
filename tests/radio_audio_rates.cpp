#include <cassert>
#include <cmath>
#include <cstdio>
#include <core.h>
#include <gui/gui.h>
#include <dsp/multirate/rational_resampler.h>

int main() {
    dsp::stream<dsp::stereo_t> input;
    dsp::multirate::RationalResampler<dsp::stereo_t> resampler;
    resampler.init(&input, 250000.0, 48000.0);
    const double rates[] = {50000.0, 250000.0, 48000.0, 3000.0, 9600.0, 8000.0};
    for (double rate : rates) {
        resampler.setRates(rate, 48000.0);
        int total = 0;
        for (int pass = 0; pass < 8; ++pass) {
            for (int i = 0; i < 512; ++i) input.writeBuf[i] = {0.25f, -0.125f};
            int count = resampler.process(512, input.writeBuf, resampler.out.writeBuf);
            assert(count >= 0 && count <= STREAM_BUFFER_SIZE);
            total += count;
            for (int i = 0; i < count; ++i) {
                assert(std::isfinite(resampler.out.writeBuf[i].l));
                assert(std::isfinite(resampler.out.writeBuf[i].r));
            }
            if (pass == 7 && count > 0) {
                assert(std::fabs(resampler.out.writeBuf[count-1].l - 0.25f) < 0.025f);
                assert(std::fabs(resampler.out.writeBuf[count-1].r + 0.125f) < 0.025f);
            }
        }
        assert(total > 0);
        std::printf("OK: Radio AF %.0f -> 48000 Hz; %d finite stereo samples\n", rate, total);
    }
}
