#pragma once

#include "peer-certificate.h"
#include <QSettings>

class IdentityManager
{
public:
    static void setStoragePath(QString path);
    QString
    getUniqueId();

    QByteArray
    getCertificate();

    QByteArray
    getPrivateKey();


    static
    IdentityManager*
    get();

private:
    IdentityManager();

    bool credentialsValid() const;

    void
    createCredentials(QSettings& settings);

    // Initialized in constructor
    QByteArray m_CachedPrivateKey;
    QByteArray m_CachedPemCert;

    // Lazy initialized
    QString m_CachedUniqueId;

    static IdentityManager* s_Im;
    static QString s_StoragePath;
};
