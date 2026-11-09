#include <atomic>
#include <cstdio>
#include <thread>
#include <dsp/stream.h>

int main() {
    dsp::stream<float> stream;
    std::atomic<bool> ready{false}, done{false};
    std::thread reader([&] {
        ready = true;
        while (!done.load()) stream.read_for(std::chrono::milliseconds(1));
    });
    while (!ready.load()) std::this_thread::yield();
    for (int i = 0; i < 20000; ++i) {
        stream.stopReader();
        stream.clearReadStop();
    }
    done = true;
    stream.stopReader();
    reader.join();
    ready = false;
    done = false;
    std::thread writer([&] {
        ready = true;
        while (!done.load()) stream.swap(1);
    });
    while (!ready.load()) std::this_thread::yield();
    for (int i = 0; i < 20000; ++i) {
        stream.stopWriter();
        stream.clearWriteStop();
        stream.flush();
    }
    done = true;
    stream.stopWriter();
    writer.join();
    std::puts("OK: stream stop/clear flags synchronize with waiting readers/writers");
}
