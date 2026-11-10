#pragma once
#include <dsp/stream.h>
#include <dsp/types.h>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

// libusb invokes the RTL callback while handling USB events. Waiting for a
// downstream DSP/audio consumer there also stalls synchronous gain/AGC/tune
// transfers. Keep USB ingress bounded and move all stream waits to a worker.
class RTLSDRIQBuffer {
public:
    ~RTLSDRIQBuffer() { stop(); }

    // The USB producer must be stopped before start/reconfiguration/destruction.
    void start(dsp::stream<dsp::complex_t>* output, uint32_t packetBytes) {
        assert(!worker.joinable() && packetBytes && !(packetBytes & 1));
        assert(packetBytes / 2 <= STREAM_BUFFER_SIZE);
        out = output;
        bytesPerPacket = packetBytes;
        storage.resize(size_t(packetBytes) * slots);
        head.store(0, std::memory_order_relaxed);
        tail.store(0, std::memory_order_relaxed);
        dropped.store(0, std::memory_order_relaxed);
        out->clearWriteStop();
        active.store(true, std::memory_order_release);
        worker = std::thread([this] { consume(); });
    }

    // Single USB producer. No allocation, mutex acquisition or DSP wait. If
    // processing cannot keep up, preserve queued data and count the new packet
    // that cannot fit; never overwrite a packet being read by the consumer.
    bool push(const unsigned char* data, uint32_t bytes) noexcept {
        if (!active.load(std::memory_order_acquire)) { return false; }
        uint32_t write = head.load(std::memory_order_relaxed);
        if (!bytes || (bytes & 1) || bytes > bytesPerPacket ||
            write - tail.load(std::memory_order_acquire) == slots) {
            dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        auto index = write % slots;
        std::memcpy(storage.data() + size_t(index) * bytesPerPacket, data, bytes);
        lengths[index] = bytes;
        head.store(write + 1, std::memory_order_release);
        ready.notify_one();
        return true;
    }

    void stop() {
        active.store(false, std::memory_order_release);
        ready.notify_all();
        if (!worker.joinable()) { return; }
        out->stopWriter();
        worker.join();
        if (out->read_for(std::chrono::milliseconds(0)) > 0) { out->flush(); }
        out->clearWriteStop();
    }

    uint64_t droppedPackets() const { return dropped.load(std::memory_order_relaxed); }

private:
    void consume() {
        while (active.load(std::memory_order_acquire)) {
            uint32_t read = tail.load(std::memory_order_relaxed);
            if (read == head.load(std::memory_order_acquire)) {
                // A bounded wait also covers a notify arriving between the
                // predicate and sleep; the USB callback never takes waitMutex.
                std::unique_lock<std::mutex> lock(waitMutex);
                ready.wait_for(lock, std::chrono::milliseconds(1), [this, read] {
                    return !active.load(std::memory_order_acquire) ||
                        read != head.load(std::memory_order_acquire);
                });
                continue;
            }
            const auto* data = storage.data() + size_t(read % slots) * bytesPerPacket;
            const int samples = lengths[read % slots] / 2;
            for (int i = 0; i < samples; ++i) {
                out->writeBuf[i].re = (float(data[i * 2]) - 127.4) / 128.0f;
                out->writeBuf[i].im = (float(data[i * 2 + 1]) - 127.4) / 128.0f;
            }
            tail.store(read + 1, std::memory_order_release);
            if (!out->swap(samples)) { return; }
        }
    }

    static constexpr uint32_t slots = 32;
    dsp::stream<dsp::complex_t>* out = nullptr;
    uint32_t bytesPerPacket = 0;
    std::vector<unsigned char> storage;
    uint32_t lengths[slots]{};
    alignas(64) std::atomic<uint32_t> head { 0 };
    alignas(64) std::atomic<uint32_t> tail { 0 };
    std::atomic<uint64_t> dropped { 0 };
    std::atomic<bool> active { false };
    std::thread worker;
    std::mutex waitMutex;
    std::condition_variable ready;
};
