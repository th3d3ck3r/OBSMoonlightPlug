#include "receiver.hpp"
#include "identitymanager.h"
#include "nvhttp.h"
#include "nvpairingmanager.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <openssl/rand.h>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <memory>
#ifdef __APPLE__
#include "worker-wire.hpp"
#include <QProcess>
#endif

namespace {
#ifndef __APPLE__
std::mutex sessionMutex;
#endif
std::atomic<Receiver*> current{nullptr};
std::string storageDirectory;
#ifdef __APPLE__
std::string workerBinaryPath;
#endif

QString hostStore(const Settings &s) {
    auto key=QCryptographicHash::hash(QByteArray::fromStdString(s.host+":"+std::to_string(s.port)),QCryptographicHash::Sha256).toHex();
    return QString::fromStdString(storageDirectory)+"/host-"+key+".ini";
}
PeerCertificate loadPin(const Settings &s) {
    QSettings store(hostStore(s),QSettings::IniFormat);
    return PeerCertificate(store.value("certificate").toByteArray());
}
void savePin(const Settings &s,const PeerCertificate &cert) {
    QSettings store(hostStore(s),QSettings::IniFormat);store.setValue("certificate",cert.toPem());store.sync();
    if(store.status()!=QSettings::NoError)throw std::runtime_error("Cannot save paired host certificate");
    QFile::setPermissions(hostStore(s),QFileDevice::ReadOwner|QFileDevice::WriteOwner);
}
Receiver *owner(){return current.load();}
int videoSetup(int f,int w,int h,int r,void *ctx,int){try{return static_cast<Receiver*>(ctx)->media.videoSetup(f,w,h,r);}catch(...){return -1;}}
int videoFrame(PDECODE_UNIT du){try{auto *r=owner();return r?r->media.videoFrame(du):DR_NEED_IDR;}catch(...){return DR_NEED_IDR;}}
void videoCleanup(){if(auto *r=owner())r->media.videoCleanup();}
int audioSetup(int,OPUS_MULTISTREAM_CONFIGURATION *c,void *ctx,int){try{return static_cast<Receiver*>(ctx)->media.audioSetup(c);}catch(...){return -1;}}
void audioFrame(char *data,int len){try{if(auto *r=owner())r->media.audioFrame(data,len);}catch(...){}}
void audioCleanup(){if(auto *r=owner())r->media.audioCleanup();}
void started(){if(auto *r=owner())r->message("Connected — receiving video and audio. Input disabled.");}
void terminated(int error){if(auto *r=owner())r->terminated(error);}
void stageStarting(int stage){if(auto *r=owner())r->message(std::string("Connecting: ")+LiGetStageName(stage));}
void stageFailed(int stage,int error){if(auto *r=owner())r->message(std::string("Connection failed at ")+LiGetStageName(stage)+" ("+std::to_string(error)+")");}
void logMessage(const char *format,...) {
    char message[1024];va_list args;va_start(args,format);std::vsnprintf(message,sizeof(message),format,args);va_end(args);
    blog(LOG_DEBUG,"[GameStream Receiver] %s",message);
}
int supportedFormats(const Settings &s,int hostModes) {
    int formats=0;
    if((s.codec==0 || s.codec==1) && !s.hdr && Media::hasCodec(AV_CODEC_ID_H264) && (hostModes&SCM_H264))formats|=VIDEO_FORMAT_H264;
    if((s.codec==0 || s.codec==2) && Media::hasCodec(AV_CODEC_ID_HEVC)) {
        if(s.hdr && (hostModes&SCM_HEVC_MAIN10))formats|=VIDEO_FORMAT_H265_MAIN10;
        else if(!s.hdr && (hostModes&SCM_HEVC))formats|=VIDEO_FORMAT_H265;
    }
    if((s.codec==0 || s.codec==3) && Media::hasCodec(AV_CODEC_ID_AV1)) {
        if(s.hdr && (hostModes&SCM_AV1_MAIN10))formats|=VIDEO_FORMAT_AV1_MAIN10;
        else if(!s.hdr && (hostModes&SCM_AV1_MAIN8))formats|=VIDEO_FORMAT_AV1_MAIN8;
    }
    return formats;
}
}

#ifdef __APPLE__
void initializeReceiverWorker(const std::string &path) { workerBinaryPath=path; }
#endif

