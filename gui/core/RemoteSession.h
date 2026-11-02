#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QThread>
#include <QVariantList>

class SshWorker;

// QML-facing controller (context property "remote"). Mirrors the shape of
// confucius's TutorSession (messages/busy/errorOccurred) so the chat UI
// carries over, and drives a remclaude server through SshWorker.
class RemoteSession : public QObject
{
    Q_OBJECT
    // state
    Q_PROPERTY(bool         connected      READ connected      NOTIFY connectedChanged)
    Q_PROPERTY(bool         busy           READ busy           NOTIFY busyChanged)
    Q_PROPERTY(QVariantList messages       READ messages       NOTIFY messagesChanged)
    Q_PROPERTY(QVariantList sessions       READ sessions       NOTIFY sessionsChanged)
    Q_PROPERTY(QString      currentSession READ currentSession NOTIFY currentSessionChanged)
    Q_PROPERTY(double       lastCost       READ lastCost       NOTIFY lastCostChanged)
    Q_PROPERTY(QString      dataDir        READ dataDir        CONSTANT)
    // settings
    Q_PROPERTY(QString host       READ host       WRITE setHost       NOTIFY settingsChanged)
    Q_PROPERTY(int     sshPort    READ sshPort    WRITE setSshPort    NOTIFY settingsChanged)
    Q_PROPERTY(QString user       READ user       WRITE setUser       NOTIFY settingsChanged)
    Q_PROPERTY(QString keyPath    READ keyPath    WRITE setKeyPath    NOTIFY settingsChanged)
    Q_PROPERTY(QString remoteHost READ remoteHost WRITE setRemoteHost NOTIFY settingsChanged)
    Q_PROPERTY(int     remotePort READ remotePort WRITE setRemotePort NOTIFY settingsChanged)
    Q_PROPERTY(QString tokenPath  READ tokenPath  WRITE setTokenPath  NOTIFY settingsChanged)

public:
    explicit RemoteSession(QObject* parent = nullptr);
    ~RemoteSession() override;

    bool         connected()      const { return m_connected; }
    bool         busy()           const { return m_pending > 0; }
    QVariantList messages()       const { return m_messages; }
    QVariantList sessions()       const { return m_sessions; }
    QString      currentSession() const { return m_current; }
    double       lastCost()       const { return m_lastCost; }
    QString      dataDir()        const { return m_dataDir; }

    QString host()       const { return m_host; }
    int     sshPort()    const { return m_sshPort; }
    QString user()       const { return m_user; }
    QString keyPath()    const { return m_keyPath; }
    QString remoteHost() const { return m_remoteHost; }
    int     remotePort() const { return m_remotePort; }
    QString tokenPath()  const { return m_tokenPath; }
    void setHost(const QString& v);
    void setSshPort(int v);
    void setUser(const QString& v);
    void setKeyPath(const QString& v);
    void setRemoteHost(const QString& v);
    void setRemotePort(int v);
    void setTokenPath(const QString& v);

    Q_INVOKABLE bool fileExists(const QString& path) const;
    Q_INVOKABLE void connectToServer();
    Q_INVOKABLE void disconnectFromServer();
    Q_INVOKABLE void refreshSessions();
    Q_INVOKABLE void createSession(const QString& name);
    Q_INVOKABLE void deleteSession(const QString& name);
    Q_INVOKABLE void selectSession(const QString& name);
    Q_INVOKABLE void sendMessage(const QString& text);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void clearHistory();

signals:
    void connectedChanged();
    void busyChanged();
    void messagesChanged();
    void sessionsChanged();
    void currentSessionChanged();
    void lastCostChanged();
    void settingsChanged();
    void errorOccurred(const QString& msg);

private slots:
    void onReply(const QString& tag, const QJsonObject& resp);
    void onFailed(const QString& tag, const QString& error);
    void onConnectedChanged(bool c);

private:
    void    send(const QString& tag, QJsonObject req);
    void    push(const QString& role, const QString& text);
    void    setPending(int delta);
    void    setCurrent(const QString& name);
    void    loadHistory();
    void    saveHistory() const;
    QString historyPath(const QString& session) const;
    void    saveSetting(const char* key, const QVariant& v);
    static QString resolveDataDir();

    QThread    m_thread;
    SshWorker* m_worker = nullptr;

    bool         m_connected = false;
    int          m_pending   = 0;
    QVariantList m_messages;
    QVariantList m_sessions;
    QString      m_current;
    double       m_lastCost = 0.0;
    QString      m_token;
    QString      m_dataDir;

    QString m_host, m_user, m_keyPath, m_remoteHost, m_tokenPath;
    int     m_sshPort = 22, m_remotePort = 8765;
};
