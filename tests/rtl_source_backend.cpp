// Exercise the actual source start/callback/control/stop paths with a fake
// RTL device. USB bulk and control operations serialize as in libusb.
#include <utils/flog.h>
#include <module.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <core.h>
#include <gui/style.h>
#include <config.h>
#include <gui/smgui.h>
#include <rtl-sdr.h>
#include "../source_modules/rtl_sdr_source/src/rtl_iq_buffer.h"
#include <cassert>
#include <cstdio>

// All dependencies above are already parsed. Expose only the source's private
// implementation to this executable, without adding an installed test API.
#define private public
#include "../source_modules/rtl_sdr_source/src/main.cpp"
#undef private

static std::mutex usbEvents;
static std::atomic<bool> cancelled { false };
static int openDevices = 0;
static uint32_t centerFrequency = 0;
static int control() { std::lock_guard<std::mutex> event(usbEvents);return 0; }

extern "C" {
uint32_t rtlsdr_get_device_count() { return 1; }
const char* rtlsdr_get_device_name(uint32_t) { return "Test RTL"; }
int rtlsdr_get_device_usb_strings(uint32_t, char* vendor, char* product, char* serial) {
    std::strcpy(vendor,"Test");std::strcpy(product,"RTL");std::strcpy(serial,"123");return 0;
}
int rtlsdr_open(rtlsdr_dev_t** dev, uint32_t) { *dev=reinterpret_cast<rtlsdr_dev_t*>(1);cancelled=false;++openDevices;return 0; }
int rtlsdr_close(rtlsdr_dev_t*) { --openDevices;return 0; }
int rtlsdr_get_tuner_gains(rtlsdr_dev_t*, int* gains) { gains[0]=0;gains[1]=100;gains[2]=200;return 3; }
int rtlsdr_set_sample_rate(rtlsdr_dev_t*, uint32_t) { return control(); }
int rtlsdr_set_center_freq(rtlsdr_dev_t*, uint32_t frequency) { int result=control();centerFrequency=frequency;return result; }
uint32_t rtlsdr_get_center_freq(rtlsdr_dev_t*) { return centerFrequency; }
int rtlsdr_set_freq_correction(rtlsdr_dev_t*, int) { return control(); }
int rtlsdr_set_tuner_bandwidth(rtlsdr_dev_t*, uint32_t) { return control(); }
int rtlsdr_set_direct_sampling(rtlsdr_dev_t*, int) { return control(); }
int rtlsdr_set_bias_tee(rtlsdr_dev_t*, int) { return control(); }
int rtlsdr_set_agc_mode(rtlsdr_dev_t*, int) { return control(); }
int rtlsdr_set_tuner_gain(rtlsdr_dev_t*, int) { return control(); }
int rtlsdr_set_tuner_gain_mode(rtlsdr_dev_t*, int) { return control(); }
int rtlsdr_set_offset_tuning(rtlsdr_dev_t*, int) { return control(); }
int rtlsdr_reset_buffer(rtlsdr_dev_t*) { return control(); }
int rtlsdr_cancel_async(rtlsdr_dev_t*) { cancelled=true;return 0; }
int rtlsdr_read_async(rtlsdr_dev_t*, rtlsdr_read_async_cb_t callback, void* ctx, uint32_t, uint32_t bytes) {
    std::vector<unsigned char> packet(bytes,128);
    while (!cancelled.load()) {
        { std::lock_guard<std::mutex> event(usbEvents);callback(packet.data(),packet.size(),ctx); }
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    return 0;
}
}

int main() {
    core::args.defineAll();
    config.conf={{"devices",json::object()},{"device",""}};
    long long maximum=0;
    {
        RTLSDRSourceModule source("RTL control test");
        source.freq=145000000;
        for (int run=0;run<8;++run) {
            source.start(&source);
            auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
            while (!source.iqBuffer.droppedPackets()) {
                assert(std::chrono::steady_clock::now()<deadline);
                std::this_thread::yield();
            }
            // Same synchronous driver operations used by the GUI. The DSP
            // stream remains intentionally unconsumed throughout this test.
            for (int step=0;step<32;++step) {
                auto start=std::chrono::steady_clock::now();
                assert(rtlsdr_set_tuner_gain(source.openDev,step*10)==0);
                assert(rtlsdr_set_agc_mode(source.openDev,step%2)==0);
                source.tune(145000000+step*1000,&source);
                auto elapsed=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count();
                maximum=std::max<long long>(maximum,elapsed);
                assert(elapsed<100000);
            }
            auto start=std::chrono::steady_clock::now();
            source.stop(&source);
            assert(std::chrono::steady_clock::now()-start<std::chrono::milliseconds(100));
            assert(!source.workerThread.joinable() && openDevices==0);
            assert(source.stream.read_for(std::chrono::milliseconds(0))==0);
        }
    }
    assert(openDevices==0);
    std::printf("OK: actual RTL source, 256 gain/AGC/tune cycles with stalled DSP; max=%lld us; 8 starts/stops <100 ms, no open device/thread or stale packet\n",maximum);
}
