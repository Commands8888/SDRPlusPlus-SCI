#include <dsp/buffer/frame_buffer.h>
#include <cassert>
#include <cstdio>
#include <thread>
#include <chrono>

int main() {
    dsp::stream<dsp::complex_t> input;
    dsp::buffer::SampleFrameBuffer<dsp::complex_t> buffer;
    buffer.init(&input);
    buffer.start();
    std::thread producer([&] {
        for(int frame=0;frame<97;++frame) {
            int count=frame%3==0?1:(frame%3==1?4096:333);
            for(int i=0;i<count;++i) input.writeBuf[i]={(float)frame,(float)i};
            if(!input.swap(count)) break;
        }
    });
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    // Deliberately stop consumption until the queue is full. It must preserve
    // the first frame and all 97 subsequent frames, across multiple wraps.
    while(buffer.queuedFrames()!=TEST_BUFFER_SIZE) {
        assert(std::chrono::steady_clock::now()<deadline);
        std::this_thread::yield();
    }
    for(int frame=0;frame<97;++frame) {
        int count=buffer.out.read();
        assert(count==(frame%3==0?1:(frame%3==1?4096:333)));
        for(int i=0;i<count;++i) assert(buffer.out.readBuf[i].re==frame && buffer.out.readBuf[i].im==i);
        buffer.out.flush();
    }
    producer.join();buffer.stop();
    assert(buffer.queuedFrames()==0);
    // Stop both full and empty queues; restart in either buffered or bypass
    // mode without replaying data from the previous run.
    for(bool bypass : {false,true,false}) {
        buffer.setBypass(bypass);
        buffer.start();
        input.writeBuf[0]={123,456};assert(input.swap(1));
        assert(buffer.out.read()==1);
        assert(buffer.out.readBuf[0].re==123 && buffer.out.readBuf[0].im==456);
        buffer.out.flush();buffer.stop();
    }
    buffer.setBypass(false);buffer.start();
    std::thread blocked([&] {
        for(int frame=0;frame<97;++frame) {
            input.writeBuf[0]={1,2};
            if(!input.swap(1)) break;
        }
    });
    deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while(buffer.queuedFrames()!=TEST_BUFFER_SIZE) {
        assert(std::chrono::steady_clock::now()<deadline);
        std::this_thread::yield();
    }
    input.stopWriter();buffer.stop();blocked.join();
    std::puts("OK: full IQ queues preserve 97 ordered variable frames, stop promptly and restart buffered/bypassed without stale data");
}
