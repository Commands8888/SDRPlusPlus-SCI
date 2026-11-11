// Test-only native backend. Captures the real sink's callback without a device.
#pragma once
#include <string>
#include <vector>
#include <stdexcept>
#include <cassert>
#ifndef SDRPP_TEST_RTAUDIO_VERSION
#define SDRPP_TEST_RTAUDIO_VERSION 5
#endif
#define RTAUDIO_VERSION_MAJOR SDRPP_TEST_RTAUDIO_VERSION
#define RTAUDIO_FLOAT32 16
#define RTAUDIO_MINIMIZE_LATENCY 2
using RtAudioStreamStatus=unsigned;
enum RtAudioErrorType { RTAUDIO_NO_ERROR,RTAUDIO_WARNING,RTAUDIO_NO_DEVICES_FOUND,RTAUDIO_DEVICE_DISCONNECT,RTAUDIO_DRIVER_ERROR };
class RtAudio {
public:
    enum Api { UNSPECIFIED, LINUX_ALSA, LINUX_PULSE };
    struct DeviceInfo {
        bool probed=true,isDefaultOutput=true;
        unsigned outputChannels=2,preferredSampleRate=48000;
        std::string name="Test audio";
        std::vector<unsigned> sampleRates{44100,48000,96000};
    };
    struct StreamParameters { unsigned deviceId=0,nChannels=2; };
    struct StreamOptions { unsigned flags=0,numberOfBuffers=0; std::string streamName; };
    using Callback=int(*)(void*,void*,unsigned,double,RtAudioStreamStatus,void*);
    static inline RtAudio* current=nullptr;
    static inline unsigned channels=2;
    static inline bool noDevice=false,failOpen=false,failStart=false;
    static inline bool failStop=false,failClose=false;
    static inline int openCount=0;
    bool opened=false,started=false;
    Callback callback=nullptr;
    void* context=nullptr;
    void(*errorCallback)(RtAudioErrorType,const std::string&)=nullptr;
    RtAudio() { current=this; }
    unsigned getDeviceCount() { return noDevice ? 0 : 1; }
    std::vector<unsigned> getDeviceIds() { return noDevice ? std::vector<unsigned>{} : std::vector<unsigned>{0}; }
    DeviceInfo getDeviceInfo(unsigned) { DeviceInfo info; info.outputChannels=channels; return info; }
    Api getCurrentApi() { return LINUX_PULSE; }
    void setErrorCallback(void(*callback)(RtAudioErrorType,const std::string&)) { errorCallback=callback; }
    void openStream(StreamParameters*,void*,unsigned,unsigned,unsigned*,Callback cb,void* ctx,StreamOptions* options) {
        assert(options && options->numberOfBuffers==2);
        if(failOpen) throw std::runtime_error("test open failure");
        callback=cb; context=ctx; opened=true; ++openCount;
    }
    void startStream() { if(failStart) throw std::runtime_error("test start failure"); started=true; }
    void stopStream() { started=false;if(failStop) throw std::runtime_error("test stop failure"); }
    void closeStream() { opened=false;--openCount;if(failClose) throw std::runtime_error("test close failure"); }
    bool isStreamOpen() { return opened; }
    bool isStreamRunning() { return started; }
    static void emit(void* out,unsigned frames) { current->callback(out,nullptr,frames,0,0,current->context); }
};
