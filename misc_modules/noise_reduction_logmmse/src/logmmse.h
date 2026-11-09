#pragma once

#include "arrays.h"
#include "math.h"
#include "bgnoise.h"
#include <utils/flog.h>
#include <cstring>
#include <deque>
#include <utility>

namespace dsp {

    // ported from https://github.com/rajivpoddar/logmmse/  by sannysanoff

    namespace logmmse {

        using namespace ::dsp::arrays;
        using namespace ::dsp::math;

        struct LogMMSE {

            struct SavedParamsC {
                int noise_history_len() {
                    if (nFFT < 1000) {
                        return 2000;
                    } else {
                        return 200;
                    }
                }

                std::deque<FloatArray> noise_history;      // chunks of nFFT*float
                std::deque<FloatArray> dev_history;
                FloatArray sums;      // sliding sum of last N noise_history
                FloatArray devs;      // sliding sum of dev_history

                FloatArray noise_mu2;
                FloatArray Xk_prev;   // empty until logmmse_sample() has run
                ComplexArray x_old;

                int Slen;
                int PERC;
                int len1;
                int len2;
                FloatArray win;
                int nFFT = 0;
                std::shared_ptr<FFTPlan> forwardPlan;
                std::shared_ptr<FFTPlan> reversePlan;
                float aa = 0.98;
                float ksi_min;
                bool hold = false;
                // Reused per-bin working storage; history still retains the
                // same number of complete, full-bandwidth FFT frames.
                FloatArray gain, integral, weight;
                FloatArray recycledNoise, recycledDeviation;
                FloatArray noiseCandidate, noiseVariance;

                FloatArray acquireNoiseFrame() {
                    FloatArray frame = std::move(recycledNoise);
                    frame.resize(nFFT);
                    return frame;
                }

                void reset() {
                    noise_history.clear();
                    dev_history.clear();
                    Xk_prev.clear();
                    noise_mu2.clear();
                    x_old.clear();
                    backgroundNoiseCalculator.reset();
                }

                // Takes the frame by value: callers pass std::move once they
                // are done with the buffer, so the deque adopts it without a
                // deep copy.
                void add_noise_history(FloatArray noise) {
                    if (hold) {
                        recycledNoise = std::move(noise);
                        return;
                    }
                    if (noise.size() != nFFT) {
                        flog::info("ERROR noise.size() != nFFT: {} {}", (int)noise.size(), nFFT);
                        return;
                    }
                    noise_history.emplace_back(std::move(noise));
                    // deque guarantees back() stays valid through the
                    // pop_front() trimming below (history length >= 200).
                    const FloatArray& latest = noise_history.back();
                    volk_32f_x2_add_32f(sums.data(), sums.data(), latest.data(), nFFT);
                    while (noise_history.size() > noise_history_len()) {
                        volk_32f_x2_subtract_32f(sums.data(), sums.data(), noise_history.front().data(), nFFT);
                        recycledNoise = std::move(noise_history.front());
                        noise_history.pop_front();
                    }

                    FloatArray diff = std::move(recycledDeviation);
                    diff.resize(nFFT);
                    const float scale = 1.0f / noise_history.size();
                    for (int i = 0; i < nFFT; ++i) {
                        float delta = latest[i] - sums[i] * scale;
                        diff[i] = delta * delta;
                    }

                    volk_32f_x2_add_32f(devs.data(), devs.data(), diff.data(), nFFT);
                    dev_history.emplace_back(std::move(diff));

                    while (dev_history.size() > noise_history_len()) {
                        volk_32f_x2_subtract_32f(devs.data(), devs.data(), dev_history.front().data(), nFFT);
                        recycledDeviation = std::move(dev_history.front());
                        dev_history.pop_front();
                    }

                }

                BackgroundNoiseCalculator backgroundNoiseCalculator;

