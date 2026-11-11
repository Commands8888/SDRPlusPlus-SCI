// Test-only module: drives the real Radio interface from the GUI thread.
// It is compiled into a temporary directory, never installed in packages.
#include <core.h>
#include <gui/gui.h>
#include <radio_interface.h>
#include <signal_path/signal_path.h>
#include <dsp/sink/handler_sink.h>
#include <dsp/sink/audio_buffer.h>
#include "../source_modules/rtl_sdr_source/src/rtl_iq_buffer.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <thread>
#include <vector>
#include <ctime>
#include <cstdlib>
#include <sys/resource.h>
#include <unistd.h>

SDRPP_MOD_INFO{"mode_integration", "Radio mode integration test", "SDRPP-FULL tests", 1, 0, 0, 1};

static std::atomic<uint64_t> deviceFrames { 0 };
static std::atomic<uint64_t> deviceMissingFrames { 0 };
class SimulatedAudioSink : public SinkManager::Sink {
public:
    SimulatedAudioSink(SinkManager::Stream* stream) : stream(stream),rate(stream->getSampleRate()) {
        buffer.init(stream->sinkOut);
        rateHandler.ctx=this;
        rateHandler.handler=[](float rate,void* ctx) {
            auto* self=static_cast<SimulatedAudioSink*>(ctx);
            bool restart=self->running;
            self->stop();self->rate=rate;
            if(restart) self->start();
        };
        stream->srChange.bindHandler(&rateHandler);
    }
    ~SimulatedAudioSink() override { stop();stream->srChange.unbindHandler(&rateHandler); }
    void start() override {
        if(running) return;
        const bool burstTest=std::getenv("SDRPP_TEST_CONTINUITY")!=nullptr;
        unsigned frames=burstTest?rate/60:rate/100;
        buffer.configure(rate,frames);buffer.start();
        stopping=false;running=true;
        device=std::thread([this,frames,burstTest] {
            std::vector<dsp::stereo_t> out(frames);
            auto next=std::chrono::steady_clock::now();
            while(!stopping.load()) {
                const unsigned batch=burstTest?4:1;
                for(unsigned period=0;period<batch;++period) {
                    unsigned got=buffer.renderStereo(out.data(),frames);
                    deviceFrames+=got;
                    deviceMissingFrames+=frames-got;
                    for(auto x:out) if(!std::isfinite(x.l)||!std::isfinite(x.r)) std::abort();
                }
                next+=std::chrono::nanoseconds(uint64_t(frames)*batch*1000000000/rate);
                std::this_thread::sleep_until(next);
            }
        });
    }
    void stop() override {
        if(!running) return;
        buffer.stop();stopping=true;
        device.join();running=false;
    }
    void menuHandler() override {}
private:
    SinkManager::Stream* stream;
    unsigned rate;
    bool running=false;
    std::atomic<bool> stopping { false };
    dsp::sink::AudioBuffer buffer;
    std::thread device;
    EventHandler<float> rateHandler;
};

