#pragma once

#include <QJsonObject>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <atomic>

struct SshConfig {
    QString host;
    int     sshPort = 22;
    QString user;
    QString keyPath;           // private key (OpenSSH format)
    QString knownHostsPath;    // TOFU store
    QString remoteHost = QStringLiteral("127.0.0.1");
    int     remotePort = 8765; // remclaude server port, as seen from the SSH host
};
Q_DECLARE_METATYPE(SshConfig)

struct _LIBSSH2_SESSION;

// Owns one SSH session and runs on its own QThread. Every remclaude request
// gets a fresh direct-tcpip channel (the server closes idle TCP connections
// after read_timeout_sec), while the SSH session itself is kept alive with
// keepalives and re-established lazily if it drops.
//
// All slots block the worker thread; call them via queued connections only.
class SshWorker : public QObject
{
    Q_OBJECT
public:
    explicit SshWorker(QObject* parent = nullptr);
    ~SshWorker() override;

    // Thread-safe: makes a blocked request() give up at its next poll.
    void abort() { m_abort = true; }

public slots:
    void connectTo(const SshConfig& cfg);
    void request(const QString& tag, const QJsonObject& req);
    void disconnectFromHost();

signals:
    void connectedChanged(bool connected);
    void reply(const QString& tag, const QJsonObject& resp);
    void failed(const QString& tag, const QString& error);

private:
    bool    openSession(QString& err);
    void    closeSession();
    bool    verifyHostKey(QString& err);
    bool    waitSocket(int timeoutMs);
    QString lastError() const;

    SshConfig          m_cfg;
    bool               m_haveCfg = false;
    int                m_sock    = -1;
    _LIBSSH2_SESSION*  m_session = nullptr;
    std::atomic<bool>  m_abort{false};
};
