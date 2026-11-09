// Test-only module: drives the real Radio interface from the GUI thread.
// It is compiled into a temporary directory, never installed in packages.
#include <core.h>
#include <gui/gui.h>
#include <radio_interface.h>
#include <signal_path/signal_path.h>
#include <dsp/sink/handler_sink.h>
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

class ModeIntegration : public ModuleManager::Instance {
public:
    ModeIntegration() {
        handler.stream = &iq;
        handler.ctx = this;
        handler.selectHandler = [](void*) { core::setInputSampleRate(1000000.0); };
        handler.deselectHandler = [](void*) {};
        handler.menuHandler = [](void*) {};
        handler.tuneHandler = [](double, void*) {};
        handler.startHandler = [](void* ctx) { static_cast<ModeIntegration*>(ctx)->startIQ(); };
        handler.stopHandler = [](void* ctx) { static_cast<ModeIntegration*>(ctx)->stopIQ(); };
        sigpath::sourceManager.registerSource("Mode test IQ", &handler);
        SinkManager::SinkProvider sink;
        sink.ctx = this;
        sink.create = [](SinkManager::Stream* stream, std::string name, void* ctx) {
            static_cast<ModeIntegration*>(ctx)->audioStream = stream;
            return SinkManager::NullSink::create(stream, name, nullptr);
        };
        sigpath::sinkManager.registerSinkProvider("Mode test sink", sink);
        frames.handler = [](MainWindow::FrameDrawArgs, void* ctx) { static_cast<ModeIntegration*>(ctx)->frame(); };
        frames.ctx = this;
        gui::mainWindow.onFrameDraw.bindHandler(&frames);
        const int ids[] = {0, 1, 2, 3, 7, 6, 4, 5, 8, 0x1301, 0x1302};
        nrTest = std::getenv("SDRPP_TEST_NR") != nullptr;
        for (int round = 0; round < (nrTest ? 1 : 4); ++round)
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
        worker = std::thread([this] {
            double phase = 0.0;
            while (!stopping.load()) {
                for (int i = 0; i < 8192; ++i) {
                    phase += 0.02;
                    if (phase > 6.283185307) phase -= 6.283185307;
                    iq.writeBuf[i] = {0.4f * (float)std::cos(phase), 0.4f * (float)std::sin(phase)};
                }
                if (!iq.swap(8192)) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
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
            }, this);
            audio.start();
            gui::mainWindow.setPlayState(true);
            startedAt = std::chrono::steady_clock::now();
            cpuAtStart = std::clock();
            started = true;
        }
        // Let the actual UI/status drawing and DSP pipeline run between changes.
        if (++waitFrames < 3) return;
        waitFrames = 0;
        if (invalidAudio.load()) fail("non-finite audio sample");
        // Exercise actual audio-device rate changes and HPF coefficient ownership.
        if (position == sequence.size() / 4) audioStream->setSampleRate(44100);
        if (position == sequence.size() * 3 / 8) audioStream->setSampleRate(96000);
        if (position == sequence.size()) {
            if (!audioBuffers.load()) fail("Radio produced no audio buffers");
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
    dsp::stream<dsp::stereo_t>* audioInput = nullptr;
    SinkManager::Stream* audioStream = nullptr;
    dsp::sink::Handler<dsp::stereo_t> audio;
    std::thread worker;
    std::vector<int> sequence;
    std::vector<long long> latencies;
    size_t position = 0;
    int waitFrames = 0;
    long warmRSS = 0;
    bool started = false, finished = false;
    bool nrTest = false;
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
