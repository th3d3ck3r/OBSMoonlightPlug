#include "media.hpp"
#include "nvhttp.h"
#include <QCoreApplication>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <cstring>

extern "C" bool parseSdpAttributeToUInt(const char*, const char*, uint32_t*);
extern "C" bool parseSdpAttributeToInt(const char*, const char*, int*);

static void check(bool ok,const char *message){if(!ok)throw std::runtime_error(message);}
template<typename F>static void rejects(F f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}check(failed,"invalid value accepted");}

int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);
    try {
        uint32_t unsignedValue=0;int signedValue=0;
        check(parseSdpAttributeToUInt("a=test:4294967295\r\n","a=test",&unsignedValue)&&unsignedValue==UINT32_MAX,"SDP uint32 boundary failed");
        check(!parseSdpAttributeToUInt("a=test:4294967296\r\n","a=test",&unsignedValue),"SDP uint32 overflow accepted");
        check(!parseSdpAttributeToUInt("a=test: -1\r\n","a=test",&unsignedValue),"SDP negative unsigned accepted");
        check(parseSdpAttributeToInt("a=test:-2147483648\r\n","a=test",&signedValue)&&signedValue==INT32_MIN,"SDP int32 boundary failed");
        check(!parseSdpAttributeToInt("a=test:2147483648\r\n","a=test",&signedValue),"SDP int32 overflow accepted");
        check(!parseSdpAttributeToInt("a=test:nope\r\n","a=test",&signedValue),"SDP nonnumeric input accepted");
        Settings s;s.host="192.168.1.20";s.validate();
        for(auto host:{"https://host","host/path","host name",""}){auto bad=s;bad.host=host;rejects([&]{bad.validate();});}
        auto bad=s;bad.width=1919;rejects([&]{bad.validate();});
        bad=s;bad.fps=0;rejects([&]{bad.validate();});
        bad=s;bad.bitrate=300001;rejects([&]{bad.validate();});
        bad=s;bad.hdr=true;bad.codec=1;rejects([&]{bad.validate();});
        bad=s;bad.channels=3;rejects([&]{bad.validate();});
        for(int channels:{2,6,8}){s.channels=channels;s.validate();}s.channels=2;
        auto *data=obs_data_create();
        obs_data_set_string(data,"host","192.168.1.20");obs_data_set_int(data,"port",47989);
        obs_data_set_int(data,"resolution",4);obs_data_set_int(data,"width",3440);obs_data_set_int(data,"height",1440);
        obs_data_set_int(data,"fps",120);obs_data_set_int(data,"bitrate",45000);obs_data_set_int(data,"channels",2);
        auto custom=Settings::read(data);custom.validate();check(custom.width==3440&&custom.height==1440&&custom.fps==120,"custom settings not honored");obs_data_release(data);

        NvHTTP::verifyResponseStatus("<root status_code=\"200\"><state>OK</state></root>");
        rejects([]{NvHTTP::verifyResponseStatus("<root status_code=\"403\" status_message=\"Denied\"/> ");});
        rejects([]{NvHTTP::verifyResponseStatus("not XML");});
        check(NvHTTP::getXmlStringFromHex("<root><data>000102ff</data></root>","data")==QByteArray::fromHex("000102ff"),"binary XML value corrupt");
        check(NvHTTP::getCurrentGame("<root><state>SERVER_FREE</state><currentgame>42</currentgame></root>")==0,"free server app incorrectly treated as running");
        check(NvHTTP::getCurrentGame("<root><state>XYZ_SERVER_BUSY</state><currentgame>42</currentgame></root>")==42,"busy app missing");

        int frames=0,audioFrames=0;uint64_t lastTimestamp=0;
        Media media(nullptr,[&](const obs_source_frame *frame){
            check(frame->width==64&&frame->height==64,"decoded dimensions wrong");
            check(frame->format==VIDEO_FORMAT_NV12,"SDR not NV12");
            check(frame->trc==VIDEO_TRC_SRGB,"SDR transfer tag wrong");
            check(frame->data[0]&&frame->data[1]&&frame->linesize[0]>=64,"decoded planes missing");
            check(frame->timestamp>lastTimestamp,"video timestamps not monotonic");lastTimestamp=frame->timestamp;++frames;
        },[&](const obs_source_audio *audio){
            check(audio->speakers==SPEAKERS_STEREO&&audio->samples_per_sec==48000,"audio layout/rate wrong");
            check(audio->format==AUDIO_FORMAT_16BIT&&audio->data[0]&&audio->timestamp>0,"audio data/timestamp missing");audioFrames+=static_cast<int>(audio->frames);
        });
        s.decoder=2;media.configure(s);check(media.videoSetup(VIDEO_FORMAT_H264,64,64,30)==0,"H264 decoder failed");
        std::ifstream input(std::string(TEST_FIXTURES)+"/frame.h264",std::ios::binary);
        std::vector<char> packet{std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
        check(!packet.empty(),"fixture missing");
        LENTRY entry{};entry.data=packet.data();entry.length=static_cast<int>(packet.size());
        DECODE_UNIT unit{};unit.bufferList=&entry;unit.fullLength=entry.length;unit.frameType=FRAME_TYPE_IDR;
        check(media.videoFrame(&unit)==DR_OK,"H264 frame decode failed");check(frames==1,"H264 frame not emitted");
        unit.presentationTimeUs=33333;check(media.videoFrame(&unit)==DR_OK&&frames==2,"second frame or timeline failed");
        unit.fullLength=entry.length+1;check(media.videoFrame(&unit)==DR_NEED_IDR,"malformed buffer accepted");
        unit.fullLength=40*1024*1024;check(media.videoFrame(&unit)==DR_NEED_IDR,"oversized frame accepted");
        media.videoCleanup();media.videoCleanup();

        int hdrFrames=0;
        Media hdr(nullptr,[&](const obs_source_frame *frame){
            check(frame->format==VIDEO_FORMAT_P010&&frame->trc==VIDEO_TRC_PQ,"HDR format/transfer tag wrong");
            check(frame->max_luminance==1000,"HDR luminance metadata missing");
            check(frame->linesize[0]>=128&&frame->data[1],"10-bit HDR planes missing");++hdrFrames;
        });
        Settings hs=s;hs.hdr=true;hdr.configure(hs);
        check(hdr.videoSetup(VIDEO_FORMAT_H265_MAIN10,64,64,30)==0,"HEVC Main10 decoder failed");
        std::ifstream hdrInput(std::string(TEST_FIXTURES)+"/frame-pq.hevc",std::ios::binary);
        std::vector<char> hdrPacket{std::istreambuf_iterator<char>(hdrInput),std::istreambuf_iterator<char>()};
        check(!hdrPacket.empty(),"HDR fixture missing");
        entry.data=hdrPacket.data();entry.length=static_cast<int>(hdrPacket.size());unit.fullLength=entry.length;unit.presentationTimeUs=0;
        check(hdr.videoFrame(&unit)==DR_OK&&hdrFrames==1,"HEVC HDR frame not emitted");hdr.videoCleanup();

        OPUS_MULTISTREAM_CONFIGURATION config{};config.sampleRate=48000;config.channelCount=2;config.streams=1;config.coupledStreams=1;config.samplesPerFrame=480;config.mapping[0]=0;config.mapping[1]=1;
        check(media.audioSetup(&config)==0,"Opus decoder failed");
        int error=0;auto *encoder=opus_multistream_encoder_create(48000,2,1,1,config.mapping,OPUS_APPLICATION_AUDIO,&error);
        check(encoder&&error==OPUS_OK,"Opus test encoder failed");
        std::vector<int16_t> pcm(960,1000);std::vector<unsigned char> encoded(4096);
        int bytes=opus_multistream_encode(encoder,pcm.data(),480,encoded.data(),static_cast<opus_int32>(encoded.size()));
        check(bytes>0,"Opus encoding failed");media.audioFrame(reinterpret_cast<char*>(encoded.data()),bytes);
        check(audioFrames==480,"Opus PCM sample count wrong");
        opus_multistream_encoder_destroy(encoder);media.audioCleanup();media.audioCleanup();
        std::cout<<"Settings, XML, H264, HEVC HDR/PQ metadata, malformed packets, timestamps and Opus decode passed\n";
        return 0;
    } catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
}
