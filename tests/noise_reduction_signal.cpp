// Signal preservation against the original two-FFT IF filter and the original
// vector-expression LogMMSE estimator. No receiver or sound device required.
#include <dsp/noise_reduction/fm_if.h>
#include "logmmse.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <random>

using dsp::arrays::ComplexArray;
using dsp::arrays::FloatArray;
using Params = dsp::logmmse::LogMMSE::SavedParamsC;
using MMSE = dsp::logmmse::LogMMSE;

static ComplexArray referenceMMSE(const ComplexArray& x, Params& p) {
    using namespace dsp::arrays;
    int frames = x.size()/p.len2 - p.Slen/p.len2;
    if (frames <= 0) { return {}; }
    p.update_noise_mu2();
    auto result = npzeros_c(frames*p.len2);
    for (int k = 0; k < frames*p.len2; k += p.len2) {
        npfftfft(muleach(p.win, nparange(x, k, k+p.Slen)), p.forwardPlan);
        const auto& spectrum = p.forwardPlan->getOutput();
        auto sig = npabsolute(spectrum);
        for (size_t i = 1; i < sig.size(); ++i) if (sig[i] == 0) sig[i] = sig[i-1];
        auto gamma = npminimum(diveach(muleach(sig, sig), p.noise_mu2), 40);
        FloatArray ksi;
        if (!npall(p.Xk_prev)) {
            ksi = add(mul(npmaximum(add(gamma,-1),0),1-p.aa),p.aa);
        } else {
            ksi = npmaximum(addeach(diveach(mul(p.Xk_prev,p.aa),p.noise_mu2),
                mul(npmaximum(add(gamma,-1),0),1-p.aa)),p.ksi_min);
        }
        auto a = diveach(ksi,add(ksi,1));
        auto h = muleach(a,npexp(mul(scipyspecialexpn(muleach(a,gamma)),0.5)));
        auto filtered = muleach(sig,h);
        p.Xk_prev = muleach(filtered,filtered);
        p.add_noise_history(std::move(sig));
        npfftfft(muleach(h,spectrum),p.reversePlan);
        const auto& inverse = p.reversePlan->getOutput();
        nparangeset(result,k,addeach(p.x_old,nparange(inverse,0,p.len1)));
        p.x_old = nparange(inverse,p.len1,p.Slen);
    }
    return result;
}

static void compare(const dsp::complex_t* a, const dsp::complex_t* b, int count, double tolerance, int context) {
    double error = 0, energy = 0;
    for (int i = 0; i < count; ++i) {
        assert(std::isfinite(a[i].re) && std::isfinite(a[i].im));
        assert(std::isfinite(b[i].re) && std::isfinite(b[i].im));
        auto d = a[i]-b[i];
        error += d.re*d.re+d.im*d.im;
        energy += b[i].re*b[i].re+b[i].im*b[i].im;
    }
    if (error > tolerance*tolerance*std::max(energy,1.0e-20)) {
        std::fprintf(stderr,"Signal comparison failed: relative RMS %.9g (limit %.9g), count %d, bins/rate %d\n",
            std::sqrt(error/std::max(energy,1.0e-20)),tolerance,count,context);
        std::abort();
    }
}

