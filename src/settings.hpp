#pragma once
#include <obs-module.h>
#include <algorithm>
#include <stdexcept>
#include <string>

struct Settings {
    std::string host;
    int port = 47989;
    int app = 0;
    int width = 1920, height = 1080, fps = 60, bitrate = 20000;
    int codec = 0, decoder = 0, channels = 2;
    int bufferMs = 50, audioOffsetMs = 0, network = 0;
    bool hdr = false, fullRange = true, hostAudio = true;
    bool reconnect = true, enabled = false, closeHidden = false;

    static Settings read(obs_data_t *data) {
        Settings s;
        s.host = obs_data_get_string(data, "host");
        s.port = static_cast<int>(obs_data_get_int(data, "port"));
        s.app = static_cast<int>(obs_data_get_int(data, "app"));
        int preset = static_cast<int>(obs_data_get_int(data,"resolution"));
        s.width = preset == 1 ? 1280 : preset == 2 ? 2560 : preset == 3 ? 3840 : preset == 4 ? static_cast<int>(obs_data_get_int(data,"width")) : 1920;
        s.height = preset == 1 ? 720 : preset == 2 ? 1440 : preset == 3 ? 2160 : preset == 4 ? static_cast<int>(obs_data_get_int(data,"height")) : 1080;
        s.fps = static_cast<int>(obs_data_get_int(data,"fps"));
        s.bitrate = static_cast<int>(obs_data_get_int(data,"bitrate"));
        s.codec = static_cast<int>(obs_data_get_int(data,"codec"));
        s.decoder = static_cast<int>(obs_data_get_int(data,"decoder"));
        s.channels = static_cast<int>(obs_data_get_int(data,"channels"));
        s.bufferMs = static_cast<int>(obs_data_get_int(data,"buffer"));
        s.audioOffsetMs = static_cast<int>(obs_data_get_int(data,"audio_offset"));
        s.network = static_cast<int>(obs_data_get_int(data,"network"));
        s.hdr = obs_data_get_bool(data,"hdr");
        s.fullRange = obs_data_get_bool(data,"full_range");
        s.hostAudio = obs_data_get_bool(data,"host_audio");
        s.reconnect = obs_data_get_bool(data,"reconnect");
        s.enabled = obs_data_get_bool(data,"enabled");
        s.closeHidden = obs_data_get_bool(data,"close_hidden");
        return s;
    }
    void validate() const {
        if (host.empty() || host.find_first_of("/\\?# \t\r\n") != std::string::npos)
            throw std::runtime_error("Enter a host name or IP address, without a URL or port suffix");
        if (port < 1 || port > 65535 || app < 0) throw std::runtime_error("Invalid host port or app ID");
        if (width < 320 || height < 240 || width > 7680 || height > 4320 || width % 2 || height % 2)
            throw std::runtime_error("Stream size must use even dimensions, from 320x240 to 7680x4320");
        if (fps < 1 || fps > 240 || bitrate < 500 || bitrate > 300000)
            throw std::runtime_error("FPS must be 1-240 and bitrate 500-300000 kbps");
        if (codec < 0 || codec > 3 || decoder < 0 || decoder > 2 || network < 0 || network > 2)
            throw std::runtime_error("Invalid stream option");
        if (hdr && codec == 1) throw std::runtime_error("HDR needs HEVC or AV1; H.264 is SDR only");
        if (channels != 2 && channels != 6 && channels != 8) throw std::runtime_error("Choose stereo, 5.1 or 7.1 audio");
        if (bufferMs < 0 || bufferMs > 250 || audioOffsetMs < -1000 || audioOffsetMs > 1000)
            throw std::runtime_error("Invalid buffering or audio sync offset");
    }
    bool operator==(const Settings &b) const {
        return host==b.host && port==b.port && app==b.app && width==b.width && height==b.height && fps==b.fps && bitrate==b.bitrate && codec==b.codec && decoder==b.decoder && channels==b.channels && bufferMs==b.bufferMs && audioOffsetMs==b.audioOffsetMs && network==b.network && hdr==b.hdr && fullRange==b.fullRange && hostAudio==b.hostAudio && reconnect==b.reconnect && enabled==b.enabled && closeHidden==b.closeHidden;
    }
};
