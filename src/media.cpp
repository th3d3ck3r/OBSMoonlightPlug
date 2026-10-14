#include "media.hpp"
#include <util/platform.h>
#include <limits>
#include <sstream>
#include <cstring>

Media::~Media() { videoCleanup(); audioCleanup(); }
void Media::configure(const Settings &s) { settings_=s; videoEpoch_=0; audioEpoch_=0; lastVideo_=0; audioSamples_=0; }
bool Media::hasCodec(AVCodecID id) { return avcodec_find_decoder(id)!=nullptr; }

AVPixelFormat Media::chooseFormat(AVCodecContext *ctx,const AVPixelFormat *formats) {
    auto *self=static_cast<Media*>(ctx->opaque);
    if (self->hardware_) for(auto f=formats;*f!=AV_PIX_FMT_NONE;++f) if(*f==self->hwFormat_) return *f;
    if(self->settings_.decoder==1) return AV_PIX_FMT_NONE;
    for(auto f=formats;*f!=AV_PIX_FMT_NONE;++f) {
        auto *desc=av_pix_fmt_desc_get(*f);
        if(desc && !(desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) return *f;
    }
    return AV_PIX_FMT_NONE;
}

bool Media::openDecoder(const AVCodec *codec,bool hardware) {
    decoder_=avcodec_alloc_context3(codec);
    if(!decoder_) return false;
    decoder_->width=width_; decoder_->height=height_;
    decoder_->pkt_timebase=AVRational{1,1000000};
    decoder_->flags|=AV_CODEC_FLAG_LOW_DELAY;
    decoder_->thread_count=2;
    decoder_->thread_type=FF_THREAD_SLICE;
    decoder_->opaque=this;
    decoder_->get_format=chooseFormat;
    if(hardware) {
        AVHWDeviceType type=AV_HWDEVICE_TYPE_NONE;
#ifdef _WIN32
        type=AV_HWDEVICE_TYPE_D3D11VA;
#elif defined(__APPLE__)
        type=AV_HWDEVICE_TYPE_VIDEOTOOLBOX;
#else
        type=AV_HWDEVICE_TYPE_VAAPI;
#endif
        for(int i=0;;++i) {
            const auto *config=avcodec_get_hw_config(codec,i);
            if(!config) break;
            if(config->device_type==type && (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
                if(av_hwdevice_ctx_create(&hardware_,type,nullptr,nullptr,0)>=0) {
                    hwFormat_=config->pix_fmt;
                    decoder_->hw_device_ctx=av_buffer_ref(hardware_);
                }
                break;
            }
        }
        if(!hardware_ && settings_.decoder==1) return false;
    }
    return avcodec_open2(decoder_,codec,nullptr)>=0;
}

int Media::videoSetup(int format,int width,int height,int) {
    videoCleanup();
    std::lock_guard<std::mutex> lock(videoMutex_);
    format_=format; width_=width; height_=height; decoded_=failed_=0;
    if(width<1 || height<1 || width>7680 || height>4320) return -1;
    AVCodecID id=(format & VIDEO_FORMAT_MASK_AV1)?AV_CODEC_ID_AV1:(format & VIDEO_FORMAT_MASK_H265)?AV_CODEC_ID_HEVC:AV_CODEC_ID_H264;
    const AVCodec *codec=avcodec_find_decoder(id);
    if(!codec) return -1;
    if(!openDecoder(codec,settings_.decoder!=2)) {
        avcodec_free_context(&decoder_);av_buffer_unref(&hardware_);hwFormat_=AV_PIX_FMT_NONE;
        if(settings_.decoder==1 || !openDecoder(codec,false)) return -1;
    }
    frame_=av_frame_alloc();download_=av_frame_alloc();
    return frame_ && download_ ? 0 : -1;
}

int Media::videoFrame(PDECODE_UNIT unit) {
    std::lock_guard<std::mutex> lock(videoMutex_);
    if(!unit || !decoder_ || !frame_ || !download_ || unit->fullLength<=0 || unit->fullLength>32*1024*1024 || unit->presentationTimeUs>31536000000000ULL) return DR_NEED_IDR;
    AVPacket *packet=av_packet_alloc();
    if(!packet || av_new_packet(packet,unit->fullLength)<0) { av_packet_free(&packet);return DR_NEED_IDR; }
    int offset=0;
    for(auto entry=unit->bufferList;entry;entry=entry->next) {
        if(entry->length<=0 || entry->length>unit->fullLength-offset) { av_packet_free(&packet);++failed_;return DR_NEED_IDR; }
        std::memcpy(packet->data+offset,entry->data,static_cast<size_t>(entry->length));offset+=entry->length;
    }
    if(offset!=unit->fullLength) { av_packet_free(&packet);++failed_;return DR_NEED_IDR; }
    packet->pts=packet->dts=static_cast<int64_t>(unit->presentationTimeUs);
    int result=avcodec_send_packet(decoder_,packet);
    if(result<0 && hardware_ && settings_.decoder==0) {
        const AVCodec *codec=decoder_->codec;
        avcodec_free_context(&decoder_);av_buffer_unref(&hardware_);hwFormat_=AV_PIX_FMT_NONE;
        if(openDecoder(codec,false))result=avcodec_send_packet(decoder_,packet);
    }
    av_packet_free(&packet);
    if(result<0) { ++failed_;if(decoder_)avcodec_flush_buffers(decoder_);return DR_NEED_IDR; }
    while((result=avcodec_receive_frame(decoder_,frame_))>=0) {
        AVFrame *decoded=frame_;
        if(frame_->format==hwFormat_ && hardware_) {
            av_frame_unref(download_);
            if(av_hwframe_transfer_data(download_,frame_,0)<0) { ++failed_;av_frame_unref(frame_);return DR_NEED_IDR; }
            av_frame_copy_props(download_,frame_);decoded=download_;usedHardware_=true;
        } else usedHardware_=false;
        outputFrame(decoded);av_frame_unref(frame_);
    }
    if(result!=AVERROR(EAGAIN) && result!=AVERROR_EOF) {++failed_;return DR_NEED_IDR;}
    return DR_OK;
}

void Media::outputFrame(AVFrame *f) {
    if(f->width<1 || f->height<1 || f->width>7680 || f->height>4320) {++failed_;return;}
    const bool hdr=settings_.hdr && (format_ & VIDEO_FORMAT_MASK_10BIT);
    AVPixelFormat output=hdr?AV_PIX_FMT_P010LE:AV_PIX_FMT_NV12;
    int size=av_image_get_buffer_size(output,f->width,f->height,32);
    if(size<=0) {++failed_;return;}
    pixels_.resize(static_cast<size_t>(size));
    uint8_t *planes[4]={};int strides[4]={};
    av_image_fill_arrays(planes,strides,pixels_.data(),output,f->width,f->height,32);
    scale_=sws_getCachedContext(scale_,f->width,f->height,static_cast<AVPixelFormat>(f->format),f->width,f->height,output,SWS_FAST_BILINEAR,nullptr,nullptr,nullptr);
    if(!scale_) {++failed_;return;}
    int range=f->color_range==AVCOL_RANGE_UNSPECIFIED?(settings_.fullRange?1:0):(f->color_range==AVCOL_RANGE_JPEG?1:0);
    int colors=(f->colorspace==AVCOL_SPC_BT2020_NCL || hdr)?SWS_CS_BT2020:SWS_CS_ITU709;
    const int *coeff=sws_getCoefficients(colors);
    sws_setColorspaceDetails(scale_,coeff,range,coeff,range,0,1<<16,1<<16);
    if(sws_scale(scale_,f->data,f->linesize,0,f->height,planes,strides)<=0) {++failed_;return;}
    int64_t pts=f->pts==AV_NOPTS_VALUE?static_cast<int64_t>(decoded_)*1000000/settings_.fps:f->pts;
    int64_t now=static_cast<int64_t>(os_gettime_ns());
    if(!videoEpoch_) videoEpoch_=now-pts*1000;
    int64_t timestamp=videoEpoch_+pts*1000+settings_.bufferMs*1000000LL;
    if(timestamp<=lastVideo_) timestamp=lastVideo_+1;
    lastVideo_=timestamp;
    obs_source_frame frame{};
    frame.width=static_cast<uint32_t>(f->width);frame.height=static_cast<uint32_t>(f->height);
    frame.format=hdr?VIDEO_FORMAT_P010:VIDEO_FORMAT_NV12;
    frame.trc=hdr?VIDEO_TRC_PQ:VIDEO_TRC_SRGB;
    if(hdr) {
        frame.max_luminance=1000;
        auto *mastering=av_frame_get_side_data(f,AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
        if(mastering && mastering->size>=sizeof(AVMasteringDisplayMetadata)) {
            auto *m=reinterpret_cast<const AVMasteringDisplayMetadata*>(mastering->data);
            if(m->has_luminance)frame.max_luminance=static_cast<uint16_t>(std::clamp(av_q2d(m->max_luminance),1.0,65535.0));
        }
        auto *light=av_frame_get_side_data(f,AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);
        if(light && light->size>=sizeof(AVContentLightMetadata)) {
            auto *m=reinterpret_cast<const AVContentLightMetadata*>(light->data);
            if(m->MaxCLL)frame.max_luminance=static_cast<uint16_t>(std::min(m->MaxCLL,65535U));
        }
    }
    frame.timestamp=static_cast<uint64_t>(timestamp);frame.full_range=range!=0;
    video_colorspace cs=hdr?VIDEO_CS_2100_PQ:VIDEO_CS_709;
    video_format_get_parameters(cs,range?VIDEO_RANGE_FULL:VIDEO_RANGE_PARTIAL,frame.color_matrix,frame.color_range_min,frame.color_range_max);
    for(size_t i=0;i<4;++i){frame.data[i]=planes[i];frame.linesize[i]=static_cast<uint32_t>(strides[i]);}
    if(videoSink_)videoSink_(&frame);else obs_source_output_video(source_,&frame);++decoded_;
}

void Media::videoCleanup() {
    std::lock_guard<std::mutex> lock(videoMutex_);
    avcodec_free_context(&decoder_);av_buffer_unref(&hardware_);
    av_frame_free(&frame_);av_frame_free(&download_);
    sws_freeContext(scale_);scale_=nullptr;hwFormat_=AV_PIX_FMT_NONE;pixels_.clear();
}

int Media::audioSetup(const OPUS_MULTISTREAM_CONFIGURATION *c) {
    audioCleanup();std::lock_guard<std::mutex> lock(audioMutex_);
    if(c->channelCount!=2 && c->channelCount!=6 && c->channelCount!=8) return -1;
    sampleRate_=c->sampleRate;channels_=c->channelCount;
    int error=0;opus_=opus_multistream_decoder_create(c->sampleRate,c->channelCount,c->streams,c->coupledStreams,c->mapping,&error);
    pcm_.resize(static_cast<size_t>(5760*c->channelCount));audioEpoch_=0;audioSamples_=0;
    return opus_ && error==OPUS_OK?0:-1;
}

void Media::audioFrame(char *data,int length) {
    std::lock_guard<std::mutex> lock(audioMutex_);
    if(!opus_ || length<0 || length>65536) return;
    int samples=opus_multistream_decode(opus_,reinterpret_cast<const unsigned char*>(data),length,pcm_.data(),5760,0);
    if(samples<=0) return;
    int64_t now=static_cast<int64_t>(os_gettime_ns());
    if(!audioEpoch_) audioEpoch_=now;
    int64_t raw=audioEpoch_+static_cast<int64_t>(audioSamples_)*1000000000LL/sampleRate_;
    if(now-raw>150000000LL){audioEpoch_+=now-raw;raw=now;}
    obs_source_audio audio{};audio.data[0]=reinterpret_cast<const uint8_t*>(pcm_.data());
    audio.frames=static_cast<uint32_t>(samples);audio.samples_per_sec=static_cast<uint32_t>(sampleRate_);
    audio.speakers=channels_==8?SPEAKERS_7POINT1:channels_==6?SPEAKERS_5POINT1:SPEAKERS_STEREO;
    audio.format=AUDIO_FORMAT_16BIT;
    audio.timestamp=static_cast<uint64_t>(std::max<int64_t>(0,raw+(settings_.bufferMs+settings_.audioOffsetMs)*1000000LL));
    if(audioSink_)audioSink_(&audio);else obs_source_output_audio(source_,&audio);audioSamples_+=static_cast<uint64_t>(samples);
}
void Media::audioCleanup(){std::lock_guard<std::mutex> lock(audioMutex_);if(opus_)opus_multistream_decoder_destroy(opus_);opus_=nullptr;pcm_.clear();}
std::string Media::statistics() const {
    std::lock_guard<std::mutex> lock(videoMutex_);if(!width_)return "No video received yet.";std::ostringstream s;
    s << width_ << 'x' << height_ << " | " << ((format_ & VIDEO_FORMAT_MASK_AV1)?"AV1":(format_ & VIDEO_FORMAT_MASK_H265)?"HEVC":"H.264") << " | " << (!decoded_?"waiting for video":usedHardware_?"hardware decode":"software decode") << " | decoded " << decoded_ << " | decode errors " << failed_;
    return s.str();
}
