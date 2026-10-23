// One Moonlight streaming engine per process. This binary intentionally does not
// link libobs, Qt, or the FFmpeg decoder; OBS still owns all A/V decoding/output.
#include "worker-wire.hpp"
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <chrono>
#include <unistd.h>

namespace {
using namespace WorkerWire;
std::mutex outputMutex;
std::atomic<bool> stopping{false}, ended{false};
std::atomic<int> endError{0};
volatile std::sig_atomic_t signalled = 0;

void stopSignal(int) { signalled = 1; }

bool readExact(void *output,size_t size) {
    auto *p=static_cast<unsigned char*>(output);
    while(size) {
        ssize_t n=::read(STDIN_FILENO,p,size);
        if(n<0 && errno==EINTR)continue;
        if(n<=0)return false;
        p+=n;size-=static_cast<size_t>(n);
    }
    return true;
}
bool writeExact(const void *input,size_t size) {
    const auto *p=static_cast<const unsigned char*>(input);
    while(size) {
        ssize_t n=::write(STDOUT_FILENO,p,size);
        if(n<0 && errno==EINTR)continue;
        if(n<=0){stopping=true;return false;}
        p+=n;size-=static_cast<size_t>(n);
    }
    return true;
}
void send(Type type,const void *first=nullptr,size_t firstSize=0,
          const void *second=nullptr,size_t secondSize=0) {
    if(stopping || firstSize+secondSize>MaxMessage)return;
    std::lock_guard<std::mutex> lock(outputMutex);
    Header h{Magic,static_cast<uint32_t>(type),static_cast<uint32_t>(firstSize+secondSize)};
    if(!writeExact(&h,sizeof h))return;
    if(firstSize&&!writeExact(first,firstSize))return;
    if(secondSize)writeExact(second,secondSize);
}
void status(const std::string &message,Type type=Type::Status) {
    send(type,message.data(),message.size());
}
int videoSetup(int format,int width,int height,int fps,void *,int) {
    VideoSetup p{format,width,height,fps};send(Type::VideoSetup,&p,sizeof p);return stopping?-1:0;
}
int videoFrame(PDECODE_UNIT unit) {
    if(!unit || !unit->bufferList || unit->fullLength<=0 ||
       static_cast<uint32_t>(unit->fullLength)>MaxMessage-sizeof(VideoFrame))return DR_NEED_IDR;
    size_t count=0;
    for(auto *item=unit->bufferList;item;item=item->next) {
        if(!item->data || item->length<=0 || size_t(item->length)>size_t(unit->fullLength)-count)
            return DR_NEED_IDR;
        count+=static_cast<size_t>(item->length);
    }
    if(count!=size_t(unit->fullLength) || stopping)return DR_NEED_IDR;
    VideoFrame p{unit->presentationTimeUs,static_cast<uint32_t>(unit->frameType),
                 static_cast<uint32_t>(unit->fullLength)};
    std::lock_guard<std::mutex> lock(outputMutex);
    Header h{Magic,static_cast<uint32_t>(Type::VideoFrame),
             static_cast<uint32_t>(sizeof p+count)};
    if(!writeExact(&h,sizeof h) || !writeExact(&p,sizeof p))return DR_NEED_IDR;
    for(auto *item=unit->bufferList;item;item=item->next)
        if(!writeExact(item->data,static_cast<size_t>(item->length)))return DR_NEED_IDR;
    return DR_OK;
}
void videoCleanup(){send(Type::VideoCleanup);}
int audioSetup(int,OPUS_MULTISTREAM_CONFIGURATION *c,void *,int) {
    if(!c)return -1;
    send(Type::AudioSetup,c,sizeof *c);return stopping?-1:0;
}
void audioFrame(char *data,int length) {
    if(data && length>0 && length<=65536)send(Type::AudioFrame,data,size_t(length));
}
void audioCleanup(){send(Type::AudioCleanup);}
void started(){status("Connected — receiving video and audio. Input disabled.");}
void terminated(int code){endError=code;ended=true;}
void starting(int stage){status(std::string("Connecting: ")+LiGetStageName(stage));}
void failed(int stage,int error){
    status(std::string("Connection failed at ")+LiGetStageName(stage)+" ("+std::to_string(error)+")");
}
void ignoreLog(const char*,...){}
}

int main(int argc,char **argv) {
    if(argc==2 && std::strcmp(argv[1],"--self-test")==0) {
        Header h{Magic,uint32_t(Type::VideoFrame),16};
        return valid(h)?0:1;
    }
    std::signal(SIGPIPE,SIG_IGN);
    std::signal(SIGTERM,stopSignal);
    Launch launch{};
    if(!readExact(&launch,sizeof launch) || !valid(launch))return 2;
    std::string address(launch.addressSize,'\0'),appVersion(launch.appVersionSize,'\0');
    std::string gfeVersion(launch.gfeVersionSize,'\0'),rtsp(launch.rtspUrlSize,'\0');
    if(!readExact(address.data(),address.size()) ||
       !readExact(appVersion.data(),appVersion.size()) ||
       !readExact(gfeVersion.data(),gfeVersion.size()) ||
       !readExact(rtsp.data(),rtsp.size()))return 3;

    // Keep stdin open: a closed parent pipe, or X, interrupts just this session.
    std::thread([] {
        char command=0;
        while(::read(STDIN_FILENO,&command,1)>0 && command!='X'){}
        stopping=true;
        LiInterruptConnection();
    }).detach();

    SERVER_INFORMATION server;LiInitializeServerInformation(&server);
    server.address=address.c_str();
    server.serverInfoAppVersion=appVersion.c_str();
    server.serverInfoGfeVersion=gfeVersion.empty()?nullptr:gfeVersion.c_str();
    server.rtspSessionUrl=rtsp.empty()?nullptr:rtsp.c_str();
    server.serverCodecModeSupport=launch.hostCodecModes;
    DECODER_RENDERER_CALLBACKS video;LiInitializeVideoCallbacks(&video);
    video.setup=videoSetup;video.submitDecodeUnit=videoFrame;video.cleanup=videoCleanup;
    AUDIO_RENDERER_CALLBACKS audio;LiInitializeAudioCallbacks(&audio);
    audio.init=audioSetup;audio.decodeAndPlaySample=audioFrame;audio.cleanup=audioCleanup;
    audio.capabilities=CAPABILITY_SUPPORTS_ARBITRARY_AUDIO_DURATION;
    CONNECTION_LISTENER_CALLBACKS listener;LiInitializeConnectionCallbacks(&listener);
    listener.stageStarting=starting;listener.stageFailed=failed;
    listener.connectionStarted=started;listener.connectionTerminated=terminated;
    listener.logMessage=ignoreLog;
    int error=LiStartConnection(&server,&launch.stream,&listener,&video,&audio,
                                 nullptr,0,nullptr,0);
    if(error) {
        status("Streaming connection failed ("+std::to_string(error)+")",Type::Error);
        return 4;
    }
    while(!stopping && !ended && !signalled)
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    LiStopConnection();
    if(ended)status("Stream ended ("+std::to_string(endError.load())+").",Type::Ended);
    return 0;
}