void initializeIdentity(const std::string &directory) {
    storageDirectory=directory;
    if(!QDir().mkpath(QString::fromStdString(directory)))throw std::runtime_error("Cannot create OBS receiver configuration directory");
    auto path=QString::fromStdString(directory)+"/identity.ini";
    IdentityManager::setStoragePath(path);
    IdentityManager::get();
    QFile::setPermissions(path,QFileDevice::ReadOwner|QFileDevice::WriteOwner);
}

Receiver::Receiver(obs_source_t *source):media(source),source_(source) {
    thread_=QThread::create([this]{run();});thread_->setObjectName("OBS GameStream Receiver");thread_->start();
}
Receiver::~Receiver() {
    {
        std::lock_guard<std::mutex> lock(mutex_);closing_=true;cancel_=true;
    }
    if(current.load()==this && starting_.load())LiInterruptConnection();
    changed_.notify_all();thread_->wait();delete thread_;
}
void Receiver::request(Job job,const Settings &s,std::string pin) {
    {
        std::lock_guard<std::mutex> lock(mutex_);desired_=s;next_=job;pin_=std::move(pin);pending_=true;cancel_=true;
    }
    if(current.load()==this && starting_.load())LiInterruptConnection();
    changed_.notify_all();
}
void Receiver::update(const Settings &s) {
    {std::lock_guard<std::mutex> lock(mutex_);if(desired_==s)return;}
    request(s.enabled?Job::Update:Job::Stop,s);
}
void Receiver::active(bool active) {
    Settings s;
    {std::lock_guard<std::mutex> lock(mutex_);active_=active;s=desired_;}
    if(s.closeHidden)request(active&&s.enabled?Job::Update:Job::Stop,s);
}
std::string Receiver::status() const {
    std::string status;{std::lock_guard<std::mutex> lock(mutex_);status=status_;}
    return status+"\n"+media.statistics();
}
std::vector<std::pair<int,std::string>> Receiver::apps() const {std::lock_guard<std::mutex> lock(mutex_);return apps_;}
void Receiver::message(std::string text) {
    {std::lock_guard<std::mutex> lock(mutex_);status_=std::move(text);}
    notifyProperties();
}
void Receiver::notifyProperties() {
    auto *weak=obs_source_get_weak_source(source_);
    obs_queue_task(OBS_TASK_UI,[](void *data) {
        auto *weak=static_cast<obs_weak_source_t*>(data);
        auto *source=obs_weak_source_get_source(weak);
        if(source) {obs_source_update_properties(source);obs_source_release(source);}
        obs_weak_source_release(weak);
    },weak,false);
}
void Receiver::terminated(int error) {
    message("Stream ended ("+std::to_string(error)+").");ended_=true;changed_.notify_all();
}
void Receiver::run() {
    for(;;) {
        Settings s;Job job;std::string pin;
        {
            std::unique_lock<std::mutex> lock(mutex_);changed_.wait(lock,[this]{return closing_||pending_;});
            if(closing_)return;
            s=desired_;job=next_;pin=pin_;pending_=false;cancel_=false;
            if(s.closeHidden&&!active_&&job==Job::Update)job=Job::Stop;
        }
        int delay=1;
        for(;;) {
            bool retry=false;
            try {perform(job,s,pin);retry=job==Job::Update && s.enabled && s.reconnect;}
            catch(const std::exception &e) {if(!cancel_)message(std::string("Error: ")+e.what());retry=job==Job::Update&&s.enabled&&s.reconnect;}
            if(!retry||cancel_)break;
            message("Disconnected. Retrying in "+std::to_string(delay)+" seconds.");
            std::unique_lock<std::mutex> lock(mutex_);
            if(changed_.wait_for(lock,std::chrono::seconds(delay),[this]{return closing_||pending_;}))break;
            delay=std::min(delay*2,30);
        }
    }
}
void Receiver::perform(Job job,const Settings &s,const std::string &pin) {
    if(job==Job::Stop){obs_source_output_video(source_,nullptr);message("Disconnected — no host input sent.");return;}
    s.validate();
    if(job==Job::Pair||job==Job::Apps)hostOperation(job,s,pin);
    else connectStream(s);
}
void Receiver::hostOperation(Job job,const Settings &s,const std::string &pin) {
    NvAddress address(QString::fromStdString(s.host),static_cast<uint16_t>(s.port));
    NvHTTP http(address,0,loadPin(s),true);http.setCancellationFlag(&cancel_);
    auto info=http.getServerInfo(NvHTTP::NVLL_ERROR);
    if(job==Job::Pair) {
        if(pin.size()!=4 || pin.find_first_not_of("0123456789")!=std::string::npos)throw std::runtime_error("Choose a four-digit PIN, then enter it in the host's pairing page");
        message("Pairing. Enter the displayed PIN in Apollo/Sunshine's host pairing page.");
        auto version=NvHTTP::getXmlString(info,"appversion");
        if(NvHTTP::parseQuad(version).isEmpty())throw std::runtime_error("Host did not report a valid version");
        NvPairingManager pairing(address,http.httpsPort(),http.serverCert(),&cancel_);
        PeerCertificate cert;
        auto result=pairing.pair(version,QString::fromStdString(pin),cert);
        if(result!=NvPairingManager::PAIRED)throw std::runtime_error(result==NvPairingManager::PIN_WRONG?"Pairing PIN did not match":"Pairing failed; check the host pairing page");
        savePin(s,cert);http.setServerCert(cert);message("Paired. Refresh the app list, then connect.");
    }
    auto list=http.getAppList();
    {std::lock_guard<std::mutex> lock(mutex_);apps_.clear();for(const auto &app:list)apps_.emplace_back(app.id,app.name.toStdString());}
    if(job==Job::Apps)message("Host app list refreshed.");else notifyProperties();
}
void Receiver::connectStream(const Settings &s) {
#ifndef __APPLE__
    std::unique_lock<std::mutex> ownership(sessionMutex,std::try_to_lock);
    if(!ownership.owns_lock())throw std::runtime_error("Only one GameStream Receiver can be connected per OBS process");
#endif
    NvAddress address(QString::fromStdString(s.host),static_cast<uint16_t>(s.port));
    auto cert=loadPin(s);if(cert.isNull())throw std::runtime_error("Pair this OBS receiver with the host first");
    NvHTTP http(address,0,cert,true);http.setCancellationFlag(&cancel_);
    auto info=http.getServerInfo(NvHTTP::NVLL_ERROR);
    if(NvHTTP::getXmlString(info,"PairStatus")!="1")throw std::runtime_error("Host does not recognize this receiver; pair again");
    int app=s.app;
    if(app==0) {
        auto list=http.getAppList();
        for(const auto &candidate:list)if(candidate.name.compare("Desktop",Qt::CaseInsensitive)==0){app=candidate.id;break;}
        if(!app)throw std::runtime_error("No app named Desktop found. Refresh the app list and choose an app");
    }
    int currentApp=NvHTTP::getCurrentGame(info);
    if(currentApp!=0 && currentApp!=app)throw std::runtime_error("A different host app is running. Choose it or stop that session on the host first");
    STREAM_CONFIGURATION config;LiInitializeStreamConfiguration(&config);
    config.width=s.width;config.height=s.height;config.fps=s.fps;config.bitrate=s.bitrate;
    config.packetSize=1392;config.streamingRemotely=s.network==1?STREAM_CFG_REMOTE:s.network==2?STREAM_CFG_AUTO:STREAM_CFG_LOCAL;
    config.audioConfiguration=s.channels==8?AUDIO_CONFIGURATION_71_SURROUND:s.channels==6?AUDIO_CONFIGURATION_51_SURROUND:AUDIO_CONFIGURATION_STEREO;
    config.colorSpace=s.hdr?COLORSPACE_REC_2020:COLORSPACE_REC_709;
    config.colorRange=s.fullRange?COLOR_RANGE_FULL:COLOR_RANGE_LIMITED;
    config.encryptionFlags=ENCFLG_ALL;config.clientRefreshRateX100=s.fps*100;
    int hostModes=NvHTTP::getXmlString(info,"ServerCodecModeSupport").toInt();
    if(!hostModes)hostModes=SCM_H264;
    config.supportedVideoFormats=supportedFormats(s,hostModes);
    if(!config.supportedVideoFormats)throw std::runtime_error("Requested codec/HDR mode is not supported by both host and receiver");
    if(RAND_bytes(reinterpret_cast<unsigned char*>(config.remoteInputAesKey),sizeof(config.remoteInputAesKey))!=1 || RAND_bytes(reinterpret_cast<unsigned char*>(config.remoteInputAesIv),4)!=1)throw std::runtime_error("Cannot generate stream session keys");
    QString url;
    http.startApp(currentApp==app?"resume":"launch",false,app,&config,false,s.hostAudio,0,false,url);
    if(cancel_)return;
    auto addressBytes=QString::fromStdString(s.host).toUtf8();
    auto appVersion=NvHTTP::getXmlString(info,"appversion").toUtf8();
    auto gfeVersion=NvHTTP::getXmlString(info,"GfeVersion").toUtf8();auto sessionUrl=url.toUtf8();
    SERVER_INFORMATION server;LiInitializeServerInformation(&server);
    server.address=addressBytes.constData();server.serverInfoAppVersion=appVersion.constData();
    server.serverInfoGfeVersion=gfeVersion.isEmpty()?nullptr:gfeVersion.constData();server.rtspSessionUrl=sessionUrl.isEmpty()?nullptr:sessionUrl.constData();server.serverCodecModeSupport=hostModes;
    DECODER_RENDERER_CALLBACKS video;LiInitializeVideoCallbacks(&video);
    video.setup=videoSetup;video.submitDecodeUnit=videoFrame;video.cleanup=videoCleanup;
    AUDIO_RENDERER_CALLBACKS audio;LiInitializeAudioCallbacks(&audio);
    audio.init=audioSetup;audio.decodeAndPlaySample=audioFrame;audio.cleanup=audioCleanup;audio.capabilities=CAPABILITY_SUPPORTS_ARBITRARY_AUDIO_DURATION;
    CONNECTION_LISTENER_CALLBACKS listener;LiInitializeConnectionCallbacks(&listener);
    listener.stageStarting=stageStarting;listener.stageFailed=stageFailed;listener.connectionStarted=started;listener.connectionTerminated=::terminated;listener.logMessage=logMessage;

#ifdef __APPLE__
    // The Moonlight C core keeps connection state in process-wide globals.
    // Spawn one isolated streaming core per OBS source; decoding remains here.
    if(workerBinaryPath.empty())throw std::runtime_error("Mac receiver helper path is unavailable");
    media.configure(s);
    ended_=false;
    QProcess worker;
    worker.setProcessChannelMode(QProcess::SeparateChannels);
    worker.start(QString::fromStdString(workerBinaryPath));
    if(!worker.waitForStarted(5000))
        throw std::runtime_error("Cannot start bundled Moonlight helper: "+worker.errorString().toStdString());

    auto shutdown=[&] {
        if(worker.state()!=QProcess::NotRunning) {
            worker.write("X",1);
            worker.closeWriteChannel();
            if(!worker.waitForFinished(3000)) {
                worker.terminate();
                if(!worker.waitForFinished(1500)) {
                    worker.kill();
                    worker.waitForFinished(1500);
                }
            }
        }
        media.videoCleanup();
        media.audioCleanup();
        obs_source_output_video(source_,nullptr);
    };
    try {
        const std::string addr=s.host;
        const std::string version=appVersion.toStdString();
        const std::string gfe=gfeVersion.toStdString();
        const std::string rtsp=sessionUrl.toStdString();
        WorkerWire::Launch launch{};
        launch.stream=config;launch.hostCodecModes=hostModes;
        launch.addressSize=static_cast<uint32_t>(addr.size());
        launch.appVersionSize=static_cast<uint32_t>(version.size());
        launch.gfeVersionSize=static_cast<uint32_t>(gfe.size());
        launch.rtspUrlSize=static_cast<uint32_t>(rtsp.size());
        if(!WorkerWire::valid(launch))throw std::runtime_error("Invalid Moonlight worker configuration");
        QByteArray request(reinterpret_cast<const char*>(&launch),sizeof launch);
        request.append(addr.data(),static_cast<qsizetype>(addr.size()));
        request.append(version.data(),static_cast<qsizetype>(version.size()));
        request.append(gfe.data(),static_cast<qsizetype>(gfe.size()));
        request.append(rtsp.data(),static_cast<qsizetype>(rtsp.size()));
        if(worker.write(request)!=request.size())
            throw std::runtime_error("Could not initialize Moonlight helper");
        while(worker.bytesToWrite()>0)
            if(!worker.waitForBytesWritten(5000))
                throw std::runtime_error("Moonlight helper initialization timed out");

        QByteArray pending;
        for(;;) {
            if(cancel_ || ended_)break;
            worker.waitForReadyRead(30);
            pending+=worker.readAllStandardOutput();
            // A private framed binary stream. Never parse arbitrary stdout text as packets.
            while(pending.size()>=static_cast<qsizetype>(sizeof(WorkerWire::Header))) {
                WorkerWire::Header h{};
                std::memcpy(&h,pending.constData(),sizeof h);
                if(!WorkerWire::valid(h))throw std::runtime_error("Invalid Moonlight helper packet");
                const auto total=static_cast<qsizetype>(sizeof h)+static_cast<qsizetype>(h.length);
                if(pending.size()<total)break;
                const char *payload=pending.constData()+sizeof h;
                const auto type=static_cast<WorkerWire::Type>(h.type);
                switch(type) {
                case WorkerWire::Type::VideoSetup: {
                    if(h.length!=sizeof(WorkerWire::VideoSetup))throw std::runtime_error("Invalid video setup");
                    WorkerWire::VideoSetup v{};std::memcpy(&v,payload,sizeof v);
                    if(media.videoSetup(v.format,v.width,v.height,v.fps)!=0)
                        throw std::runtime_error("Cannot initialize video decoder");
                    break;
                }
                case WorkerWire::Type::VideoFrame: {
                    if(h.length<sizeof(WorkerWire::VideoFrame))throw std::runtime_error("Invalid video packet");
                    WorkerWire::VideoFrame v{};std::memcpy(&v,payload,sizeof v);
                    if(v.length==0 || v.length!=h.length-sizeof v)
                        throw std::runtime_error("Invalid video packet length");
                    LENTRY entry{};entry.data=const_cast<char*>(payload+sizeof v);
                    entry.length=static_cast<int>(v.length);
                    DECODE_UNIT unit{};unit.bufferList=&entry;unit.fullLength=entry.length;
                    unit.presentationTimeUs=v.ptsUs;unit.frameType=static_cast<int>(v.frameType);
                    media.videoFrame(&unit);
                    break;
                }
                case WorkerWire::Type::VideoCleanup:media.videoCleanup();break;
                case WorkerWire::Type::AudioSetup: {
                    if(h.length!=sizeof(OPUS_MULTISTREAM_CONFIGURATION))throw std::runtime_error("Invalid audio setup");
                    OPUS_MULTISTREAM_CONFIGURATION a{};std::memcpy(&a,payload,sizeof a);
                    if(media.audioSetup(&a)!=0)throw std::runtime_error("Cannot initialize Opus decoder");
                    break;
                }
                case WorkerWire::Type::AudioFrame:
                    if(h.length>0 && h.length<=65536)
                        media.audioFrame(const_cast<char*>(payload),static_cast<int>(h.length));
                    else throw std::runtime_error("Invalid Opus packet length");
                    break;
                case WorkerWire::Type::AudioCleanup:media.audioCleanup();break;
                case WorkerWire::Type::Status:
                    if(h.length<=2048)message(std::string(payload,payload+h.length));
                    break;
                case WorkerWire::Type::Ended:
                    if(h.length<=2048)message(std::string(payload,payload+h.length));
                    ended_=true;break;
                case WorkerWire::Type::Error:
                    throw std::runtime_error(std::string(payload,payload+std::min<uint32_t>(h.length,2048)));
                }
                pending.remove(0,total);
            }
            // Drain stderr separately to avoid blocking the child; never log launch secrets.
            worker.readAllStandardError();
            if(worker.state()==QProcess::NotRunning) {
                if(!ended_ && !cancel_)throw std::runtime_error(
                    "Moonlight worker exited (code "+std::to_string(worker.exitCode())+")");
                break;
            }
        }
    } catch(...) {
        shutdown();
        throw;
    }
    shutdown();

#else
    media.configure(s);ended_=false;current=this;starting_=true;
    int error=LiStartConnection(&server,&config,&listener,&video,&audio,this,0,this,0);
    starting_=false;
    if(error!=0){current=nullptr;media.videoCleanup();media.audioCleanup();throw std::runtime_error("Streaming connection failed ("+std::to_string(error)+")");}
    {
        std::unique_lock<std::mutex> lock(mutex_);changed_.wait(lock,[this]{return cancel_.load()||ended_.load()||closing_||pending_;});
    }
    LiStopConnection();current=nullptr;media.videoCleanup();media.audioCleanup();
    obs_source_output_video(source_,nullptr);
    // No /cancel call: disconnecting a recorder must not quit the host application.
#endif
}
