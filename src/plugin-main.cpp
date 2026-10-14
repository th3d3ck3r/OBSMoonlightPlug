#include "receiver.hpp"
#include <obs-module.h>
#include <memory>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-gamestream-receiver", "en-US")
MODULE_EXPORT const char *obs_module_description(void) { return "Receive-only Apollo/Sunshine source using Moonlight 6.2.0 transport"; }

namespace {
const char *name(void *) { return "GameStream Receiver (Moonlight 6.2)"; }
void defaults(obs_data_t *d) {
    obs_data_set_default_int(d,"port",47989);obs_data_set_default_int(d,"app",0);
    obs_data_set_default_int(d,"resolution",0);obs_data_set_default_int(d,"width",1920);obs_data_set_default_int(d,"height",1080);
    obs_data_set_default_int(d,"fps",60);obs_data_set_default_int(d,"bitrate",20000);
    obs_data_set_default_int(d,"codec",0);obs_data_set_default_int(d,"decoder",0);obs_data_set_default_int(d,"channels",2);
    obs_data_set_default_int(d,"buffer",50);obs_data_set_default_int(d,"audio_offset",0);obs_data_set_default_int(d,"network",0);
    obs_data_set_default_bool(d,"full_range",true);obs_data_set_default_bool(d,"host_audio",true);
    obs_data_set_default_bool(d,"reconnect",true);obs_data_set_default_bool(d,"enabled",false);obs_data_set_default_bool(d,"close_hidden",false);
    obs_data_set_default_string(d,"pin","1234");
}
void *create(obs_data_t *data,obs_source_t *source) {
    try {auto *r=new Receiver(source);r->update(Settings::read(data));return r;}
    catch(const std::exception &e){blog(LOG_ERROR,"[GameStream Receiver] %s",e.what());return nullptr;}
}
void destroy(void *data){delete static_cast<Receiver*>(data);}
void update(void *data,obs_data_t *settings){static_cast<Receiver*>(data)->update(Settings::read(settings));}
void activate(void *data){static_cast<Receiver*>(data)->active(true);}
void deactivate(void *data){static_cast<Receiver*>(data)->active(false);}

bool visibility(obs_properties_t *properties,obs_property_t *,obs_data_t *settings) {
    bool custom=obs_data_get_int(settings,"resolution")==4;
    obs_property_set_visible(obs_properties_get(properties,"width"),custom);
    obs_property_set_visible(obs_properties_get(properties,"height"),custom);
    return true;
}
bool pairButton(obs_properties_t *,obs_property_t *,void *data) {
    auto *r=static_cast<Receiver*>(data);
    // Source settings are obtained through the receiver's public source accessor.
    auto *settings=obs_source_get_settings(r->source());
    auto config=Settings::read(settings);config.enabled=false;
    auto pin=std::string(obs_data_get_string(settings,"pin"));
    obs_data_set_bool(settings,"enabled",false);obs_source_update(r->source(),settings);
    r->request(Receiver::Job::Pair,config,pin);obs_data_release(settings);return true;
}
bool appsButton(obs_properties_t *,obs_property_t *,void *data) {
    auto *r=static_cast<Receiver*>(data);auto *settings=obs_source_get_settings(r->source());
    auto config=Settings::read(settings);config.enabled=false;
    obs_data_set_bool(settings,"enabled",false);obs_source_update(r->source(),settings);
    r->request(Receiver::Job::Apps,config);obs_data_release(settings);return true;
}
bool connectButton(obs_properties_t *,obs_property_t *,void *data) {
    auto *r=static_cast<Receiver*>(data);auto *settings=obs_source_get_settings(r->source());
    obs_data_set_bool(settings,"enabled",true);obs_source_update(r->source(),settings);
    r->request(Receiver::Job::Update,Settings::read(settings));obs_data_release(settings);return true;
}
bool disconnectButton(obs_properties_t *,obs_property_t *,void *data) {
    auto *r=static_cast<Receiver*>(data);auto *settings=obs_source_get_settings(r->source());
    obs_data_set_bool(settings,"enabled",false);obs_source_update(r->source(),settings);
    r->request(Receiver::Job::Stop,Settings::read(settings));obs_data_release(settings);return true;
}
bool refreshButton(obs_properties_t *,obs_property_t *,void *){return true;}
bool applyPreset(obs_properties_t *,obs_property_t *,void *data) {
    auto *r=static_cast<Receiver*>(data);auto *s=obs_source_get_settings(r->source());
    int preset=static_cast<int>(obs_data_get_int(s,"preset"));
    if(preset>0) {
        obs_data_set_int(s,"resolution",preset==3?3:0);obs_data_set_int(s,"fps",60);
        obs_data_set_int(s,"bitrate",preset==1?15000:preset==3?50000:20000);
        obs_data_set_int(s,"buffer",preset==1?0:preset==3?100:50);
        obs_data_set_int(s,"codec",0);obs_data_set_bool(s,"hdr",false);
        obs_source_update(r->source(),s);
    }
    obs_data_release(s);return true;
}

obs_properties_t *properties(void *data) {
    auto *r=static_cast<Receiver*>(data);auto *p=obs_properties_create();
    obs_properties_add_text(p,"host","Host address",OBS_TEXT_DEFAULT);
    obs_properties_add_int(p,"port","GameStream HTTP port",1,65535,1);
    obs_properties_add_text(p,"pin","Four-digit pairing PIN (enter on host)",OBS_TEXT_PASSWORD);
    obs_properties_add_button2(p,"pair","Pair with host",pairButton,r);
    obs_properties_add_button2(p,"apps","Refresh host apps (disconnects stream)",appsButton,r);
    auto *app=obs_properties_add_list(p,"app","Host app / desktop",OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(app,"Desktop (automatically locate)",0);
    if(r)for(const auto &entry:r->apps())obs_property_list_add_int(app,entry.second.c_str(),entry.first);
    auto *preset=obs_properties_add_list(p,"preset","Stream preset",OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(preset,"Custom / keep current values",0);obs_property_list_add_int(preset,"Low latency · 1080p60",1);
    obs_property_list_add_int(preset,"Balanced · 1080p60",2);obs_property_list_add_int(preset,"Recording quality · 4K60",3);
    obs_properties_add_button2(p,"apply_preset","Apply selected preset",applyPreset,r);
    auto *resolution=obs_properties_add_list(p,"resolution","Stream resolution",OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(resolution,"1920 × 1080",0);obs_property_list_add_int(resolution,"1280 × 720",1);
    obs_property_list_add_int(resolution,"2560 × 1440",2);obs_property_list_add_int(resolution,"3840 × 2160",3);obs_property_list_add_int(resolution,"Custom",4);
    obs_property_set_modified_callback(resolution,visibility);
    obs_properties_add_int(p,"width","Custom width",320,7680,2);obs_properties_add_int(p,"height","Custom height",240,4320,2);
    obs_properties_add_int(p,"fps","Stream frame rate",1,240,1);
    obs_properties_add_int(p,"bitrate","Stream bitrate (kbps)",500,300000,500);
    auto *codec=obs_properties_add_list(p,"codec","Video codec",OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(codec,"Automatic",0);obs_property_list_add_int(codec,"H.264",1);obs_property_list_add_int(codec,"HEVC / H.265",2);obs_property_list_add_int(codec,"AV1",3);
    auto *decoder=obs_properties_add_list(p,"decoder","Video decoder",OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(decoder,"Automatic (hardware preferred)",0);obs_property_list_add_int(decoder,"Require hardware",1);obs_property_list_add_int(decoder,"Software",2);
    obs_properties_add_bool(p,"hdr","HDR (10-bit PQ; configure OBS for HDR recording)");
    obs_properties_add_bool(p,"full_range","Request full-range color (Moonlight 6.2 default)");
    auto *channels=obs_properties_add_list(p,"channels","Stream audio layout",OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(channels,"Stereo",2);obs_property_list_add_int(channels,"5.1 surround",6);obs_property_list_add_int(channels,"7.1 surround",8);
    obs_properties_add_bool(p,"host_audio","Keep audio playing on the host PC");
    obs_properties_add_int_slider(p,"buffer","Receive buffering (ms)",0,250,5);
    obs_properties_add_int(p,"audio_offset","Additional audio sync offset (ms)",-1000,1000,1);
    auto *network=obs_properties_add_list(p,"network","Network optimization",OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(network,"Local network",0);obs_property_list_add_int(network,"Remote network",1);obs_property_list_add_int(network,"Automatic",2);
    obs_properties_add_bool(p,"reconnect","Automatically reconnect after a drop");
    obs_properties_add_bool(p,"close_hidden","Disconnect when source is inactive");
    obs_properties_add_bool(p,"enabled","Connection enabled (restored with scene collection)");
    obs_properties_add_button2(p,"connect","Connect",connectButton,r);obs_properties_add_button2(p,"disconnect","Disconnect",disconnectButton,r);
    std::string status=r?r->status():"Disconnected";
    obs_properties_add_text(p,"status",status.c_str(),OBS_TEXT_INFO);
    obs_properties_add_button2(p,"refresh_status","Refresh connection status",refreshButton,r);
    obs_properties_add_text(p,"notice","Receive only. No gameplay input. Changes reconnect the source. No separate player window. OBS controls recording encoding.",OBS_TEXT_INFO);
    if(!r)for(const char *key:{"pair","apps","connect","disconnect","apply_preset"})obs_property_set_enabled(obs_properties_get(p,key),false);
    return p;
}
}

bool obs_module_load(void) {
    try {
        char *path=obs_module_config_path("");
        if(!path)return false;
        std::string configPath(path);bfree(path);initializeIdentity(configPath);
        obs_source_info info{};info.id="obs_gamestream_receiver";info.type=OBS_SOURCE_TYPE_INPUT;
        info.output_flags=OBS_SOURCE_ASYNC_VIDEO|OBS_SOURCE_AUDIO;
        info.get_name=name;info.create=create;info.destroy=destroy;info.update=update;
        info.get_defaults=defaults;info.get_properties=properties;info.activate=activate;info.deactivate=deactivate;
        obs_register_source(&info);
        blog(LOG_INFO,"[GameStream Receiver] Loaded. Moonlight 6.2.0 core, receive-only, no playback window.");return true;
    } catch(const std::exception &e){blog(LOG_ERROR,"[GameStream Receiver] Initialization failed: %s",e.what());return false;}
}
