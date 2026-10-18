#include "http-transport.hpp"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <algorithm>
#include <chrono>
#include <memory>
#include <climits>
#include <QTcpSocket>
#include <QEventLoop>
#include <QTimer>

namespace ReceiverHttp {
namespace {
constexpr qsizetype MaximumBody=8*1024*1024;
constexpr qsizetype MaximumHeader=64*1024;
[[noreturn]] void malformed(){throw std::runtime_error("Invalid or oversized HTTP response from host");}
quint64 number(const QByteArray &text,int base) {
    if(text.isEmpty() || text.startsWith('-') || text.startsWith('+'))malformed();
    bool ok=false;quint64 value=text.toULongLong(&ok,base);if(!ok || value>static_cast<quint64>(MaximumBody))malformed();return value;
}
struct PinCheck {const PeerCertificate *pin;bool mismatch=false;};
int verifyPin(X509_STORE_CTX *store,void *argument) {
    auto *check=static_cast<PinCheck*>(argument);
    try {if(check->pin->matches(X509_STORE_CTX_get0_cert(store)))return 1;}catch(...){}
    check->mismatch=true;X509_STORE_CTX_set_error(store,X509_V_ERR_APPLICATION_VERIFICATION);return 0;
}
using Clock=std::chrono::steady_clock;
struct Deadline {
    const std::atomic<bool> *cancel;Clock::time_point until;
    void check() const {
        if(cancel && cancel->load())throw std::runtime_error("Host request cancelled");
        if(Clock::now()>=until)throw std::runtime_error("Host request timed out");
    }
};
[[noreturn]] void networkError(const char *operation) {
    // Do not print request URLs, pairing secrets or client private keys.
    char description[256]={};auto error=ERR_get_error();if(error)ERR_error_string_n(error,description,sizeof(description));
    throw std::runtime_error(std::string(operation)+(error?std::string(": ")+description:" failed"));
}
}
Response parse(const QByteArray &raw,bool eof) {
    if(raw.size()>MaximumBody+MaximumHeader)malformed();
    Response response;auto end=raw.indexOf("\r\n\r\n");
    if(end<0){if(raw.size()>MaximumHeader || eof)malformed();return response;}
    if(end>MaximumHeader)malformed();
    auto lines=raw.left(end).split('\n');auto status=lines.takeFirst().trimmed().split(' ');
    if(status.size()<2 || (status[0]!="HTTP/1.0" && status[0]!="HTTP/1.1"))malformed();
    bool ok=false;response.status=status[1].toInt(&ok);if(!ok || response.status<200 || response.status>599)malformed();
    QByteArray length,transfer;
    for(auto line:lines){auto colon=line.indexOf(':');if(colon<=0)malformed();auto name=line.left(colon).trimmed().toLower();auto value=line.mid(colon+1).trimmed();
        if(name=="content-length"){if(!length.isNull())malformed();length=value;}
        if(name=="transfer-encoding"){if(!transfer.isNull())malformed();transfer=value.toLower();}
    }
    QByteArray body=raw.mid(end+4);
    if(!transfer.isNull()) {
        if(transfer!="chunked" || !length.isNull())malformed();
        qsizetype offset=0;
        while(true) {
            auto lineEnd=body.indexOf("\r\n",offset);if(lineEnd<0)break;
            if(lineEnd-offset>128)malformed();
            quint64 size=number(body.mid(offset,lineEnd-offset).split(';').first().trimmed(),16);offset=lineEnd+2;
            if(size==0) {
                if(body.mid(offset,2)=="\r\n" || body.indexOf("\r\n\r\n",offset)>=0)response.complete=true;
                break;
            }
            if(size>static_cast<quint64>(MaximumBody-response.body.size()))malformed();
            auto bytes=static_cast<qsizetype>(size);if(body.size()-offset<bytes+2)break;
            if(body.mid(offset+bytes,2)!="\r\n")malformed();
            response.body.append(body.constData()+offset,bytes);offset+=bytes+2;
        }
    } else if(!length.isNull()) {
        auto bytes=static_cast<qsizetype>(number(length,10));if(body.size()>bytes)malformed();
        if(body.size()==bytes){response.body=body;response.complete=true;}
    } else if(eof) { response.body=body;response.complete=true; }
    if(eof && !response.complete)malformed();
    return response;
}
Response get(const QUrl &url,const QByteArray &certificate,const QByteArray &key,
             const PeerCertificate &pin,const std::atomic<bool> *cancel,int timeoutMs) {
    const bool tls=url.scheme()=="https";
    if(!tls && url.scheme()!="http")throw std::runtime_error("Unsupported host URL scheme");
    if(url.host().isEmpty() || url.port()<1 || url.port()>65535)throw std::runtime_error("Invalid host address/port");
    if(tls && pin.isNull())throw PinError();
    Deadline deadline{cancel,Clock::now()+std::chrono::milliseconds(timeoutMs>0?timeoutMs:5000)};deadline.check();
    ERR_clear_error();
    std::unique_ptr<SSL_CTX,decltype(&SSL_CTX_free)> context(nullptr,SSL_CTX_free);
    PinCheck check{&pin};
    std::unique_ptr<SSL,decltype(&SSL_free)> tlsSession(nullptr,SSL_free);
    SSL *session=nullptr;
    if(tls) {
        context.reset(SSL_CTX_new(TLS_client_method()));if(!context)networkError("TLS context creation");
        if(SSL_CTX_set_min_proto_version(context.get(),TLS1_2_VERSION)!=1)networkError("TLS configuration");
        SSL_CTX_set_verify(context.get(),SSL_VERIFY_PEER,nullptr);
        SSL_CTX_set_cert_verify_callback(context.get(),verifyPin,&check);
        std::unique_ptr<BIO,decltype(&BIO_free)> certBio(BIO_new_mem_buf(certificate.constData(),static_cast<int>(certificate.size())),BIO_free);
        std::unique_ptr<BIO,decltype(&BIO_free)> keyBio(BIO_new_mem_buf(key.constData(),static_cast<int>(key.size())),BIO_free);
        std::unique_ptr<X509,decltype(&X509_free)> cert(certBio?PEM_read_bio_X509(certBio.get(),nullptr,nullptr,nullptr):nullptr,X509_free);
        std::unique_ptr<EVP_PKEY,decltype(&EVP_PKEY_free)> privateKey(keyBio?PEM_read_bio_PrivateKey(keyBio.get(),nullptr,nullptr,nullptr):nullptr,EVP_PKEY_free);
        if(!cert || !privateKey || SSL_CTX_use_certificate(context.get(),cert.get())!=1 || SSL_CTX_use_PrivateKey(context.get(),privateKey.get())!=1 || SSL_CTX_check_private_key(context.get())!=1)networkError("Client identity loading");
        tlsSession.reset(SSL_new(context.get()));session=tlsSession.get();
        if(!session)networkError("TLS session creation");
        BIO *input=BIO_new(BIO_s_mem()),*output=BIO_new(BIO_s_mem());
        if(!input || !output){BIO_free(input);BIO_free(output);networkError("TLS buffer creation");}
        SSL_set_bio(session,input,output);SSL_set_connect_state(session);
#ifdef SSL_OP_IGNORE_UNEXPECTED_EOF
        SSL_set_options(session,SSL_OP_IGNORE_UNEXPECTED_EOF);
#endif
    }
    auto host=url.host();if(host.startsWith('[') && host.endsWith(']'))host=host.mid(1,host.size()-2);
    auto hostUtf8=host.toUtf8();
    if(session && SSL_set_tlsext_host_name(session,hostUtf8.constData())!=1)networkError("TLS host configuration");
    // Qt supplies ordinary TCP/DNS only. TLS and certificate parsing are OpenSSL.
    QTcpSocket socket;QEventLoop connecting;QTimer timer;
    QObject::connect(&socket,&QTcpSocket::connected,&connecting,&QEventLoop::quit);
    QObject::connect(&socket,&QTcpSocket::errorOccurred,&connecting,&QEventLoop::quit);
    QObject::connect(&timer,&QTimer::timeout,&connecting,[&]{
        if((cancel && cancel->load()) || Clock::now()>=deadline.until)connecting.quit();
    });
    timer.start(5);socket.connectToHost(host,static_cast<quint16>(url.port()));
    if(socket.state()!=QAbstractSocket::ConnectedState && socket.state()!=QAbstractSocket::UnconnectedState)connecting.exec(QEventLoop::ExcludeUserInputEvents);
    timer.stop();deadline.check();
    if(socket.state()!=QAbstractSocket::ConnectedState)throw std::runtime_error("Host TCP connection failed: "+socket.errorString().toStdString());
    auto send=[&](const char *data,qint64 length){
        qint64 offset=0;
        while(offset<length){deadline.check();auto count=socket.write(data+offset,length-offset);if(count<0)throw std::runtime_error("Host TCP write failed");offset+=count;
            while(socket.bytesToWrite()>0){deadline.check();socket.waitForBytesWritten(5);if(socket.state()==QAbstractSocket::UnconnectedState)throw std::runtime_error("Host disconnected during write");}
        }
    };
    auto flush=[&]{
        if(!session)return;
        char data[16384];auto *output=SSL_get_wbio(session);
        while(BIO_ctrl_pending(output)>0){int count=BIO_read(output,data,sizeof(data));if(count<=0)networkError("TLS outgoing buffer");send(data,count);}
    };
    auto receive=[&]{
        while(socket.bytesAvailable()==0){deadline.check();if(socket.state()==QAbstractSocket::UnconnectedState)return false;socket.waitForReadyRead(5);}
        if(session){auto data=socket.read(16384);if(BIO_write(SSL_get_rbio(session),data.constData(),static_cast<int>(data.size()))!=data.size())networkError("TLS incoming buffer");}
        return true;
    };
    auto retryTls=[&](int result){
        int error=SSL_get_error(session,result);flush();
        if(check.mismatch)throw PinError();
        if(error==SSL_ERROR_WANT_READ){if(!receive())throw std::runtime_error("Host disconnected during TLS exchange");}
        else if(error!=SSL_ERROR_WANT_WRITE)networkError("Host TLS exchange");
    };
    if(session){while(true){deadline.check();int result=SSL_do_handshake(session);if(result==1){flush();break;}retryTls(result);}}
    if(session) {
        std::unique_ptr<X509,decltype(&X509_free)> peer(SSL_get1_peer_certificate(session),X509_free);
        if(!pin.matches(peer.get()))throw PinError();
    }
    auto path=url.path(QUrl::FullyEncoded).toUtf8();if(path.isEmpty())path="/";
    if(url.hasQuery())path+="?"+url.query(QUrl::FullyEncoded).toUtf8();
    QByteArray request="GET "+path+" HTTP/1.1\r\nHost: "+url.authority(QUrl::FullyEncoded).toUtf8()+"\r\nConnection: close\r\nAccept-Encoding: identity\r\n\r\n";
    if(session){
        qsizetype offset=0;
        while(offset<request.size()){
            deadline.check();int count=SSL_write(session,request.constData()+offset,static_cast<int>(std::min<qsizetype>(request.size()-offset,INT_MAX)));
            if(count>0){offset+=count;flush();}else retryTls(count);
        }
    }else send(request.constData(),request.size());
    QByteArray raw;char buffer[16384];
    while(true){
        deadline.check();
        if(session){
            int count=SSL_read(session,buffer,sizeof(buffer));
            if(count>0)raw.append(buffer,count);
            else{
                int error=SSL_get_error(session,count);flush();
                if(error==SSL_ERROR_ZERO_RETURN)return parse(raw,true);
                if(error==SSL_ERROR_WANT_READ){if(!receive())return parse(raw,true);continue;}
                if(error==SSL_ERROR_WANT_WRITE)continue;
                networkError("Host TLS response read");
            }
        }else{if(!receive())return parse(raw,true);raw.append(socket.read(16384));}
        auto response=parse(raw,false);if(response.complete)return response;
    }
}
}
