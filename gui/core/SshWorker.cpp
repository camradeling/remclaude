#include "SshWorker.h"

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QDir>

#include <libssh2.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <mutex>

namespace {

constexpr int kConnectTimeoutMs   = 10000;
constexpr int kBlockingTimeoutMs  = 20000;  // handshake / auth / channel open / write
constexpr int kKeepaliveSec       = 15;
constexpr int kPollMs             = 1000;

std::once_flag g_initOnce;

// Non-blocking connect with a timeout; returns fd or -1 (err filled in).
int tcpConnect(const QString& host, int port, QString& err)
{
    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const QByteArray h = host.toUtf8();
    const QByteArray p = QByteArray::number(port);
    int rc = getaddrinfo(h.constData(), p.constData(), &hints, &res);
    if (rc != 0) {
        err = QStringLiteral("cannot resolve %1: %2").arg(host, QString::fromUtf8(gai_strerror(rc)));
        return -1;
    }

    int fd = -1;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;

        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int c = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (c != 0 && errno == EINPROGRESS) {
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(fd, &wfds);
            timeval tv{kConnectTimeoutMs / 1000, 0};
            if (select(fd + 1, nullptr, &wfds, nullptr, &tv) == 1) {
                int soerr = 0;
                socklen_t len = sizeof(soerr);
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len);
                c = soerr == 0 ? 0 : -1;
                errno = soerr;
            } else {
                errno = ETIMEDOUT;
                c = -1;
            }
        }
        if (c == 0) {
            fcntl(fd, F_SETFL, flags);  // back to blocking; libssh2 manages its own mode
            int one = 1;
            setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
            break;
        }
        err = QStringLiteral("connect to %1:%2 failed: %3")
                  .arg(host).arg(port).arg(QString::fromUtf8(strerror(errno)));
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

int knownHostKeyBit(int hostkeyType)
{
    switch (hostkeyType) {
    case LIBSSH2_HOSTKEY_TYPE_RSA:       return LIBSSH2_KNOWNHOST_KEY_SSHRSA;
    case LIBSSH2_HOSTKEY_TYPE_DSS:       return LIBSSH2_KNOWNHOST_KEY_SSHDSS;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_256: return LIBSSH2_KNOWNHOST_KEY_ECDSA_256;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_384: return LIBSSH2_KNOWNHOST_KEY_ECDSA_384;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_521: return LIBSSH2_KNOWNHOST_KEY_ECDSA_521;
    case LIBSSH2_HOSTKEY_TYPE_ED25519:   return LIBSSH2_KNOWNHOST_KEY_ED25519;
    default:                             return LIBSSH2_KNOWNHOST_KEY_UNKNOWN;
    }
}

// Errors on channel open that mean the SSH transport itself is gone (so a
// reconnect is worth trying), as opposed to the remote side refusing.
bool isTransportError(int rc)
{
    return rc == LIBSSH2_ERROR_SOCKET_SEND || rc == LIBSSH2_ERROR_SOCKET_RECV ||
           rc == LIBSSH2_ERROR_SOCKET_DISCONNECT || rc == LIBSSH2_ERROR_SOCKET_TIMEOUT ||
           rc == LIBSSH2_ERROR_TIMEOUT || rc == LIBSSH2_ERROR_BAD_SOCKET;
}

}  // namespace

SshWorker::SshWorker(QObject* parent) : QObject(parent)
{
    qRegisterMetaType<SshConfig>("SshConfig");
    std::call_once(g_initOnce, [] { libssh2_init(0); });
}

SshWorker::~SshWorker()
{
    closeSession();
}

QString SshWorker::lastError() const
{
    if (!m_session) return QStringLiteral("no session");
    char* msg = nullptr;
    libssh2_session_last_error(m_session, &msg, nullptr, 0);
    return QString::fromUtf8(msg ? msg : "unknown error");
}

void SshWorker::connectTo(const SshConfig& cfg)
{
    closeSession();
    m_cfg = cfg;
    m_haveCfg = true;
    QString err;
    if (openSession(err)) {
        emit connectedChanged(true);
        emit reply(QStringLiteral("connect"), QJsonObject{{"ok", true}});
    } else {
        emit failed(QStringLiteral("connect"), err);
        emit connectedChanged(false);
    }
}

void SshWorker::disconnectFromHost()
{
    closeSession();
    emit connectedChanged(false);
}