                void update_noise_mu2() {
                    auto nframes = noise_history.size();
                    if (nframes > 100 && !hold) {
                        const float scale = 1.0f / nframes;
                        noiseCandidate.resize(nFFT);
                        noiseVariance.resize(nFFT);
                        volk_32f_s32f_multiply_32f(noiseVariance.data(), devs.data(), scale, nFFT);
                        volk_32f_x2_multiply_32f(noiseVariance.data(), noiseVariance.data(), noiseVariance.data(), nFFT);
                        int radius = nFFT * 15 / 100;
                        if (radius > 0) {
                            std::fill(noiseVariance.begin() + nFFT/2 - radius + 1,
                                noiseVariance.begin() + nFFT/2 + radius, BackgroundNoiseCalculator::ERASED_SAMPLE);
                        }
                        float threshold = backgroundNoiseCalculator.addFrame(noiseVariance);
                        for (int i = 0; i < nFFT; ++i) {
                            float average = sums[i] * scale;
                            noiseCandidate[i] = noiseVariance[i] < threshold ? average * average : 0.0f;
                        }
                        // Build a candidate instead of copying the old model
                        // merely to restore it if interpolation has no anchors.
                        if (linearInterpolateHoles(noiseCandidate.data(), nFFT)) {
                            noise_mu2.swap(noiseCandidate);
                        }
                    }
                }
            };

            static void logmmse_sample(const ComplexArray &x, int Srate, SavedParamsC *params, int noise_frames) {
                params->Slen = floor(0.02 * Srate);
                if (params->Slen % 2 == 1) params->Slen++;
                params->PERC = 50;
                params->len1 = floor(params->Slen * params->PERC / 100);
                params->noise_history.clear();
                params->dev_history.clear();
                params->len2 = params->Slen - params->len1;         // len1+len2
                params->win = nphanning(params->Slen);
                params->win = div(mul(params->win, params->len2), npsum(params->win));
                int fftSize = 2 * params->Slen;
                if (params->nFFT != fftSize || !params->forwardPlan || !params->reversePlan) {
                    params->forwardPlan = allocateFFTWPlan(false, fftSize);
                    params->reversePlan = allocateFFTWPlan(true, fftSize);
                }
                params->nFFT = fftSize;
                params->sums = npzeros(params->nFFT);
                params->devs = npzeros(params->nFFT);
                params->gain.resize(params->nFFT);
                params->integral.resize(params->nFFT);
                params->weight.resize(params->nFFT);

                flog::debug("logmmse: sampling noise, srate={}, Slen={}, nFFT={}", Srate, params->Slen, params->nFFT);
                auto noise_mean = npzeros(params->nFFT);
                for (int j = 0; j < params->Slen * noise_frames; j += params->Slen) {
                    params->forwardPlan->transformWeighted(x.data() + j, params->win.data(), params->Slen);
                    auto noise = params->acquireNoiseFrame();
                    volk_32fc_magnitude_32f(noise.data(), (const lv_32fc_t*)params->forwardPlan->getOutput().data(), params->nFFT);
                    volk_32f_x2_add_32f(noise_mean.data(), noise_mean.data(), noise.data(), params->nFFT);
                    params->add_noise_history(std::move(noise));
                }
                params->noise_mu2 = div(noise_mean, noise_frames);
                params->noise_mu2 = npmavg(params->noise_mu2, 120);
                params->noise_mu2 = muleach(params->noise_mu2, params->noise_mu2);
                // A silent source has exactly zero estimated noise power;
                // keep the estimator finite instead of feeding 0/0 into it.
                for (auto& power : params->noise_mu2) { power = std::max(power, 1.0e-20f); }
                params->Xk_prev = npzeros(params->len1);
                params->x_old = npzeros_c(params->len1);
                params->ksi_min = ::pow(10, -25.0 / 10.0);
            }

