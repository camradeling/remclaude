#include "RemoteSession.h"
#include "SshWorker.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>

RemoteSession::RemoteSession(QObject* parent) : QObject(parent)
{
    m_dataDir = resolveDataDir();
    QDir().mkpath(m_dataDir);

    QSettings s;
    m_host       = s.value("conn/host").toString();
    m_sshPort    = s.value("conn/sshPort", 22).toInt();
    m_user       = s.value("conn/user").toString();
    m_keyPath    = s.value("conn/keyPath", m_dataDir + "/id_ed25519").toString();
    m_remoteHost = s.value("conn/remoteHost", "127.0.0.1").toString();
    m_remotePort = s.value("conn/remotePort", 8765).toInt();
    m_tokenPath  = s.value("conn/tokenPath", m_dataDir + "/token.txt").toString();

    m_worker = new SshWorker;
    m_worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_worker, &SshWorker::reply,            this, &RemoteSession::onReply);
    connect(m_worker, &SshWorker::failed,           this, &RemoteSession::onFailed);
    connect(m_worker, &SshWorker::connectedChanged, this, &RemoteSession::onConnectedChanged);
    m_thread.start();

    // Auto-connect on startup if the connection is already configured.
    if (!m_host.isEmpty() && !m_user.isEmpty() && QFileInfo::exists(m_keyPath))
        QMetaObject::invokeMethod(this, &RemoteSession::connectToServer, Qt::QueuedConnection);
}

RemoteSession::~RemoteSession()
{
    m_worker->abort();
    m_thread.quit();
    m_thread.wait();
}

QString RemoteSession::resolveDataDir()
{
#ifdef Q_OS_ANDROID
    // Prefer the app-specific *external* dir (/sdcard/Android/data/<pkg>/files):
    // reachable with `adb push`, no storage permission needed.
    const QStringList dirs = QStandardPaths::standardLocations(QStandardPaths::AppDataLocation);
    for (const QString& d : dirs)
        if (d.contains(QLatin1String("/Android/data/")))
            return d;
    return dirs.value(0);
#else
    // ~/.local/share/remclaude (AppDataLocation would add org/app twice).
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
           QStringLiteral("/remclaude");
#endif
}

// ── settings ─────────────────────────────────────────────────────────────────

void RemoteSession::saveSetting(const char* key, const QVariant& v)
{
    QSettings s;
    s.setValue(key, v);
    emit settingsChanged();
}

void RemoteSession::setHost(const QString& v)       { if (v != m_host)       { m_host = v.trimmed();       saveSetting("conn/host", m_host); } }
void RemoteSession::setSshPort(int v)               { if (v != m_sshPort)    { m_sshPort = v;              saveSetting("conn/sshPort", v); } }
void RemoteSession::setUser(const QString& v)       { if (v != m_user)       { m_user = v.trimmed();       saveSetting("conn/user", m_user); } }
void RemoteSession::setKeyPath(const QString& v)    { if (v != m_keyPath)    { m_keyPath = v.trimmed();    saveSetting("conn/keyPath", m_keyPath); } }
void RemoteSession::setRemoteHost(const QString& v) { if (v != m_remoteHost) { m_remoteHost = v.trimmed(); saveSetting("conn/remoteHost", m_remoteHost); } }
void RemoteSession::setRemotePort(int v)            { if (v != m_remotePort) { m_remotePort = v;           saveSetting("conn/remotePort", v); } }
void RemoteSession::setTokenPath(const QString& v)  { if (v != m_tokenPath)  { m_tokenPath = v.trimmed();  saveSetting("conn/tokenPath", m_tokenPath); } }

bool RemoteSession::fileExists(const QString& path) const
{
    return !path.isEmpty() && QFileInfo::exists(path);
}

// ── connection ───────────────────────────────────────────────────────────────

void RemoteSession::connectToServer()
{
    QFile tf(m_tokenPath);
    if (!tf.open(QIODevice::ReadOnly)) {
        emit errorOccurred(QStringLiteral("Cannot read token file: %1").arg(m_tokenPath));
        return;
    }
    m_token = QString::fromUtf8(tf.readLine()).trimmed();
    if (m_token.isEmpty()) {
        emit errorOccurred(QStringLiteral("Token file is empty: %1").arg(m_tokenPath));
        return;
    }

    SshConfig cfg;
    cfg.host           = m_host;
    cfg.sshPort        = m_sshPort;
    cfg.user           = m_user;
    cfg.keyPath        = m_keyPath;
    cfg.knownHostsPath = m_dataDir + "/known_hosts";
    cfg.remoteHost     = m_remoteHost;
    cfg.remotePort     = m_remotePort;

    setPending(+1);
    QMetaObject::invokeMethod(m_worker, [w = m_worker, cfg] { w->connectTo(cfg); },
                              Qt::QueuedConnection);
    // Result arrives as reply("connect") or failed("connect").
}

void RemoteSession::disconnectFromServer()
{
    m_worker->abort();
    QMetaObject::invokeMethod(m_worker, &SshWorker::disconnectFromHost, Qt::QueuedConnection);
}

void RemoteSession::onConnectedChanged(bool c)
{
    if (m_connected != c) {
        m_connected = c;
        emit connectedChanged();
    }
}

void RemoteSession::cancel()
{
    m_worker->abort();
}

// ── requests ─────────────────────────────────────────────────────────────────

