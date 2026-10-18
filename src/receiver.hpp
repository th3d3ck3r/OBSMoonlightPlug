#pragma once
#include "media.hpp"
#include <QThread>
#include <condition_variable>
#include <atomic>
#include <utility>

class Receiver {
public:
    enum class Job { Update, Pair, Apps, Stop };
    explicit Receiver(obs_source_t *source);
    ~Receiver();
    void update(const Settings &settings);
    void request(Job job, const Settings &settings, std::string pin={});
    void active(bool active);
    std::string status() const;
    std::vector<std::pair<int,std::string>> apps() const;
    obs_source_t *source() const {return source_;}
    Media media;
    void message(std::string text);
    void terminated(int error);
private:
    void run();
    void perform(Job job,const Settings &settings,const std::string &pin);
    void connectStream(const Settings &settings);
    void hostOperation(Job job,const Settings &settings,const std::string &pin);
    void notifyProperties();
    obs_source_t *source_;
    QThread *thread_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    Settings desired_;
    Job next_=Job::Stop;
    std::string pin_, status_="Disconnected. Pair with a host to begin.";
    std::vector<std::pair<int,std::string>> apps_;
    bool pending_=false, closing_=false, active_=true;
    std::atomic<bool> cancel_{false}, ended_{false}, starting_{false};
};

void initializeIdentity(const std::string &directory);
