#pragma once
#include <dsp/block.h>
#include <dsp/stream.h>
#include <dsp/types.h>
#include <dsp/processor.h>
#include <utils/flog.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>
#include "omlsa_mcra.h"

namespace dsp {

    // OMLSA-MCRA speech denoiser ("Audio NR2"), inserted into the radio's
    // stereo AF chain. Processes the left channel in fixed-point blocks and
    // outputs mono (L copied to R). Buffers partial frames; passthrough while
    // `allowed` is off or after a fatal init failure.
    struct AFNR_OMLSA_MCRA : public Processor<stereo_t, stereo_t> {
        bool failed = false;
        dsp::omlsa_mcra omlsa_mcra;
        std::vector<stereo_t> buffer;
        std::atomic<bool> allowed { false };
        std::atomic<int> targetSampleRate { 48000 };
        std::atomic<float> scaled { 32767.0f }; // UI snapshot only
        int appliedSampleRate = 0;

        ~AFNR_OMLSA_MCRA() override {
            if (_block_init) { stop(); }
        }

        void setAudioSampleRate(int sampleRate) {
            if (sampleRate > 0) { targetSampleRate.store(sampleRate, std::memory_order_relaxed); }
        }

        void doStart() override {
            resetRequested = true;
            Processor::doStart();
        }

        void process(const stereo_t* readBuf, int count, stereo_t* writeBuf, int& wrote) {
            wrote = 0;
            if (!allowed.load(std::memory_order_relaxed)) {
                if (count) { std::copy(readBuf, readBuf + count, writeBuf); }
                wrote = count;
                buffer.clear();
                return;
            }
            int rate = targetSampleRate.load(std::memory_order_relaxed);
            if (rate != appliedSampleRate || resetRequested) {
                omlsa_mcra.setSampleRate(rate);
                appliedSampleRate = rate;
                resetRequested = false;
                buffer.clear();
                scale = 32767.0f;
            }
            if (count) { buffer.insert(buffer.end(), readBuf, readBuf + count); }
            const int blockSize = omlsa_mcra.blockSize();
            processIn.resize(blockSize);
            // The backend can return carried-over overlap as well as this
            // block. Its largest analysis window is 1024 samples.
            processOut.resize(blockSize + 1024);
            size_t consumed = 0;
            while (buffer.size() - consumed >= static_cast<size_t>(blockSize)) {
                if (failed) {
                    size_t n = std::min(buffer.size() - consumed, static_cast<size_t>(STREAM_BUFFER_SIZE - wrote));
                    std::copy_n(buffer.data() + consumed, n, writeBuf + wrote);
                    consumed += n;
                    wrote += n;
                    break;
                }
                if (wrote + processOut.size() > STREAM_BUFFER_SIZE) { break; }
                if (scale < 32757.0f) { scale += 10.0f; }
                float peak = 0.0f;
                for (int q = 0; q < blockSize; ++q) {
                    float sample = buffer[consumed + q].l;
                    if (std::isfinite(sample)) { peak = std::max(peak, std::fabs(sample)); }
                }
                if (peak > 32767.0f / scale) { scale = 32767.0f / peak; }
                for (int q = 0; q < blockSize; ++q) {
                    float sample = buffer[consumed + q].l * scale;
                    processIn[q] = std::isfinite(sample) ? static_cast<short>(std::clamp(sample, -32767.0f, 32767.0f)) : 0;
                }
                int produced = 0;
                if (!omlsa_mcra.process(processIn.data(), blockSize, processOut.data(), produced)) {
                    flog::error("OMLSA processing failed, disabling Audio NR2");
                    failed = true;
                    continue;
                }
                for (int q = 0; q < produced; ++q) {
                    writeBuf[wrote + q].r = writeBuf[wrote + q].l = processOut[q] / scale;
                }
                wrote += produced;
                consumed += blockSize;
            }
            // Compact once per input packet, not after every OMLSA block.
            buffer.erase(buffer.begin(), buffer.begin() + consumed);
            scaled.store(scale, std::memory_order_relaxed);
        }

        int run() override {
            // Drain a full output packet before accepting more input. This
            // also bounds pending audio when upstream sends maximum packets.
            bool drain = allowed.load(std::memory_order_relaxed) && appliedSampleRate &&
                (buffer.size() >= static_cast<size_t>(omlsa_mcra.blockSize()));
            int count = drain ? 0 : _in->read();
            if (count < 0) { return -1; }
            int wrote;
            process(drain ? nullptr : _in->readBuf, count, out.writeBuf, wrote);
            if (!drain) { _in->flush(); }
            if (wrote && !out.swap(wrote)) { return -1; }
            return 1;
        }

    private:
        std::vector<short> processIn, processOut;
        float scale = 32767.0f;
        bool resetRequested = true; // DSP-thread owned, set before starting it
    };

}