            // Write directly to a caller's fixed output buffer. The estimator
            // advances once per 10 ms hop, independent of upstream packet sizes.
            static int processInto(const complex_t* x, int inputLen, SavedParamsC* params,
                                   complex_t* output, int capacity, float outputGain = 1.0f) {
                int frames = std::min(std::max(0, inputLen / params->len2 - 2), capacity / params->len2);
                for (int k = 0; k < frames * params->len2; k += params->len2) {
                    params->update_noise_mu2();
                    params->forwardPlan->transformWeighted(x + k, params->win.data(), params->Slen);
                    const auto& spec = params->forwardPlan->getOutput();
                    auto sig = params->acquireNoiseFrame();
                    volk_32fc_magnitude_32f(sig.data(), (const lv_32fc_t*)spec.data(), params->nFFT);
                    auto sigD = sig.data();
                    for (auto z = 1; z < sig.size(); z++) {
                        if (sigD[z] == 0) {
                            sigD[z] = sigD[z - 1];      // for some reason fft returns 0 instead if small value
                        }
                    }
                    const bool usePrevious = npall(params->Xk_prev);
                    // Same decision-directed estimator, bounds, E1 table and
                    // expfast kernel. Fuse temporary-vector arithmetic into
                    // one pass, keeping the original float operation order.
                    for (int i = 0; i < params->nFFT; ++i) {
                        float gamma = (sigD[i] * sigD[i]) / params->noise_mu2[i];
                        if (gamma > 40.0f) { gamma = 40.0f; }
                        float excess = gamma - 1.0f;
                        if (excess < 0.0f) { excess = 0.0f; }
                        float ksi = excess * (1.0f - params->aa);
                        if (usePrevious) {
                            ksi += (params->Xk_prev[i] * params->aa) / params->noise_mu2[i];
                            if (ksi < params->ksi_min) { ksi = params->ksi_min; }
                        }
                        else { ksi += params->aa; }
                        float a = ksi / (ksi + 1.0f);
                        params->gain[i] = a;
                        params->integral[i] = a * gamma;
                    }
                    // Keep the scalar E1 lookup separate from the gain arithmetic.
                    for (int i = 0; i < params->nFFT; ++i) {
                        params->integral[i] = dsp::math::expn(params->integral[i]) * 0.5f;
                    }
                    volk_32f_expfast_32f(params->weight.data(), params->integral.data(), params->nFFT);
                    volk_32f_x2_multiply_32f(params->weight.data(), params->gain.data(), params->weight.data(), params->nFFT);
                    params->Xk_prev.resize(params->nFFT);
                    for (int i = 0; i < params->nFFT; ++i) {
                        float value = sigD[i] * params->weight[i];
                        params->Xk_prev[i] = value * value;
                    }
                    // Deferred from right after the hole-filling loop: this is
                    // past sig's last read, so the history can adopt the
                    // buffer instead of copying it. The intervening math does
                    // not touch sums/devs/noise_history, so the ordering is
                    // equivalent.
                    params->add_noise_history(std::move(sig));
                    params->reversePlan->transformWeighted(spec.data(), params->weight.data(), spec.size());
                    const auto& xi_w0 = params->reversePlan->getOutput();
                    volk_32fc_x2_add_32fc((lv_32fc_t*)(output + k), (const lv_32fc_t*)params->x_old.data(), (const lv_32fc_t*)xi_w0.data(), params->len1);
                    if (outputGain != 1.0f) {
                        volk_32f_s32f_multiply_32f((float*)(output + k), (const float*)(output + k), outputGain, params->len1 * 2);
                    }
                    std::copy(xi_w0.begin() + params->len1, xi_w0.begin() + params->Slen, params->x_old.begin());
                }
                return frames * params->len2;
            }

            // Compatibility adapter for batch callers and numerical oracles.
            static ComplexArray logmmse_all(const ComplexArray& x, SavedParamsC* params, int maxInput = -1) {
                int inputLen = maxInput < 0 ? (int)x.size() : std::min<int>(x.size(), maxInput);
                int count = std::max(0, inputLen / params->len2 - 2) * params->len2;
                ComplexArray output(count);
                if (count) { processInto(x.data(), inputLen, params, output.data(), count); }
                return output;
            }

        };

    }
}
