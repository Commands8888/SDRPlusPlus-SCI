#include "af_nr.h"
#include "omlsa_mcra/Datablock_Read.h"
#include <cassert>
#include <cstdio>
#include <random>

int main() {
    std::mt19937 random(179);
    std::uniform_real_distribution<float> noise(-0.1f,0.1f);
    std::vector<dsp::stereo_t> output(STREAM_BUFFER_SIZE);
    {
        G_calculate gain;
        assert(gain.Initialize(256) == 0);
        std::vector<Complex_num> spectrum(257,{100,0});
        spectrum.back() = {123456,654321};
        assert(gain.G_calculate_process(spectrum.data(),0) == 0);
        assert(spectrum.back().real == 123456 && spectrum.back().imag == 654321);
    }
    for (int rate : {8000,44100,48000,96000}) {
        dsp::AFNR_OMLSA_MCRA filter;
        filter.allowed = true;
        filter.setAudioSampleRate(rate);
        dsp::omlsa_mcra reference;
        reference.setSampleRate(rate);
        int block = reference.blockSize();
        std::vector<dsp::stereo_t> input(block*30), actual, expected;
        std::vector<short> pcm(block), reduced(block+1024);
        for (auto& x : input) { x = {noise(random),noise(random)}; }
        for (size_t offset = 0; offset < input.size(); offset += block) {
            for (int i = 0; i < block; ++i) pcm[i] = input[offset+i].l*32767.0f;
            int count = 0;
            assert(reference.process(pcm.data(),block,reduced.data(),count));
            for (int i = 0; i < count; ++i) expected.push_back({reduced[i]/32767.0f,reduced[i]/32767.0f});
        }
        // Arbitrary packet sizes must produce the exact same audio sequence
        // as consecutive complete blocks, with at most one partial block left.
        size_t offset = 0;
        for (int packet = 0; offset < input.size(); ++packet) {
            int count = std::min<size_t>(packet%3 ? 8192 : 127,input.size()-offset);
            int wrote = 0;
            filter.process(input.data()+offset,count,output.data(),wrote);
            assert(wrote >= 0 && wrote <= STREAM_BUFFER_SIZE);
            actual.insert(actual.end(),output.begin(),output.begin()+wrote);
            offset += count;
            assert(filter.buffer.size() < static_cast<size_t>(block));
        }
        assert(actual.size() == expected.size());
        for (size_t i = 0; i < actual.size(); ++i) assert(actual[i].l == expected[i].l && actual[i].r == expected[i].r);
        assert(!filter.failed && filter.buffer.empty());
        // Cross the old signed-short rollover without minutes of artificial
        // playback; the initialized estimator must keep its frame count.
        reference.dtr->m_blockInd = 32766;
        int wrote = 0;
        assert(reference.process(pcm.data(),block,reduced.data(),wrote));
        assert(reference.dtr->m_blockInd > 32767);
    }
    {
        dsp::AFNR_OMLSA_MCRA filter;
        filter.allowed = true;
        std::vector<dsp::stereo_t> input(STREAM_BUFFER_SIZE,{0.05f,0.05f});
        for (int packet = 0; packet < 3; ++packet) {
            int wrote;
            filter.process(input.data(),input.size(),output.data(),wrote);
            assert(wrote <= STREAM_BUFFER_SIZE);
            // The runtime drains leftovers before reading its next packet.
            while (filter.buffer.size() >= static_cast<size_t>(filter.omlsa_mcra.blockSize())) {
                filter.process(nullptr,0,output.data(),wrote);
                assert(wrote > 0 && wrote <= STREAM_BUFFER_SIZE);
            }
            assert(filter.buffer.size() < 4608);
        }
        filter.setAudioSampleRate(8000);
        input.assign(8192,{2.0f,-2.0f});
        int wrote;
        filter.process(input.data(),input.size(),output.data(),wrote);
        assert(filter.appliedSampleRate == 8000 && !filter.failed);
        for (int i = 0; i < wrote; ++i) assert(std::isfinite(output[i].l));
    }
    {
        dsp::stream<dsp::stereo_t> input;
        dsp::AFNR_OMLSA_MCRA filter;
        filter.init(&input);
        input.writeBuf[0] = {0,0};
        assert(input.swap(1));
        filter.out.stopWriter();
        assert(filter.run() == -1); // Stop must terminate, not spin returning 0.
    }
    std::puts("OK: Audio NR matches backend sample-for-sample across packet sizes and four rates; bounded backlog and clean stop");
}
