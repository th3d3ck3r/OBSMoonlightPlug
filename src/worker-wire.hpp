#pragma once
// Private, versioned local IPC between OBS and its per-stream macOS Moonlight worker.
// Both programs are compiled together for the same local CPU architecture.
#include <Limelight.h>
#include <cstdint>
#include <cstring>
#include <string>

namespace WorkerWire {
constexpr uint32_t Magic = 0x31525347; // GSR1
constexpr uint32_t MaxMessage = 32u * 1024u * 1024u + 64u;
constexpr uint32_t MaxString = 8192u;

enum class Type : uint32_t {
    VideoSetup=1, VideoFrame=2, VideoCleanup=3,
    AudioSetup=4, AudioFrame=5, AudioCleanup=6,
    Status=7, Ended=8, Error=9
};

struct Header {
    uint32_t magic = Magic;
    uint32_t type = 0;
    uint32_t length = 0;
};
struct Launch {
    uint32_t magic = Magic;
    uint32_t version = 1;
    STREAM_CONFIGURATION stream{};
    int32_t hostCodecModes = 0;
    uint32_t addressSize = 0, appVersionSize = 0, gfeVersionSize = 0, rtspUrlSize = 0;
};
struct VideoSetup { int32_t format, width, height, fps; };
struct VideoFrame { uint64_t ptsUs; uint32_t frameType, length; };
inline bool valid(const Header &h) {
    return h.magic == Magic && h.type >= uint32_t(Type::VideoSetup) &&
           h.type <= uint32_t(Type::Error) && h.length <= MaxMessage;
}
inline bool valid(const Launch &l) {
    return l.magic == Magic && l.version == 1 &&
           l.addressSize > 0 && l.addressSize <= MaxString &&
           l.appVersionSize > 0 && l.appVersionSize <= MaxString &&
           l.gfeVersionSize <= MaxString && l.rtspUrlSize <= MaxString;
}
}