bool SshWorker::openSession(QString& err)
{
    if (m_cfg.host.isEmpty() || m_cfg.user.isEmpty()) {
        err = QStringLiteral("host and user must be set");
        return false;
    }
    if (!QFileInfo::exists(m_cfg.keyPath)) {
        err = QStringLiteral("private key not found: %1").arg(m_cfg.keyPath);
        return false;
    }

    m_sock = tcpConnect(m_cfg.host, m_cfg.sshPort, err);
    if (m_sock < 0) return false;

    m_session = libssh2_session_init();
    if (!m_session) {
        err = QStringLiteral("libssh2_session_init failed");
        closeSession();
        return false;
    }
    libssh2_session_set_blocking(m_session, 1);
    libssh2_session_set_timeout(m_session, kBlockingTimeoutMs);

    if (libssh2_session_handshake(m_session, m_sock) != 0) {
        err = QStringLiteral("SSH handshake failed: %1").arg(lastError());
        closeSession();
        return false;
    }
    if (!verifyHostKey(err)) {
        closeSession();
        return false;
    }

    const QByteArray user = m_cfg.user.toUtf8();
    const QByteArray key  = QFile::encodeName(m_cfg.keyPath);
    if (libssh2_userauth_publickey_fromfile_ex(m_session, user.constData(),
                                               static_cast<unsigned>(user.size()),
                                               nullptr, key.constData(), "") != 0) {
        err = QStringLiteral("SSH key auth as '%1' failed: %2").arg(m_cfg.user, lastError());
        closeSession();
        return false;
    }

    libssh2_keepalive_config(m_session, 1, kKeepaliveSec);
    return true;
}

void SshWorker::closeSession()
{
    if (m_session) {
        libssh2_session_set_blocking(m_session, 1);
        libssh2_session_set_timeout(m_session, 2000);
        libssh2_session_disconnect(m_session, "bye");
        libssh2_session_free(m_session);
        m_session = nullptr;
    }
    if (m_sock >= 0) {
        ::close(m_sock);
        m_sock = -1;
    }
}

bool SshWorker::verifyHostKey(QString& err)
{
    size_t len = 0;
    int type = 0;
    const char* key = libssh2_session_hostkey(m_session, &len, &type);
    if (!key) {
        err = QStringLiteral("server sent no host key");
        return false;
    }

    const char* sha = libssh2_hostkey_hash(m_session, LIBSSH2_HOSTKEY_HASH_SHA256);
    const QString fp = sha
        ? QStringLiteral("SHA256:") +
              QString::fromLatin1(QByteArray(sha, 32).toBase64(QByteArray::OmitTrailingEquals))
        : QStringLiteral("(unknown)");

    LIBSSH2_KNOWNHOSTS* hosts = libssh2_knownhost_init(m_session);
    if (!hosts) {
        err = QStringLiteral("knownhost init failed");
        return false;
    }
    const QByteArray path = QFile::encodeName(m_cfg.knownHostsPath);
    if (QFileInfo::exists(m_cfg.knownHostsPath))
        libssh2_knownhost_readfile(hosts, path.constData(), LIBSSH2_KNOWNHOST_FILE_OPENSSH);

    const int mask = LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW | knownHostKeyBit(type);
    const QByteArray host = m_cfg.host.toUtf8();
    libssh2_knownhost* found = nullptr;
    int rc = libssh2_knownhost_checkp(hosts, host.constData(), m_cfg.sshPort, key, len, mask, &found);

    bool ok = false;
    switch (rc) {
    case LIBSSH2_KNOWNHOST_CHECK_MATCH:
        ok = true;
        break;
    case LIBSSH2_KNOWNHOST_CHECK_NOTFOUND: {
        // Trust on first use: remember this key for next time.
        const QByteArray entry = m_cfg.sshPort == 22
            ? host
            : QByteArray("[") + host + "]:" + QByteArray::number(m_cfg.sshPort);
        libssh2_knownhost_addc(hosts, entry.constData(), nullptr, key, len, nullptr, 0, mask, nullptr);
        QDir().mkpath(QFileInfo(m_cfg.knownHostsPath).absolutePath());
        if (libssh2_knownhost_writefile(hosts, path.constData(), LIBSSH2_KNOWNHOST_FILE_OPENSSH) != 0)
            qWarning("could not write %s", path.constData());
        ok = true;
        break;
    }
    case LIBSSH2_KNOWNHOST_CHECK_MISMATCH:
        err = QStringLiteral("HOST KEY CHANGED for %1 (now %2). Refusing to connect. "
                             "If this is expected, remove the entry from %3.")
                  .arg(m_cfg.host, fp, m_cfg.knownHostsPath);
        break;
    default:
        err = QStringLiteral("host key check failed for %1").arg(m_cfg.host);
        break;
    }
    libssh2_knownhost_free(hosts);
    return ok;
}

