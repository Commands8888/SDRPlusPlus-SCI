// Test-only observer for a real first launch with the package's default modules.
#include <core.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <GLFW/glfw3.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <sys/resource.h>

SDRPP_MOD_INFO{"startup_integration", "Startup lifecycle test", "SDRPP-FULL tests", 1, 0, 0, 1};
static EventHandler<MainWindow::FrameDrawArgs> frames;
static int frameCount = 0;
static const auto start = std::chrono::steady_clock::now();
MOD_EXPORT void _INIT_() {
    frames.handler = [](MainWindow::FrameDrawArgs, void*) {
        if (++frameCount == 12) {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
            rusage usage{};
            getrusage(RUSAGE_SELF, &usage);
            std::printf("OK: default startup reached GUI; %zu live modules; %lld ms; peak RSS %ld KB\n",core::moduleManager.instances.size(),(long long)ms,usage.ru_maxrss);
            glfwSetWindowShouldClose(glfwGetCurrentContext(), GLFW_TRUE);
        }
    };
    gui::mainWindow.onFrameDraw.bindHandler(&frames);
}
MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string) { return nullptr; }
MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance*) {}
MOD_EXPORT void _END_() {
    gui::mainWindow.onFrameDraw.unbindHandler(&frames);
    if (!core::moduleManager.instances.empty() || frameCount < 12) std::abort();
    if (!sigpath::sourceManager.getSourceNames().empty()) {
        std::fputs("STARTUP TEST FAILED: destroyed source still registered\n",stderr);
        std::abort();
    }
    std::puts("OK: default startup instances drained and destroyed before module shutdown");
}
