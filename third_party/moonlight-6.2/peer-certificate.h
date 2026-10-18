#pragma once
#include <QByteArray>
#include <openssl/pem.h>

// Store canonical DER for exact host pinning without a Qt TLS backend.
class PeerCertificate {
public:
    PeerCertificate() = default;
    explicit PeerCertificate(const QByteArray &pem) {
        BIO *input=BIO_new_mem_buf(pem.constData(),static_cast<int>(pem.size()));
        X509 *cert=input?PEM_read_bio_X509(input,nullptr,nullptr,nullptr):nullptr;
        BIO_free(input);
        if(cert) {
            int length=i2d_X509(cert,nullptr);
            if(length>0) { der_.resize(length);auto *out=reinterpret_cast<unsigned char*>(der_.data());i2d_X509(cert,&out);pem_=pem; }
            X509_free(cert);
        }
    }
    bool isNull() const { return der_.isEmpty(); }
    QByteArray toPem() const { return pem_; }
    bool matches(X509 *cert) const {
        if(isNull() || !cert)return false;
        int length=i2d_X509(cert,nullptr);if(length!=der_.size())return false;
        QByteArray der(length,0);auto *out=reinterpret_cast<unsigned char*>(der.data());
        return i2d_X509(cert,&out)==length && der==der_;
    }
    bool operator==(const PeerCertificate &other) const { return der_==other.der_; }
    bool operator!=(const PeerCertificate &other) const { return !(*this==other); }
private:
    QByteArray der_,pem_;
};
