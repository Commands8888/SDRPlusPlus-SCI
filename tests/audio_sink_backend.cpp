// Compile twice: exercise each shipped native sink through captured callbacks,
// including open/start failure and a computer with no audio output device.
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

#ifdef SDRPP_TEST_PORTAUDIO
#include <portaudio.h>
static unsigned fakeChannels=2;
static bool fakeNoDevice=false,fakeFailOpen=false,fakeFailStart=false;
static int fakeOpenCount=0;
static PaStreamCallback* fakeCallback=nullptr;
static void* fakeContext=nullptr;
extern "C" {
PaError Pa_Initialize() { return paNoError; }
PaError Pa_Terminate() { return paNoError; }
PaDeviceIndex Pa_GetDeviceCount() { return fakeNoDevice?0:1; }
const PaDeviceInfo* Pa_GetDeviceInfo(PaDeviceIndex) {
    static PaDeviceInfo info{};
    info.name="Test audio";info.maxOutputChannels=fakeChannels;info.defaultSampleRate=48000;info.defaultLowOutputLatency=0.01;
    return &info;
}
const PaHostApiInfo* Pa_GetHostApiInfo(PaHostApiIndex) { static PaHostApiInfo info{};info.name="Test API";return &info; }
PaDeviceIndex Pa_GetDefaultOutputDevice() { return 0; }
PaError Pa_IsFormatSupported(const PaStreamParameters*,const PaStreamParameters*,double rate) { return rate==48000?paFormatIsSupported:paInvalidSampleRate; }
PaError Pa_OpenStream(PaStream** stream,const PaStreamParameters*,const PaStreamParameters*,double,unsigned long,PaStreamFlags,PaStreamCallback* cb,void* ctx) {
    if(fakeFailOpen) return paUnanticipatedHostError;
    fakeCallback=cb;fakeContext=ctx;*stream=reinterpret_cast<PaStream*>(1);++fakeOpenCount;return paNoError;
}
PaError Pa_StartStream(PaStream*) { return fakeFailStart?paUnanticipatedHostError:paNoError; }
PaError Pa_AbortStream(PaStream*) { return paNoError; }
PaError Pa_CloseStream(PaStream*) { --fakeOpenCount;return paNoError; }
const char* Pa_GetErrorText(PaError) { return "test native failure"; }
}
#include "../sink_modules/new_portaudio_sink/src/main.cpp"
static void emit(void* out,unsigned frames) { PaStreamCallbackTimeInfo info{};fakeCallback(nullptr,out,frames,&info,0,fakeContext); }
#else
#include "../sink_modules/audio_sink/src/main.cpp"
static auto& fakeChannels=RtAudio::channels;
static auto& fakeNoDevice=RtAudio::noDevice;
static auto& fakeFailOpen=RtAudio::failOpen;
static auto& fakeFailStart=RtAudio::failStart;
static auto& fakeOpenCount=RtAudio::openCount;
static void emit(void* out,unsigned frames) { RtAudio::emit(out,frames); }
#endif

int main() {
    for(int scenario=0;scenario<6;++scenario) {
        fakeChannels=scenario==1?1:2;
        fakeNoDevice=scenario==2;fakeFailOpen=scenario==3;fakeFailStart=scenario==4;
#ifndef SDRPP_TEST_PORTAUDIO
        RtAudio::failStop=scenario==5;RtAudio::failClose=scenario==5;
#endif
        config.conf=json::object();
        dsp::stream<dsp::stereo_t> input;
        EventHandler<float> rateChanged([](float,void*){},nullptr);
        SinkManager::Stream stream(&input,&rateChanged,48000);
        stream.sinkOut=&input;
        {
            AudioSink sink(&stream,"Test radio");
            sink.start();
#if !defined(SDRPP_TEST_PORTAUDIO) && RTAUDIO_VERSION_MAJOR >= 6
            if(scenario==0) RtAudio::current->errorCallback(RTAUDIO_DRIVER_ERROR,"test asynchronous native error");
#endif
            if(scenario<2) {
                std::vector<dsp::stereo_t> stereo(2048,{NAN,NAN});
                std::vector<float> mono(2048,NAN);
                auto render=[&](unsigned count) { emit(fakeChannels==1?(void*)mono.data():(void*)stereo.data(),count); };
                render(800); // Empty producer must not block the native callback.
                for(int i=0;i<800;++i) assert(fakeChannels==1?mono[i]==0:(stereo[i].l==0&&stereo[i].r==0));
                const int total=10001;
                for(int i=0;i<total;++i) input.writeBuf[i]={(float)(i+1),(float)(i+1)*0.5f};
                assert(input.swap(total));
                int received=0,iteration=0;
                auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
                while(received<total) {
                    assert(std::chrono::steady_clock::now()<deadline);
                    const unsigned sizes[]={127,511,1024,800,259};
                    unsigned count=sizes[iteration++%5];render(count);
                    bool silent=false;
                    for(unsigned i=0;i<count;++i) {
                        float value=fakeChannels==1?mono[i]:stereo[i].l;
                        if(value==0) { silent=true;if(fakeChannels==2) assert(stereo[i].r==0);continue; }
                        assert(!silent && received<total);
                        float expected=received+1;
                        assert(value==expected*(fakeChannels==1?0.75f:1.0f));
                        if(fakeChannels==2) assert(stereo[i].r==expected*0.5f);
                        ++received;
                    }
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                }
                render(800);
            } else {
                // Failed/native-missing output must still drain the shared
                // DSP splitter instead of freezing reception and the FFT.
                for(int packet=0;packet<4;++packet) {
                    input.writeBuf[0]={0,0};assert(input.swap(1));
                }
            }
            sink.stop();sink.stop();
        }
        assert(fakeOpenCount==0);
    }
    std::puts("OK: native audio sink handles empty/variable callbacks, mono/stereo, missing device and open/start failures without blocking or leaking a stream");
}
