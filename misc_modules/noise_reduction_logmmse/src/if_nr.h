#pragma once
#include <dsp/block.h>
#include <dsp/stream.h>
#include <dsp/types.h>
#include <dsp/processor.h>
#include "arrays.h"
#include <utils/flog.h>
#include <signal_path/signal_path.h>
#include <atomic>
#include <chrono>
#include "logmmse.h"

namespace dsp {

    using namespace ::dsp::arrays;
    using namespace ::dsp::logmmse;

    // Monotonic: feeds the CPU-usage watchdog, which must not trip on wall-clock jumps.
    inline long long currentTimeMillis() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    class IFNRLogMMSE : public Processor<complex_t, complex_t> {
        using base_type = Processor<complex_t, complex_t>;

    public:
        ~IFNRLogMMSE() override {
            if (_block_init) { stop(); }
        }

        void setInput(stream<complex_t>* in) override {
            if (!_block_init) {
                init(in);
            }
            else {
                Processor::setInput(in);
            }
        }

    public:
        ComplexArray worker1c;
        size_t inputOffset = 0;
        int freq = 192000;
        LogMMSE::SavedParamsC params;

        void doStart() override {
            reset();
            lastReport = currentTimeMillis();
            cpuUsed = 0;
            prevPercentUsage = 0;
            percentUsage = 0;
            txHandler.ctx = this;
            txHandler.handler = [](bool active, void* ctx) {
                ((IFNRLogMMSE*)ctx)->txActive.store(active, std::memory_order_relaxed);
            };
            sigpath::txState.bindHandler(&txHandler);
            base_type::doStart();
        }

        void doStop() override {
            base_type::doStop();
            sigpath::txState.unbindHandler(&txHandler);
            percentUsage = -1;
        }

        void setDisableCpuDeactivation(bool disable) {
            disableCpuDeactivation = disable;
        }

        std::atomic<bool> shouldReset { true };
        void reset() {
            shouldReset = true;
        }

        void process(complex_t* in, int count, complex_t* out, int& outCount) {
            // Detect sample rate / decimation changes: the noise profile is
            // only valid for the rate it was sampled at.
            int curFreq = (int)sigpath::iqFrontEnd.getSampleRate();
            if (shouldReset.exchange(false, std::memory_order_relaxed) || curFreq != freq) {
                flog::debug("Resetting IF NR LogMMSE");
                worker1c.clear();
                inputOffset = 0;
                params.reset();
                freq = curFreq;
            }
            params.hold = txActive.load(std::memory_order_relaxed);
            if (count) {
                if (inputOffset) {
                    worker1c.erase(worker1c.begin(), worker1c.begin() + inputOffset);
                    inputOffset = 0;
                }
                worker1c.insert(worker1c.end(), in, in + count);
            }
            int noiseFrames = 12;
            int fram = freq / 100;
            int initialDemand = params.Xk_prev.empty() ? 0 : params.Slen + params.len2;
            if (params.Xk_prev.empty()) {
                initialDemand = fram * (noiseFrames + 2) * 2;
            }
            if (worker1c.size() - inputOffset < initialDemand) {
                outCount = 0;
                return;
            }
            if (params.Xk_prev.empty()) {
                flog::debug("IF NR LogMMSE: sampling noise profile");
                LogMMSE::logmmse_sample(worker1c, freq, &params, noiseFrames);
            }

            // One hop per publication bounds the processing burst and lets
            // downstream FFT/radios/sinks run between hops. Drain the backlog
            // before accepting another input packet; no samples are dropped.
            outCount = LogMMSE::processInto(worker1c.data() + inputOffset,
                worker1c.size() - inputOffset, &params, out, params.len2, 4.0f);
            inputOffset += outCount;
        }

        bool hasPendingFrame() const {
            return !params.Xk_prev.empty() && worker1c.size() - inputOffset >= static_cast<size_t>(params.Slen + params.len2);
        }

        long long lastReport = currentTimeMillis();
        long long cpuUsed = 0;
        std::atomic<int> percentUsage { -1 };
        int prevPercentUsage = 0;

        int runMMSE(stream<complex_t>* _in, stream<complex_t>& out) {
            bool drain = hasPendingFrame();
            int count = drain ? 0 : _in->read();
            if (count < 0) { return -1; }
            int outCount;
            long long ctm0 = currentTimeMillis();
            process(drain ? nullptr : _in->readBuf, count, out.writeBuf, outCount);
            long long ctm = currentTimeMillis();
            if (!drain) { _in->flush(); }
            if (outCount && !out.swap(outCount)) {
                return -1;
            }
            cpuUsed += ctm - ctm0;
            if (lastReport / 400 != ctm / 400) {
                auto timeSinceLastReport = ctm - lastReport;
                auto usedSinceLastReport = cpuUsed;
                cpuUsed = 0;
                lastReport = ctm;
                prevPercentUsage = percentUsage;
                percentUsage = (usedSinceLastReport * 100) / timeSinceLastReport;
                if (prevPercentUsage >= 95 && percentUsage >= 95 && !disableCpuDeactivation) {
                    stopReason = "Slow CPU. Reduce sample rate.";
                }
            }
            return 1;
        }

        int run() override {
            return runMMSE(_in, out);
        }

        bool bypass = true;
        // Written by the DSP thread, read by the UI thread every frame; points
        // to a string literal (or nullptr) so the load is a single atomic read.
        std::atomic<const char*> stopReason { nullptr };
        std::atomic<bool> disableCpuDeactivation { false };
        std::atomic<bool> txActive { false };
        EventHandler<bool> txHandler;
    };


}