class ModeIntegration : public ModuleManager::Instance {
public:
    ModeIntegration() {
        handler.stream = &iq;
        handler.ctx = this;
        handler.selectHandler = [](void*) { core::setInputSampleRate(std::getenv("SDRPP_TEST_NR") ? 2400000.0 : 1000000.0); };
        handler.deselectHandler = [](void*) {};
        handler.menuHandler = [](void*) {};
        handler.tuneHandler = [](double, void*) {};
        handler.startHandler = [](void* ctx) { static_cast<ModeIntegration*>(ctx)->startIQ(); };
        handler.stopHandler = [](void* ctx) { static_cast<ModeIntegration*>(ctx)->stopIQ(); };
        sigpath::sourceManager.registerSource("Mode test IQ", &handler);
        SinkManager::SinkProvider sink;
        sink.ctx = this;
        sink.create = [](SinkManager::Stream* stream, std::string name, void* ctx) -> SinkManager::Sink* {
            static_cast<ModeIntegration*>(ctx)->audioStream = stream;
            return new SimulatedAudioSink(stream);
        };
        sigpath::sinkManager.registerSinkProvider("Mode test sink", sink);
        frames.handler = [](MainWindow::FrameDrawArgs, void* ctx) { static_cast<ModeIntegration*>(ctx)->frame(); };
        frames.ctx = this;
        gui::mainWindow.onFrameDraw.bindHandler(&frames);
        const int ids[] = {0, 1, 2, 3, 7, 6, 4, 5, 8, 0x1301, 0x1302};
        nrTest = std::getenv("SDRPP_TEST_NR") != nullptr;
        continuityTest=std::getenv("SDRPP_TEST_CONTINUITY")!=nullptr;
        if(continuityTest) sequence.assign(std::begin(ids),std::end(ids));
        else for (int round = 0; round < (nrTest ? 1 : 4); ++round)
            for (int from : ids)
                for (int to : ids)
                    if (from != to) { sequence.push_back(from); sequence.push_back(to); }
    }
    ~ModeIntegration() {
        if (worker.joinable()) fail("core did not stop the active source before deleting it");
        detach();
        sigpath::sourceManager.unregisterSource("Mode test IQ");
        sigpath::sinkManager.unregisterSinkProvider("Mode test sink");
        std::puts("OK: active source stopped before controller teardown");
    }
    void postInit() override {}
    void enable() override {}
    void disable() override {}
    bool isEnabled() override { return true; }
    void stopIQ() {
        if (!worker.joinable()) return;
        stopping = true;
        iq.stopWriter();
        if (nrTest) rtlIngress.stop();
        worker.join();
        iq.clearWriteStop();
    }
    void detach() { gui::mainWindow.onFrameDraw.unbindHandler(&frames); stopAudio(); stopIQ(); }
private:
    void stopAudio() {
        if (!audioInput) return;
        audio.stop();
        sigpath::sinkManager.unbindStream("Radio", audioInput);
        audioInput = nullptr;
    }
    void startIQ() {
        if (worker.joinable()) return;
        stopping = false;
        if (nrTest) rtlIngress.start(&iq, 16384);
        worker = std::thread([this] {
            double phase = 0.0;
            std::vector<unsigned char> raw(16384);
            auto next = std::chrono::steady_clock::now();
            while (!stopping.load()) {
                {
                    // Model bulk callbacks and hardware controls sharing a USB
                    // event context. Only the NR run uses production RTL ingress.
                    std::lock_guard<std::mutex> event(usbEvents);
                    for (int i = 0; i < 8192; ++i) {
                        phase += 0.02;
                        if (phase > 6.283185307) phase -= 6.283185307;
                        if (nrTest) {
                            raw[i*2] = 128 + int(50*std::cos(phase));
                            raw[i*2+1] = 128 + int(50*std::sin(phase));
                        }
                        else iq.writeBuf[i] = {0.4f * (float)std::cos(phase), 0.4f * (float)std::sin(phase)};
                    }
                    if (nrTest) rtlIngress.push(raw.data(), raw.size());
                    else if (!iq.swap(8192)) break;
                }
                if (nrTest) {
                    next += std::chrono::nanoseconds(3413333); // 8192 IQ at 2.4 MHz.
                    std::this_thread::sleep_until(next);
                }
                else std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        });
    }
    static long rssKB() {
        long total = 0, resident = 0;
        std::ifstream stat("/proc/self/statm");
        stat >> total >> resident;
        return resident * sysconf(_SC_PAGESIZE) / 1024;
    }
    void fail(const char* reason) {
        std::fprintf(stderr, "MODE TEST FAILED: %s\n", reason);
        std::fflush(stderr);
        std::_Exit(2);
    }
    void checkContinuity() {
        using Clock=std::chrono::steady_clock;
        const auto now=Clock::now();
        if(!qualityConfigured) {
            if(position==sequence.size()) {
                std::printf("OK: GUI continuity checked nine analog modes for inserted silence and eleven modes for FFT/control liveness with 2.4 MHz NR and four-period audio bursts\n");
                finished=true;
                glfwSetWindowShouldClose(glfwGetCurrentContext(),GLFW_TRUE);
                return;
            }
            int mode=sequence[position];
            auto begin=Clock::now();
            { std::lock_guard<std::mutex> command(usbEvents); }
            if(Clock::now()-begin>std::chrono::milliseconds(100)) fail("USB-style control stalled during continuity test");
            if(!core::modComManager.callInterface("Radio",RADIO_IFACE_CMD_SET_MODE,&mode,nullptr)) fail("continuity mode command failed");
            qualityModeAt=Clock::now();
            qualityConfigured=true;qualityCollecting=false;
            return;
        }
        if(now-qualityModeAt<std::chrono::milliseconds(600)) return;
        auto fft=gui::waterfall.getFFTFrameIndex();
        if(!qualityCollecting) {
            qualityFrames=deviceFrames.load();qualityMissing=deviceMissingFrames.load();
            audioPacketMax=0;
            qualityFFT=0;lastQualityFFT=fft;qualityDropped=rtlIngress.droppedPackets();
            qualityAt=lastQualityFFTAt=now;qualityMaxFFTGap=0;qualityCollecting=true;
            return;
        }
        if(fft!=lastQualityFFT) {
            ++qualityFFT;
            auto gap=std::chrono::duration_cast<std::chrono::microseconds>(now-lastQualityFFTAt).count();
            qualityMaxFFTGap=std::max<long long>(qualityMaxFFTGap,gap);
            lastQualityFFT=fft;lastQualityFFTAt=now;
        }
        if(now-qualityAt<std::chrono::milliseconds(2500)) return;
        auto frames=deviceFrames.load()-qualityFrames;
        auto missing=deviceMissingFrames.load()-qualityMissing;
        auto dropped=rtlIngress.droppedPackets()-qualityDropped;
        std::printf("GUI continuity mode=%d audio frames=%llu missing=%llu FFT frames=%llu maximum FFT gap=%lld us IQ drops=%llu largest audio packet=%u\n",sequence[position],
            (unsigned long long)frames,(unsigned long long)missing,(unsigned long long)qualityFFT,qualityMaxFFTGap,(unsigned long long)dropped,audioPacketMax.load());
        std::fflush(stdout);
        // With a non-digital input DSD/oldDSD need not synthesize speech. Their
        // source, controls and FFT must keep running while the decoder waits.
        if(sequence[position]<0x1300 && (!frames || missing)) fail("inserted silence during steady analog playback");
        if(qualityFFT<3 || qualityMaxFFTGap>200000 || now-lastQualityFFTAt>std::chrono::milliseconds(200)) fail("FFT publication stalled during playback");
        if(dropped) fail("IQ packet loss during steady continuity test");
        ++position;qualityConfigured=false;
    }
    void frame() {
        if (finished) return;
        if (!started) {
            if (!core::modComManager.interfaceExists("Radio")) fail("Radio interface missing");
            if (nrTest) {
                auto* nr = core::moduleManager.instances.at("Noise Reduction").instance;
                nr->disable(); nr->enable();
                for (int i = 0; i < 2; ++i) {
                    if (core::moduleManager.createInstance("Temporary radio", "radio") != 0) fail("temporary radio creation failed");
                    core::moduleManager.postInit("Temporary radio");
                    if (core::moduleManager.deleteInstance("Temporary radio") != 0) fail("temporary radio removal failed");
                }
                std::puts("OK: NR detached from deleted radios and attached again to reused names");
            }
            sigpath::sinkManager.setStreamSink("Radio", "Mode test sink");
            if (!audioStream) fail("test sink missing");
            audioInput = sigpath::sinkManager.bindStream("Radio");
            if (!audioInput) fail("Radio audio stream missing");
            audio.init(audioInput, [](dsp::stereo_t* data, int count, void* ctx) {
                auto* self = static_cast<ModeIntegration*>(ctx);
                for (int i = 0; i < count; ++i)
                    if (!std::isfinite(data[i].l) || !std::isfinite(data[i].r)) self->invalidAudio = true;
                self->audioBuffers++;
                unsigned largest=self->audioPacketMax.load();
                while(unsigned(count)>largest && !self->audioPacketMax.compare_exchange_weak(largest,unsigned(count))) {}
            }, this);
            audio.start();
            gui::mainWindow.setPlayState(true);
            startedAt = std::chrono::steady_clock::now();
            cpuAtStart = std::clock();
            started = true;
        }
        if(continuityTest) { checkContinuity();return; }
        // Let the actual UI/status drawing and DSP pipeline run between changes.
        if (++waitFrames < 3) return;
        waitFrames = 0;
        if (invalidAudio.load()) fail("non-finite audio sample");
        // Exercise actual audio-device rate changes and HPF coefficient ownership.
        if (position == sequence.size() / 4) audioStream->setSampleRate(44100);
        if (position == sequence.size() * 3 / 8) audioStream->setSampleRate(96000);
        if (position == sequence.size()) {
            if (!audioBuffers.load()) fail("Radio produced no audio buffers");
            if (!deviceFrames.load()) fail("simulated audio device received no frames");
            std::printf("OK: real-time audio sink consumed %llu frames during mode/rate changes\n",(unsigned long long)deviceFrames.load());
            if (nrTest) {
                std::sort(controlLatencies.begin(), controlLatencies.end());
                std::printf("OK: RTL-style GUI controls during NR/mode changes; count=%zu p95=%lld us max=%lld us; overload packets=%llu\n",
                    controlLatencies.size(), controlLatencies[controlLatencies.size()*95/100], controlLatencies.back(),
                    (unsigned long long)rtlIngress.droppedPackets());
                if (controlLatencies.back() > 100000) fail("USB-style control stalled the GUI for over 100 ms");
            }
            std::sort(latencies.begin(), latencies.end());
            std::printf("OK: GUI RadioModule all 110 directed pairs %s; %zu switches; %d audio buffers; p95=%lld us max=%lld us; RSS warm=%ld KB final=%ld KB\n",
                nrTest ? "with Noise Reduction" : "four times", position, audioBuffers.load(), latencies[latencies.size() * 95 / 100], latencies.back(), warmRSS, rssKB());
            rusage usage{};
            getrusage(RUSAGE_SELF, &usage);
            const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now()-startedAt).count();
            const double cpu = double(std::clock()-cpuAtStart)/CLOCKS_PER_SEC;
            std::printf("OK: GUI workload wall=%.3f s process CPU=%.3f s (%.1f%% of one core); peak RSS=%ld KB\n",wall,cpu,100.0*cpu/wall,usage.ru_maxrss);
            std::fflush(stdout);
            finished = true;
            // Leave source, audio and Radio alive: normal application shutdown
            // must stop/join them in the correct order, rather than the test.
            glfwSetWindowShouldClose(glfwGetCurrentContext(), GLFW_TRUE);
            return;
        }
        if (nrTest && position && position % 44 == 0) {
            auto* nr = core::moduleManager.instances.at("Noise Reduction").instance;
            nr->disable(); nr->enable();
        }
        int mode = sequence[position];
        if (nrTest) {
            auto begin = std::chrono::steady_clock::now();
            { std::lock_guard<std::mutex> command(usbEvents); }
            controlLatencies.push_back(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-begin).count());
        }
        auto begin = std::chrono::steady_clock::now();
        if (!core::modComManager.callInterface("Radio", RADIO_IFACE_CMD_SET_MODE, &mode, nullptr)) fail("mode command failed");
        int selected = -1;
        core::modComManager.callInterface("Radio", RADIO_IFACE_CMD_GET_MODE, nullptr, &selected);
        if (selected != mode) fail("selected mode mismatch");
        float bandwidth = 0;
        core::modComManager.callInterface("Radio", RADIO_IFACE_CMD_GET_BANDWIDTH, nullptr, &bandwidth);
        if (!std::isfinite(bandwidth) || bandwidth <= 0) fail("invalid bandwidth after switch");
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - begin).count();
        latencies.push_back(elapsed);
        ++position;
        if (position == sequence.size() / 2) warmRSS = rssKB();
        std::printf("GUI switch %zu: mode=%d bandwidth=%.0f time=%lld us\n", position, mode, bandwidth, (long long)elapsed);
        std::fflush(stdout);
    }
    dsp::stream<dsp::complex_t> iq;
    SourceManager::SourceHandler handler{};
    EventHandler<MainWindow::FrameDrawArgs> frames;
    std::atomic<bool> stopping{false};
    std::atomic<bool> invalidAudio{false};
    std::atomic<int> audioBuffers{0};
    std::atomic<unsigned> audioPacketMax{0};
    dsp::stream<dsp::stereo_t>* audioInput = nullptr;
    SinkManager::Stream* audioStream = nullptr;
    dsp::sink::Handler<dsp::stereo_t> audio;
    std::thread worker;
    RTLSDRIQBuffer rtlIngress;
    std::mutex usbEvents;
    std::vector<long long> controlLatencies;
    std::vector<int> sequence;
    std::vector<long long> latencies;
    size_t position = 0;
    int waitFrames = 0;
    long warmRSS = 0;
    bool started = false, finished = false;
    bool nrTest = false;
    bool continuityTest=false,qualityConfigured=false,qualityCollecting=false;
    uint64_t qualityFrames=0,qualityMissing=0,qualityFFT=0,lastQualityFFT=0,qualityDropped=0;
    long long qualityMaxFFTGap=0;
    std::chrono::steady_clock::time_point qualityModeAt,qualityAt,lastQualityFFTAt;
    std::chrono::steady_clock::time_point startedAt;
    std::clock_t cpuAtStart = 0;
};
static ModeIntegration* controller = nullptr;
MOD_EXPORT void _INIT_() {}
MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string) { controller = new ModeIntegration; return controller; }
MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* instance) { delete instance; controller = nullptr; }
MOD_EXPORT void _END_() {
    if (controller) { std::fputs("MODE TEST FAILED: live instance at module shutdown\n",stderr); std::abort(); }
    std::puts("OK: GUI Radio and controller instances destroyed before module shutdown");
}
