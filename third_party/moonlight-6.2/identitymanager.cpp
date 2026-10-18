#include <stdexcept>
#include "identitymanager.h"
#include "utils.h"

#include <QDebug>

#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/rand.h>

#define SER_UNIQUEID "uniqueid"
#define SER_CERT "certificate"
#define SER_KEY "key"

IdentityManager* IdentityManager::s_Im = nullptr;
QString IdentityManager::s_StoragePath;
void IdentityManager::setStoragePath(QString path) { s_StoragePath = std::move(path); }

IdentityManager*
IdentityManager::get()
{
    // This will always be called first on the main thread,
    // so it's safe to initialize without locks.
    if (s_Im == nullptr) {
        s_Im = new IdentityManager();
    }

    return s_Im;
}

void IdentityManager::createCredentials(QSettings& settings)
{
    X509* cert = X509_new();
    THROW_BAD_ALLOC_IF_NULL(cert);

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    EVP_PKEY* pk = EVP_RSA_gen(2048);
    THROW_BAD_ALLOC_IF_NULL(pk);
#else
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
    THROW_BAD_ALLOC_IF_NULL(ctx);

    EVP_PKEY_keygen_init(ctx);
    EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048);

    // pk must be initialized on input
    EVP_PKEY* pk = NULL;
    EVP_PKEY_keygen(ctx, &pk);

    EVP_PKEY_CTX_free(ctx);
    THROW_BAD_ALLOC_IF_NULL(pk);
#endif

    X509_set_version(cert, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(cert), 0);
#if OPENSSL_VERSION_NUMBER < 0x10100000L
    X509_gmtime_adj(X509_get_notBefore(cert), 0);
    X509_gmtime_adj(X509_get_notAfter(cert), 60 * 60 * 24 * 365 * 20); // 20 yrs
#else
    X509_gmtime_adj(X509_getm_notBefore(cert), 0);
    X509_gmtime_adj(X509_getm_notAfter(cert), 60 * 60 * 24 * 365 * 20); // 20 yrs
#endif

    X509_set_pubkey(cert, pk);

    X509_NAME* name = X509_NAME_new();
    THROW_BAD_ALLOC_IF_NULL(name);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               reinterpret_cast<unsigned char *>(const_cast<char*>("NVIDIA GameStream Client")),
                               -1, -1, 0);
    X509_set_subject_name(cert, name);
    X509_set_issuer_name(cert, name);
    X509_NAME_free(name);

    X509_sign(cert, pk, EVP_sha256());

    BIO* biokey = BIO_new(BIO_s_mem());
    THROW_BAD_ALLOC_IF_NULL(biokey);
    PEM_write_bio_PrivateKey(biokey, pk, NULL, NULL, 0, NULL, NULL);

    BIO* biocert = BIO_new(BIO_s_mem());
    THROW_BAD_ALLOC_IF_NULL(biocert);
    PEM_write_bio_X509(biocert, cert);

    BUF_MEM* mem;
    BIO_get_mem_ptr(biokey, &mem);
    m_CachedPrivateKey = QByteArray(mem->data, (int)mem->length);

    BIO_get_mem_ptr(biocert, &mem);
    m_CachedPemCert = QByteArray(mem->data, (int)mem->length);

    X509_free(cert);
    EVP_PKEY_free(pk);
    BIO_free(biokey);
    BIO_free(biocert);

    // Check that the new keypair is valid before persisting it
    if (!credentialsValid()) {
        throw std::runtime_error("Newly generated certificate is unreadable");
    }
    if (!credentialsValid()) {
        throw std::runtime_error("Newly generated private key is unreadable");
    }

    settings.setValue(SER_CERT, m_CachedPemCert);
    settings.setValue(SER_KEY, m_CachedPrivateKey);

    qInfo() << "Wrote new identity credentials to settings";
}

IdentityManager::IdentityManager()
{
    QSettings settings(s_StoragePath, QSettings::IniFormat);

    m_CachedPemCert = settings.value(SER_CERT).toByteArray();
    m_CachedPrivateKey = settings.value(SER_KEY).toByteArray();

    if (m_CachedPemCert.isEmpty() || m_CachedPrivateKey.isEmpty()) {
        qInfo() << "No existing credentials found";
        createCredentials(settings);
    }
    else if (!credentialsValid()) {
        qWarning() << "Invalid stored credentials; generating a new identity";
        createCredentials(settings);
    }
    if (!credentialsValid()) throw std::runtime_error("Identity certificate/key validation failed");

    // Load the unique ID from settings
    m_CachedUniqueId = settings.value(SER_UNIQUEID).toString();
    if (!m_CachedUniqueId.isEmpty()) {
        qInfo() << "Loaded unique ID from settings:" << m_CachedUniqueId;
    }
    else {
        // Generate a new unique ID in base 16
        uint64_t uid;
        RAND_bytes(reinterpret_cast<unsigned char*>(&uid), sizeof(uid));
        m_CachedUniqueId = QString::number(uid, 16);

        qInfo() << "Generated new unique ID:" << m_CachedUniqueId;

        settings.setValue(SER_UNIQUEID, m_CachedUniqueId);
    }
}

bool IdentityManager::credentialsValid() const
{
    BIO *certBio=BIO_new_mem_buf(m_CachedPemCert.constData(),static_cast<int>(m_CachedPemCert.size()));
    BIO *keyBio=BIO_new_mem_buf(m_CachedPrivateKey.constData(),static_cast<int>(m_CachedPrivateKey.size()));
    X509 *cert=certBio?PEM_read_bio_X509(certBio,nullptr,nullptr,nullptr):nullptr;
    EVP_PKEY *key=keyBio?PEM_read_bio_PrivateKey(keyBio,nullptr,nullptr,nullptr):nullptr;
    bool valid=cert && key && X509_check_private_key(cert,key)==1;
    BIO_free(certBio);BIO_free(keyBio);X509_free(cert);EVP_PKEY_free(key);
    return valid;
}

QString
IdentityManager::getUniqueId()
{
    return m_CachedUniqueId;
}

QByteArray
IdentityManager::getCertificate()
{
    return m_CachedPemCert;
}

QByteArray
IdentityManager::getPrivateKey()
{
    return m_CachedPrivateKey;
}
