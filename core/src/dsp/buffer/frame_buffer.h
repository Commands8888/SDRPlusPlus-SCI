#pragma once
#include "../block.h"
#define TEST_BUFFER_SIZE 32

namespace dsp::buffer {
    template <class T>
    class SampleFrameBuffer : public block {
        using base_type = block;
    public:
        SampleFrameBuffer() {}

        SampleFrameBuffer(stream<T>* in) { init(in); }

        ~SampleFrameBuffer() {
            if (!base_type::_block_init) { return; }
            base_type::stop();
            for (int i = 0; i < TEST_BUFFER_SIZE; i++) {
                buffer::free(buffers[i]);
            }
        }

        void init(stream<T>* in) {
            _in = in;

            for (int i = 0; i < TEST_BUFFER_SIZE; i++) {
                buffers[i] = buffer::alloc<T>(STREAM_BUFFER_SIZE);
            }

            base_type::registerInput(in);
            base_type::registerOutput(&out);
            base_type::_block_init = true;
        }

        void setInput(stream<T>* in) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            base_type::tempStop();
            base_type::unregisterInput(_in);
            _in = in;
            base_type::registerInput(_in);
            base_type::tempStart();
        }

        void flush() {
            {
                std::lock_guard<std::mutex> lock(bufMtx);
                readCur = writeCur;
                queued = 0;
            }
            spaceAvailable.notify_all();
        }

        int queuedFrames() {
            std::lock_guard<std::mutex> lock(bufMtx);
            return queued;
        }

        void setBypass(bool enabled) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lock(base_type::ctrlMtx);
            if (bypass == enabled) { return; }
            base_type::tempStop();
            bypass = enabled;
            base_type::tempStart();
        }

        int run() {
            // Wait for data
            int count = _in->read();
            if (count < 0) { return -1; }

            if (bypass) {
                memcpy(out.writeBuf, _in->readBuf, count * sizeof(T));
                _in->flush();
                if (!out.swap(count)) { return -1; }
                return count;
            }

            // Push it on the ring buffer
            {
                std::unique_lock<std::mutex> lck(bufMtx);
                // A wrapped write index is not an empty queue. Preserve all
                // pending frames and apply backpressure rather than overwrite.
                spaceAvailable.wait(lck, [this] { return queued < TEST_BUFFER_SIZE || stopWorker; });
                if (stopWorker) {
                    lck.unlock();
                    _in->flush();
                    return -1;
                }
                memcpy(buffers[writeCur], _in->readBuf, count * sizeof(T));
                sizes[writeCur] = count;
                writeCur++;
                writeCur = ((writeCur) % TEST_BUFFER_SIZE);
                ++queued;
            }
            cnd.notify_all();
            _in->flush();
            return count;
        }

        void worker() {
            while (true) {
                // Wait for data
                std::unique_lock lck(bufMtx);
                cnd.wait(lck, [this]() { return queued > 0 || stopWorker; });
                if (stopWorker) { break; }

                // Write one to output buffer and unlock in preparation to swap buffers
                int count = sizes[readCur];
                memcpy(out.writeBuf, buffers[readCur], count * sizeof(T));
                readCur++;
                readCur = ((readCur) % TEST_BUFFER_SIZE);
                --queued;
                lck.unlock();
                spaceAvailable.notify_one();

                // Swap
                if (!out.swap(count)) { break; }
            }
        }

        stream<T> out;

        int writeCur = 0;
        int readCur = 0;

        bool bypass = false;

    private:
        void doStart() {
            base_type::workerThread = std::thread(&SampleFrameBuffer<T>::workerLoop, this);
            readWorkerThread = std::thread(&SampleFrameBuffer<T>::worker, this);
        }

        void doStop() {
            _in->stopReader();
            out.stopWriter();
            {
                std::lock_guard<std::mutex> lock(bufMtx);
                stopWorker = true;
            }
            cnd.notify_all();
            spaceAvailable.notify_all();

            if (base_type::workerThread.joinable()) { base_type::workerThread.join(); }
            if (readWorkerThread.joinable()) { readWorkerThread.join(); }

            _in->clearReadStop();
            out.clearWriteStop();
            {
                std::lock_guard<std::mutex> lock(bufMtx);
                stopWorker = false;
                readCur = writeCur = queued = 0;
            }
            if (_in->read_for(std::chrono::milliseconds(0)) > 0) { _in->flush(); }
        }

        stream<T>* _in;

        std::thread readWorkerThread;
        std::mutex bufMtx;
        std::condition_variable cnd;
        std::condition_variable spaceAvailable;
        T* buffers[TEST_BUFFER_SIZE];
        int sizes[TEST_BUFFER_SIZE];

        bool stopWorker = false;
        int queued = 0;
    };
}
