#pragma once
#include "peer-certificate.h"
#include <QUrl>
#include <atomic>
#include <stdexcept>

namespace ReceiverHttp {
struct Response { int status=0;QByteArray body;bool complete=false; };
class PinError : public std::runtime_error { public: PinError():std::runtime_error("Host certificate pin mismatch"){} };
Response parse(const QByteArray &raw,bool eof);
Response get(const QUrl &url,const QByteArray &certificate,const QByteArray &key,
             const PeerCertificate &pin,const std::atomic<bool> *cancel,int timeoutMs);
}