void RemoteSession::send(const QString& tag, QJsonObject req)
{
    if (m_token.isEmpty()) {
        emit errorOccurred(QStringLiteral("Not connected — open ⚙ Settings and press Connect"));
        return;
    }
    req["token"] = m_token;
    setPending(+1);
    QMetaObject::invokeMethod(m_worker, [w = m_worker, tag, req] { w->request(tag, req); },
                              Qt::QueuedConnection);
}

void RemoteSession::refreshSessions()
{
    send(QStringLiteral("list"), {{"cmd", "list"}});
}

void RemoteSession::createSession(const QString& name)
{
    const QString n = name.trimmed();
    if (n.isEmpty()) return;
    send(QStringLiteral("create:") + n, {{"cmd", "create"}, {"name", n}});
}

void RemoteSession::deleteSession(const QString& name)
{
    if (name.isEmpty()) return;
    send(QStringLiteral("delete:") + name, {{"cmd", "delete"}, {"name", name}});
}

void RemoteSession::selectSession(const QString& name)
{
    setCurrent(name);
}

void RemoteSession::sendMessage(const QString& text)
{
    const QString t = text.trimmed();
    if (t.isEmpty() || m_current.isEmpty() || busy()) return;
    push(QStringLiteral("user"), t);
    send(QStringLiteral("prompt:") + m_current,
         {{"cmd", "prompt"}, {"name", m_current}, {"text", t}});
}

void RemoteSession::onReply(const QString& tag, const QJsonObject& resp)
{
    setPending(-1);
    const bool ok = resp.value("ok").toBool();
    if (!ok) {
        emit errorOccurred(resp.value("error").toString(QStringLiteral("server error")));
        return;
    }

    if (tag == QLatin1String("connect")) {
        refreshSessions();
    } else if (tag == QLatin1String("list")) {
        m_sessions = resp.value("sessions").toArray().toVariantList();
        emit sessionsChanged();
        // Restore the last used session if it still exists.
        if (m_current.isEmpty()) {
            const QString last = QSettings().value("ui/lastSession").toString();
            for (const QVariant& v : m_sessions)
                if (v.toMap().value("name").toString() == last) {
                    setCurrent(last);
                    break;
                }
        }
    } else if (tag.startsWith(QLatin1String("create:"))) {
        setCurrent(resp.value("name").toString(tag.mid(7)));
        refreshSessions();
    } else if (tag.startsWith(QLatin1String("delete:"))) {
        const QString name = tag.mid(7);
        QFile::remove(historyPath(name));
        if (name == m_current) setCurrent(QString());
        refreshSessions();
    } else if (tag.startsWith(QLatin1String("prompt:"))) {
        const QString session = tag.mid(7);
        m_lastCost = resp.value("cost_usd").toDouble();
        emit lastCostChanged();
        const QString result = resp.value("result").toString();
        if (session == m_current) {
            push(QStringLiteral("assistant"), result);
        } else {
            // User switched sessions while waiting: append to that session's file.
            QFile f(historyPath(session));
            QJsonArray arr;
            if (f.open(QIODevice::ReadOnly)) arr = QJsonDocument::fromJson(f.readAll()).array();
            f.close();
            arr.append(QJsonObject{{"role", "assistant"}, {"text", result}});
            QSaveFile out(historyPath(session));
            if (out.open(QIODevice::WriteOnly)) {
                out.write(QJsonDocument(arr).toJson());
                out.commit();
            }
        }
        if (resp.value("is_error").toBool())
            emit errorOccurred(QStringLiteral("Claude reported an error for this prompt"));
    }
}

void RemoteSession::onFailed(const QString& tag, const QString& error)
{
    setPending(-1);
    if (tag == QLatin1String("connect")) {
        m_token.clear();
    }
    emit errorOccurred(error);
}

// ── local history ────────────────────────────────────────────────────────────

void RemoteSession::setCurrent(const QString& name)
{
    if (name == m_current) return;
    m_current = name;
    QSettings().setValue("ui/lastSession", name);
    emit currentSessionChanged();
    loadHistory();
}

QString RemoteSession::historyPath(const QString& session) const
{
    return m_dataDir + "/history/" +
           QString::fromLatin1(QUrl::toPercentEncoding(session)) + ".json";
}

void RemoteSession::loadHistory()
{
    m_messages.clear();
    if (!m_current.isEmpty()) {
        QFile f(historyPath(m_current));
        if (f.open(QIODevice::ReadOnly))
            m_messages = QJsonDocument::fromJson(f.readAll()).array().toVariantList();
    }
    emit messagesChanged();
}

void RemoteSession::saveHistory() const
{
    if (m_current.isEmpty()) return;
    QDir().mkpath(m_dataDir + "/history");
    QSaveFile f(historyPath(m_current));
    if (!f.open(QIODevice::WriteOnly)) return;
    f.write(QJsonDocument(QJsonArray::fromVariantList(m_messages)).toJson());
    f.commit();
}

void RemoteSession::clearHistory()
{
    m_messages.clear();
    if (!m_current.isEmpty()) QFile::remove(historyPath(m_current));
    emit messagesChanged();
}

void RemoteSession::push(const QString& role, const QString& text)
{
    QVariantMap msg;
    msg["role"] = role;
    msg["text"] = text;
    m_messages.append(msg);
    saveHistory();
    emit messagesChanged();
}

void RemoteSession::setPending(int delta)
{
    const bool was = busy();
    m_pending = qMax(0, m_pending + delta);
    if (busy() != was) emit busyChanged();
}