bool SshWorker::waitSocket(int timeoutMs)
{
    fd_set rfds, wfds;
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    int dir = libssh2_session_block_directions(m_session);
    if (dir & LIBSSH2_SESSION_BLOCK_INBOUND)  FD_SET(m_sock, &rfds);
    if (dir & LIBSSH2_SESSION_BLOCK_OUTBOUND) FD_SET(m_sock, &wfds);
    if (!dir) FD_SET(m_sock, &rfds);
    timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    return select(m_sock + 1, &rfds, &wfds, nullptr, &tv) >= 0;
}

void SshWorker::request(const QString& tag, const QJsonObject& req)
{
    m_abort = false;
    if (!m_haveCfg) {
        emit failed(tag, QStringLiteral("not configured — connect first"));
        return;
    }

    // Open a fresh tunnel channel; reconnect the SSH session once if the
    // transport turned out to be dead. Nothing has been sent yet at this
    // point, so retrying cannot double-submit a prompt.
    LIBSSH2_CHANNEL* ch = nullptr;
    for (int attempt = 0; attempt < 2 && !ch; ++attempt) {
        if (!m_session) {
            QString err;
            if (!openSession(err)) {
                emit connectedChanged(false);
                emit failed(tag, err);
                return;
            }
            emit connectedChanged(true);
        }
        libssh2_session_set_blocking(m_session, 1);
        const QByteArray rhost = m_cfg.remoteHost.toUtf8();
        ch = libssh2_channel_direct_tcpip_ex(m_session, rhost.constData(), m_cfg.remotePort,
                                             "127.0.0.1", 0);
        if (!ch) {
            int rc = libssh2_session_last_errno(m_session);
            if (isTransportError(rc) && attempt == 0) {
                closeSession();
                continue;
            }
            QString why = lastError();
            if (rc == LIBSSH2_ERROR_CHANNEL_FAILURE)
                why = QStringLiteral("tunnel to %1:%2 refused — is the remclaude server running "
                                     "and bound there?").arg(m_cfg.remoteHost).arg(m_cfg.remotePort);
            if (isTransportError(rc)) {
                closeSession();
                emit connectedChanged(false);
            }
            emit failed(tag, why);
            return;
        }
    }

    const QByteArray line = QJsonDocument(req).toJson(QJsonDocument::Compact) + '\n';
    qsizetype off = 0;
    while (off < line.size()) {
        ssize_t n = libssh2_channel_write(ch, line.constData() + off,
                                          static_cast<size_t>(line.size() - off));
        if (n < 0) {
            QString e = lastError();
            libssh2_channel_free(ch);
            closeSession();
            emit connectedChanged(false);
            emit failed(tag, QStringLiteral("send failed: %1").arg(e));
            return;
        }
        off += n;
    }

    // Wait for one reply line. claude -p can take minutes, so there is no
    // deadline: poll non-blocking, keep the SSH session alive, honour abort().
    libssh2_session_set_blocking(m_session, 0);
    QByteArray buf;
    QString error;
    char tmp[4096];
    while (true) {
        int nl = buf.indexOf('\n');
        if (nl >= 0) {
            buf.truncate(nl);
            break;
        }
        if (m_abort) {
            error = QStringLiteral("cancelled");
            break;
        }
        ssize_t n = libssh2_channel_read(ch, tmp, sizeof(tmp));
        if (n > 0) {
            buf.append(tmp, static_cast<int>(n));
            continue;
        }
        if (n == LIBSSH2_ERROR_EAGAIN) {
            int next = 0;
            libssh2_keepalive_send(m_session, &next);
            waitSocket(kPollMs);
            continue;
        }
        if (n == 0 && libssh2_channel_eof(ch)) {
            error = QStringLiteral("server closed the connection without replying");
            break;
        }
        if (n < 0) {
            error = QStringLiteral("receive failed: %1").arg(lastError());
            break;
        }
    }
    libssh2_session_set_blocking(m_session, 1);

    const bool transportDead = !error.isEmpty() && error != QStringLiteral("cancelled") &&
                               !error.startsWith(QStringLiteral("server closed"));
    if (transportDead) {
        libssh2_channel_free(ch);
        closeSession();
        emit connectedChanged(false);
        emit failed(tag, error);
        return;
    }
    libssh2_channel_close(ch);
    libssh2_channel_free(ch);
    if (error == QStringLiteral("cancelled")) {
        // The channel was mid-read; the session may hold stale state. Start clean.
        closeSession();
        emit connectedChanged(false);
    }
    if (!error.isEmpty()) {
        emit failed(tag, error);
        return;
    }

    QJsonParseError pe{};
    QJsonDocument doc = QJsonDocument::fromJson(buf, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        emit failed(tag, QStringLiteral("bad response from server"));
        return;
    }
    emit reply(tag, doc.object());
}
