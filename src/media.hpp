#pragma once
#include "settings.hpp"
#include <Limelight.h>
#include <atomic>
#include <mutex>
#include <vector>
#include <functional>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libavutil/mastering_display_metadata.h>
#include <libswscale/swscale.h>
#include <opus_multistream.h>
}

class Media {
public:
    using VideoSink=std::function<void(const obs_source_frame*)>;
    using AudioSink=std::function<void(const obs_source_audio*)>;
    explicit Media(obs_source_t *source,VideoSink video={},AudioSink audio={}) : source_(source),videoSink_(std::move(video)),audioSink_(std::move(audio)) {}
    ~Media();
    void configure(const Settings &s);
    int videoSetup(int format, int width, int height, int fps);
    int videoFrame(PDECODE_UNIT unit);
    void videoCleanup();
    int audioSetup(const OPUS_MULTISTREAM_CONFIGURATION *config);
    void audioFrame(char *data, int length);
    void audioCleanup();
    std::string statistics() const;
    static bool hasCodec(enum AVCodecID codec);
private:
    static enum AVPixelFormat chooseFormat(AVCodecContext *ctx, const enum AVPixelFormat *formats);
    bool openDecoder(const AVCodec *codec, bool hardware);
    void outputFrame(AVFrame *frame);
    obs_source_t *source_;
    VideoSink videoSink_;
    AudioSink audioSink_;
    Settings settings_;
    mutable std::mutex videoMutex_;
    std::mutex audioMutex_;
    AVCodecContext *decoder_ = nullptr;
    AVBufferRef *hardware_ = nullptr;
    AVFrame *frame_ = nullptr, *download_ = nullptr;
    SwsContext *scale_ = nullptr;
    enum AVPixelFormat hwFormat_ = AV_PIX_FMT_NONE;
    std::vector<uint8_t> pixels_;
    int format_ = 0, width_ = 0, height_ = 0;
    bool usedHardware_ = false;
    int64_t videoEpoch_ = 0, lastVideo_ = 0;
    uint64_t decoded_ = 0, failed_ = 0;
    OpusMSDecoder *opus_ = nullptr;
    std::vector<int16_t> pcm_;
    int sampleRate_ = 48000, channels_ = 2;
    int64_t audioEpoch_ = 0;
    uint64_t audioSamples_ = 0;
};
