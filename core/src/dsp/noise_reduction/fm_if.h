#pragma once
#include "../processor.h"
#include "../window/nuttall.h"
#include <fftw3.h>
#include <cmath>

namespace dsp::noise_reduction {
    class FMIF : public Processor<complex_t, complex_t> {
        using base_type = Processor<complex_t, complex_t>;
    public:
        FMIF() {}

        FMIF(stream<complex_t>* in, int bins) { init(in, bins); }

        ~FMIF() {
            if (!base_type::_block_init) { return; }
            base_type::stop();
            destroyBuffers();
        }

        void init(stream<complex_t>* in, int bins) {
            _bins = bins;
            initBuffers();
            base_type::init(in);
        }

        void setBins(int bins) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            base_type::tempStop();
            _bins = bins;
            destroyBuffers();
            initBuffers();
            base_type::tempStart();
        }

        void reset() {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            base_type::tempStop();
            buffer::clear(buffer, _bins - 1);
            base_type::tempStart();
        }

        int process(int count, const complex_t* in, complex_t* out) {
            // Write new input data to buffer buffer
            memcpy(bufferStart, in, count * sizeof(complex_t));
            
            // Iterate the FFT
            for (int i = 0; i < count; i++) {
                // Apply windows
                volk_32fc_32f_multiply_32fc((lv_32fc_t*)forwFFTIn, (lv_32fc_t*)&buffer[i], fftWin, _bins);

                // Do forward FFT
                fftwf_execute(forwardPlan);

                // Process bins here
                uint32_t idx;
                volk_32fc_magnitude_32f(ampBuf, (lv_32fc_t*)forwFFTOut, _bins);
                volk_32f_index_max_32u(&idx, ampBuf, _bins);

                // The inverse spectrum has exactly one nonzero bin. Its
                // middle time sample is that bin times a fixed phasor; a
                // whole inverse FFT per input sample computes unused work.
                // FFTW's backward transform is unnormalized (no 1/N here).
                out[i] = forwFFTOut[idx] * inversePhase[idx];
            }

            // Move buffer buffer
            memmove(buffer, &buffer[count], (_bins - 1) * sizeof(complex_t));

            return count;
        }

        int run() {
            int count = base_type::_in->read();
            if (count < 0) { return -1; }

            process(count, base_type::_in->readBuf, base_type::out.writeBuf);

            // Swap if some data was generated
            base_type::_in->flush();
            if (!base_type::out.swap(count)) { return -1; }
            return count;
        }

    protected:
        void initBuffers() {
            // Allocate FFT buffers
            forwFFTIn = (complex_t*)fftwf_malloc(_bins * sizeof(complex_t));
            forwFFTOut = (complex_t*)fftwf_malloc(_bins * sizeof(complex_t));
            inversePhase = buffer::alloc<complex_t>(_bins);
            for (int i = 0; i < _bins; ++i) {
                if ((_bins % 2) == 0) {
                    inversePhase[i] = { (i % 2) ? -1.0f : 1.0f, 0.0f };
                }
                else {
                    double phase = 2.0 * M_PI * i * (_bins / 2) / _bins;
                    inversePhase[i] = { (float)std::cos(phase), (float)std::sin(phase) };
                }
            }

            // Allocate and clear delay buffer
            buffer = buffer::alloc<complex_t>(STREAM_BUFFER_SIZE + _bins - 1);
            bufferStart = &buffer[_bins - 1];
            buffer::clear(buffer, _bins - 1);

            // Allocate amplitude buffer
            ampBuf = buffer::alloc<float>(_bins);

            // Allocate and generate Window
            fftWin = buffer::alloc<float>(_bins);
            for (int i = 0; i < _bins; i++) { fftWin[i] = window::nuttall(i, _bins - 1); }

            // Plan FFTs
            forwardPlan = fftwf_plan_dft_1d(_bins, (fftwf_complex*)forwFFTIn, (fftwf_complex*)forwFFTOut, FFTW_FORWARD, FFTW_ESTIMATE);
        }

        void destroyBuffers() {
            fftwf_destroy_plan(forwardPlan);
            fftwf_free(forwFFTIn);
            fftwf_free(forwFFTOut);
            buffer::free(inversePhase);
            buffer::free(buffer);
            buffer::free(ampBuf);
            buffer::free(fftWin);
        }

        complex_t* forwFFTIn;
        complex_t* forwFFTOut;
        complex_t* inversePhase;

        fftwf_plan forwardPlan;

        complex_t* buffer;
        complex_t* bufferStart;

        float* fftWin;

        float* ampBuf;

        int _bins;

    }; 
}
