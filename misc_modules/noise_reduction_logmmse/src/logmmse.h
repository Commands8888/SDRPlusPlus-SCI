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
                int nFFT;
                std::shared_ptr<FFTPlan> forwardPlan;
                std::shared_ptr<FFTPlan> reversePlan;
                float aa = 0.98;
                float ksi_min;
                bool hold = false;
                // Reused per-bin working storage; history still retains the
                // same number of complete, full-bandwidth FFT frames.
                FloatArray gain, integral, weight;

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
                        noise_history.pop_front();
                    }

                    FloatArray diff(nFFT);
                    const float scale = 1.0f / noise_history.size();
                    for (int i = 0; i < nFFT; ++i) {
                        float delta = latest[i] - sums[i] * scale;
                        diff[i] = delta * delta;
                    }

                    volk_32f_x2_add_32f(devs.data(), devs.data(), diff.data(), nFFT);
                    dev_history.emplace_back(std::move(diff));

                    while (dev_history.size() > noise_history_len()) {
                        volk_32f_x2_subtract_32f(devs.data(), devs.data(), dev_history.front().data(), nFFT);
                        dev_history.pop_front();
                    }

                }

                BackgroundNoiseCalculator backgroundNoiseCalculator;

                void update_noise_mu2() {
                    auto nframes = noise_history.size();
                    if (nframes > 100 && !hold) {
                        auto noise_mu2_copy = noise_mu2;

                        auto noiseAvg = mul(sums, 1 / (float)nframes);

                        auto hi = mul(devs, 1 / (float)nframes);
                        auto devSquare = muleach(hi, hi);
                        auto devSquareD = devSquare.data();
                        for (int z = 0; z < nFFT; z++) {
                            if (abs(z - nFFT/2) < nFFT * 15 / 100) {
                                // after fft, rightmost and leftmost sides of real frequencies range are at the center of the resulting table.
                                // We exclude middle of the table from lookup
                                devSquareD[z] = BackgroundNoiseCalculator::ERASED_SAMPLE;
                            }
                        }
                        memset(noise_mu2.data(), 0, nFFT*sizeof(float));
                        float detectedNoise = backgroundNoiseCalculator.addFrame(devSquare);
                        auto acceptible_stdev = detectedNoise;
                        auto nmu2 = noise_mu2.data();
                        auto navg =  noiseAvg.data();
                        for(int q=0; q < nFFT; q++) {
                            if (devSquareD[q] < acceptible_stdev) {
                                nmu2[q] = navg[q] * navg[q];
                            }
                        }

                        if (!linearInterpolateHoles(nmu2, nFFT)) {
                            noise_mu2 = std::move(noise_mu2_copy);
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
                params->nFFT = 2 * params->Slen;
                params->forwardPlan = allocateFFTWPlan(false, params->nFFT);
                params->reversePlan = allocateFFTWPlan(true, params->nFFT);
                params->sums = npzeros(params->nFFT);
                params->devs = npzeros(params->nFFT);
                params->gain.resize(params->nFFT);
                params->integral.resize(params->nFFT);
                params->weight.resize(params->nFFT);

                flog::debug("logmmse: sampling noise, srate={}, Slen={}, nFFT={}", Srate, params->Slen, params->nFFT);
                auto noise_mean = npzeros(params->nFFT);
                for (int j = 0; j < params->Slen * noise_frames; j += params->Slen) {
                    params->forwardPlan->transformWeighted(x.data() + j, params->win.data(), params->Slen);
                    auto noise = npabsolute(params->forwardPlan->getOutput());
                    noise_mean = addeach(noise_mean, noise);
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

            // maxInput caps how much of x is consumed in one pass (-1 = all of it),
            // so a large backlog can be drained in bounded chunks without copying.
            static ComplexArray logmmse_all(const ComplexArray &x, SavedParamsC *params, int maxInput = -1) {
                int inputLen = (int)x.size();
                if (maxInput >= 0 && maxInput < inputLen) {
                    inputLen = maxInput;
                }
                auto Nframes = floor(inputLen / params->len2) - floor(params->Slen / params->len2);
                if (Nframes <= 0) { return {}; }
                params->update_noise_mu2();
                auto xfinal = npzeros_c(Nframes * params->len2);
                for (int k = 0; k < Nframes * params->len2; k += params->len2) {
                    params->forwardPlan->transformWeighted(x.data() + k, params->win.data(), params->Slen);
                    const auto& spec = params->forwardPlan->getOutput();
                    auto sig = npabsolute(spec);
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
                    // Keep the scalar E1 lookup separate so the estimator
                    // arithmetic above can be vectorized by the compiler.
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
                    volk_32fc_x2_add_32fc((lv_32fc_t*)(xfinal.data() + k), (const lv_32fc_t*)params->x_old.data(), (const lv_32fc_t*)xi_w0.data(), params->len1);
                    std::copy(xi_w0.begin() + params->len1, xi_w0.begin() + params->Slen, params->x_old.begin());
                }
                return xfinal;
            }

        };

    }
}
