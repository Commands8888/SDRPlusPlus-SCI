#pragma once
#include "../sink.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <vector>

namespace dsp::sink {
    // Single DSP producer / single device callback. Configure/start only while
    // the device callback is stopped. The callback never allocates, locks,
    // waits for DSP, or accesses GUI state. Backpressure stays on the producer.
    class AudioBuffer : public Sink<stereo_t> {
    public:
        ~AudioBuffer() override { if (_block_init) { stop(); } }

        void configure(unsigned sampleRate, unsigned deviceFrames) {
            assert(!running && sampleRate && deviceFrames);
            const uint32_t wanted = std::max<uint32_t>(deviceFrames * 2, sampleRate / 50);
            uint32_t capacity = 1;
            while (capacity < wanted && capacity < STREAM_BUFFER_SIZE) { capacity <<= 1; }
            storage.resize(capacity);
            mask = capacity - 1;
            primeFrames = std::min<uint32_t>(capacity / 2, std::max<uint32_t>(deviceFrames, sampleRate / 100));
        }

        uint32_t capacity() const { return storage.size(); }
        uint32_t queued() const {
            return std::min<uint32_t>(capacity(), written.load(std::memory_order_acquire) - consumed.load(std::memory_order_acquire));
        }
        uint64_t underruns() const { return shortReads.load(std::memory_order_relaxed); }

        // Returns real frames copied; the remainder is always initialized to
        // silence. A device may ask for a different size on every callback.
        unsigned renderStereo(stereo_t* out, unsigned frames) noexcept { return render(out, frames, false); }
        unsigned renderMono(float* out, unsigned frames) noexcept { return render(out, frames, true); }

        int run() override {
            int count = _in->read();
            if (count < 0) { return -1; }
            int offset = 0;
            while (offset < count && active.load(std::memory_order_acquire)) {
                uint32_t head = written.load(std::memory_order_relaxed);
                uint32_t tail = consumed.load(std::memory_order_acquire);
                uint32_t free = capacity() - (head - tail);
                if (!free) {
                    // The callback performs no condition-variable operations.
                    // Only a full producer queue waits, at most 1 ms per wake.
                    std::unique_lock<std::mutex> lock(waitMutex);
                    stopWake.wait_for(lock, std::chrono::milliseconds(1), [&] {
                        return !active.load(std::memory_order_acquire) || queued() < capacity();
                    });
                    continue;
                }
                uint32_t n = std::min<uint32_t>(free, count - offset);
                uint32_t first = std::min<uint32_t>(n, capacity() - (head & mask));
                std::memcpy(storage.data() + (head & mask), _in->readBuf + offset, first * sizeof(stereo_t));
                std::memcpy(storage.data(), _in->readBuf + offset + first, (n - first) * sizeof(stereo_t));
                written.store(head + n, std::memory_order_release);
                offset += n;
            }
            _in->flush();
            return active.load(std::memory_order_acquire) ? count : -1;
        }

    protected:
        void doStart() override {
            assert(!storage.empty());
            written.store(0, std::memory_order_relaxed);
            consumed.store(0, std::memory_order_relaxed);
            shortReads.store(0, std::memory_order_relaxed);
            primed = false;
            primeWait = 0;
            active.store(true, std::memory_order_release);
            block::doStart();
        }
        void doStop() override {
            active.store(false, std::memory_order_release);
            stopWake.notify_all();
            block::doStop();
            // Do not replay an already published old-rate packet when a
            // different device/rate is started on the same stream.
            if (_in->read_for(std::chrono::milliseconds(0)) > 0) { _in->flush(); }
        }

    private:
        unsigned render(void* output, unsigned frames, bool mono) noexcept {
            if (!frames) { return 0; }
            std::memset(output, 0, frames * (mono ? sizeof(float) : sizeof(stereo_t)));
            if (!active.load(std::memory_order_acquire)) { return 0; }
            uint32_t tail = consumed.load(std::memory_order_relaxed);
            uint32_t available = written.load(std::memory_order_acquire) - tail;
            // One device period of headroom (at least 10 ms when capacity
            // permits) absorbs normal DSP scheduling jitter. No startup sleep.
            if (!available) {
                if (primed) { shortReads.fetch_add(1, std::memory_order_relaxed); }
                primed = false;
                primeWait = 0;
                return 0;
            }
            if (!primed && available < primeFrames) {
                // A final short packet must still play if no more data arrives.
                if (primeWait < primeFrames) {
                    primeWait = std::min<uint32_t>(primeFrames, primeWait + frames);
                    return 0;
                }
            }
            primed = true;
            primeWait = 0;
            uint32_t n = std::min<uint32_t>(frames, available);
            uint32_t first = std::min<uint32_t>(n, capacity() - (tail & mask));
            if (mono) {
                auto out = static_cast<float*>(output);
                for (uint32_t i = 0; i < n; ++i) {
                    auto sample = storage[(tail + i) & mask];
                    out[i] = (sample.l + sample.r) / 2.0f;
                }
            }
            else {
                auto out = static_cast<stereo_t*>(output);
                std::memcpy(out, storage.data() + (tail & mask), first * sizeof(stereo_t));
                std::memcpy(out + first, storage.data(), (n - first) * sizeof(stereo_t));
            }
            consumed.store(tail + n, std::memory_order_release);
            if (n < frames) {
                primed = false;
                shortReads.fetch_add(1, std::memory_order_relaxed);
            }
            return n;
        }

        std::vector<stereo_t> storage;
        uint32_t mask = 0, primeFrames = 0;
        uint32_t primeWait = 0;
        alignas(64) std::atomic<uint32_t> written { 0 };
        alignas(64) std::atomic<uint32_t> consumed { 0 };
        std::atomic<uint64_t> shortReads { 0 };
        std::atomic<bool> active { false };
        bool primed = false; // callback-owned until the device is stopped
        std::mutex waitMutex;
        std::condition_variable stopWake;
    };
}
