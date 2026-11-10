// Simulate libusb's event serialization: a gain/AGC control transfer shares
// the event context with bulk callbacks. No radio or sound card is required.
#include "../source_modules/rtl_sdr_source/src/rtl_iq_buffer.h"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <mutex>

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

static void waitUntil(const std::atomic<unsigned>& value, unsigned target) {
    auto limit = Clock::now() + 2s;
    while (value.load() < target) {
        assert(Clock::now() < limit);
        std::this_thread::yield();
    }
}

static bool tryWithin(std::mutex& mutex, std::chrono::milliseconds timeout) {
    const auto deadline = Clock::now() + timeout;
    do {
        if (mutex.try_lock()) { return true; }
        std::this_thread::yield();
    } while (Clock::now() < deadline);
    return false;
}

int main() {
    // Reproduce the previous callback: the second stream.swap waits for DSP
    // while still inside USB event dispatch, so a hardware command must wait.
    {
        dsp::stream<dsp::complex_t> out;
        std::mutex usbEvents;
        std::atomic<unsigned> entered { 0 };
        std::thread usb([&] {
            std::lock_guard<std::mutex> event(usbEvents);
            assert(out.swap(1));
            entered = 1;
            out.swap(1);
        });
        waitUntil(entered, 1);
        auto start = Clock::now();
        assert(!tryWithin(usbEvents, 250ms));
        auto blocked = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
        out.stopWriter();usb.join();
        std::printf("OK: old RTL callback reproduces blocked USB control for %lld us with stalled DSP\n", (long long)blocked);
    }
    // With the exact production ingress, even a full queue and a DSP consumer
    // that never reads cannot retain the USB event context or block shutdown.
    {
        dsp::stream<dsp::complex_t> out;
        RTLSDRIQBuffer buffer;
        buffer.start(&out, 16384);
        std::vector<unsigned char> packet(16384, 128);
        std::mutex usbEvents;
        std::atomic<unsigned> entered { 0 };
        std::thread usb([&] {
            std::lock_guard<std::mutex> event(usbEvents);
            entered = 1;
            for (int i = 0; i < 1024; ++i) { buffer.push(packet.data(), packet.size()); }
        });
        waitUntil(entered, 1);
        auto start = Clock::now();
        assert(tryWithin(usbEvents, 100ms));
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
        usbEvents.unlock();usb.join();
        assert(buffer.droppedPackets() > 0);
        assert(!buffer.push(packet.data(), 3)); // Reject incomplete IQ pairs.
        assert(!buffer.push(packet.data(), packet.size() + 2));
        start = Clock::now();buffer.stop();
        assert(Clock::now() - start < 100ms);
        std::printf("OK: RTL USB control completed in %lld us with full/stalled DSP; stop <100 ms; overload counted\n", (long long)elapsed);
    }
    // The normal path keeps every byte/sample, including the original double
    // precision conversion constants, variable packet lengths and queue wraps.
    for (uint32_t bytes : {1024u, 11776u, 16384u}) {
        dsp::stream<dsp::complex_t> out;
        RTLSDRIQBuffer buffer;
        buffer.start(&out, bytes);
        std::atomic<unsigned> received { 0 };
        std::thread consumer([&] {
            for (unsigned frame = 0; frame < 256; ++frame) {
                auto count = out.read();
                auto expected = frame % 3 == 0 ? 1u : (frame % 3 == 1 ? bytes / 2 : 333u);
                assert(count == int(expected));
                for (int i = 0; i < count; ++i) {
                    float re = (float((frame + 2 * i) % 256) - 127.4) / 128.0f;
                    float im = (float((frame + 2 * i + 1) % 256) - 127.4) / 128.0f;
                    assert(out.readBuf[i].re == re && out.readBuf[i].im == im);
                }
                out.flush();received = frame + 1;
            }
        });
        std::vector<unsigned char> packet(bytes);
        for (unsigned frame = 0; frame < 256; ++frame) {
            unsigned count = frame % 3 == 0 ? 1 : (frame % 3 == 1 ? bytes / 2 : 333);
            for (unsigned i = 0; i < count * 2; ++i) { packet[i] = (frame + i) % 256; }
            assert(buffer.push(packet.data(), count * 2));
            if (frame % 16 == 15) { waitUntil(received, frame + 1); }
        }
        consumer.join();assert(buffer.droppedPackets() == 0);buffer.stop();
        for (int run = 0; run < 40; ++run) {
            buffer.start(&out, bytes);
            std::atomic<unsigned> entered { 0 };
            std::atomic<bool> done { false };
            std::thread usb([&] {
                entered = 1;
                while (!done.load()) { buffer.push(packet.data(), bytes); }
            });
            waitUntil(entered, 1);
            std::this_thread::sleep_for(1ms);
            buffer.stop();done = true;usb.join();
            // No old packet may survive the next start.
            assert(out.read_for(0ms) == 0);
        }
    }
    std::puts("OK: RTL ingress preserves 768 variable packets exactly, 120 concurrent stops/restarts, zero normal-path loss");
}