int main() {
    std::mt19937 random(179);
    std::uniform_real_distribution<float> noise(-0.1f,0.1f);
    dsp::stream<dsp::complex_t> stream;
    for (int bins : {9,15,31,32}) {
        dsp::noise_reduction::FMIF filter(&stream,bins);
        // Independent oracle computes the complete inverse spectrum, exactly
        // as v1.7.8 did; tests odd/even phases and history across tiny packets.
        std::vector<dsp::complex_t> history(bins-1), input(4096), actual(4096), expected(4096);
        // Match the original analyzer's FFTW/VOLK alignment as well as its
        // magnitude kernel, including architectures with approximate SIMD sqrt.
        auto fftIn = (dsp::complex_t*)fftwf_malloc(bins*sizeof(dsp::complex_t));
        auto fftOut = (dsp::complex_t*)fftwf_malloc(bins*sizeof(dsp::complex_t));
        auto inverseIn = (dsp::complex_t*)fftwf_malloc(bins*sizeof(dsp::complex_t));
        auto inverseOut = (dsp::complex_t*)fftwf_malloc(bins*sizeof(dsp::complex_t));
        std::fill_n(inverseIn,bins,dsp::complex_t{0,0});
        auto amplitude = dsp::buffer::alloc<float>(bins);
        auto forward = fftwf_plan_dft_1d(bins,(fftwf_complex*)fftIn,(fftwf_complex*)fftOut,FFTW_FORWARD,FFTW_ESTIMATE);
        auto backward = fftwf_plan_dft_1d(bins,(fftwf_complex*)inverseIn,(fftwf_complex*)inverseOut,FFTW_BACKWARD,FFTW_ESTIMATE);
        for (int pass = 0; pass < 3; ++pass) {
            filter.reset(); history.assign(bins-1,{});
            for (int count : {1,7,256,4096,31}) {
                for (int i = 0; i < count; ++i) input[i] = pass == 0 ? dsp::complex_t{0,0} : dsp::complex_t{noise(random),noise(random)};
                history.insert(history.end(),input.begin(),input.begin()+count);
                for (int i = 0; i < count; ++i) {
                    for (int j = 0; j < bins; ++j) fftIn[j] = history[i+j]*dsp::window::nuttall(j,bins-1);
                    fftwf_execute(forward);
                    // Preserve the existing magnitude/bin-selection kernel.
                    // ARM SIMD magnitude may approximate sqrt differently
                    // from scalar libm near a tie; this test isolates the
                    // replacement of the inverse transform, not that kernel.
                    uint32_t best;
                    volk_32fc_magnitude_32f(amplitude,(lv_32fc_t*)fftOut,bins);
                    volk_32f_index_max_32u(&best,amplitude,bins);
                    inverseIn[best] = fftOut[best];
                    fftwf_execute(backward);
                    expected[i] = inverseOut[bins/2];
                    inverseIn[best] = {0,0};
                }
                filter.process(count,input.data(),actual.data());
                compare(actual.data(),expected.data(),count,2.0e-6,bins);
                history.erase(history.begin(),history.begin()+count);
            }
        }
        fftwf_destroy_plan(forward); fftwf_destroy_plan(backward);
        fftwf_free(fftIn); fftwf_free(fftOut); fftwf_free(inverseIn); fftwf_free(inverseOut);
        dsp::buffer::free(amplitude);
    }
    std::puts("OK: IF NR matches full inverse-FFT oracle for all four presets and packet histories");
    for (int rate : {8000,44100,192000}) {
        for (bool silent : {false,true}) {
            ComplexArray input(rate*28/100);
            for (auto& x : input) x = silent ? dsp::complex_t{0,0} : dsp::complex_t{noise(random),noise(random)};
            Params actual, expected;
            MMSE::logmmse_sample(input,rate,&actual,12);
            MMSE::logmmse_sample(input,rate,&expected,12);
            input.resize(actual.Slen+actual.len2);
            for (int frame = 0; frame < 140; ++frame) {
                for (size_t i = 0; i < input.size(); ++i) {
                    float phase = 2.0*M_PI*700*(frame*actual.len2+i)/rate;
                    input[i] = silent ? dsp::complex_t{0,0} : dsp::complex_t{0.2f*std::cos(phase)+noise(random),0.2f*std::sin(phase)+noise(random)};
                }
                auto a = MMSE::logmmse_all(input,&actual);
                auto b = referenceMMSE(input,expected);
                assert(a.size() == b.size());
                compare(a.data(),b.data(),a.size(),2.0e-5,rate);
                if (silent) for (auto x : a) assert(x.re == 0 && x.im == 0);
            }
            // Incomplete frames must not run/adapt the noise model repeatedly.
            input.resize(actual.Slen);
            auto saved = actual.noise_mu2;
            for (int i = 0; i < 5; ++i) assert(MMSE::logmmse_all(input,&actual).empty());
            assert(saved == actual.noise_mu2);
        }
    }
    std::puts("OK: Baseband NR preserves reference gains/FFT/overlap at 8/44.1/192 kHz; silence stays finite");
}
