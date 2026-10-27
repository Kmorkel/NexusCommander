#pragma once

#include <QFuture>
#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QSet>
#include <QStringList>

class QUdpSocket;
class QWidget;

/**
 * @brief Finds SMB servers in the LAN (NAS, Raspberry Pi, …) and lists their shares.
 *
 * Two protocols run in parallel on every IPv4 interface, from the same
 * ephemeral-port sockets, and answers are collected for a few seconds:
 *  - mDNS query for "_smb._tcp.local" — Samba + Avahi, Synology / QNAP with Bonjour.
 *  - WS-Discovery probe — what Explorer's "Network" uses; Synology / QNAP /
 *    Samba with wsdd, Windows PCs. Answers carry no name, so the IP is
 *    reverse-resolved before the share listing.
 * Every new host gets its share list fetched via NetShareEnum on the thread
 * pool, so results appear one host at a time instead of after the slowest one.
 *
 * @par Threading
 * Sockets are event-driven on the GUI thread — nothing here blocks. Only
 * name lookup + share enumeration (which can take the SMB timeout) run on
 * QtConcurrent; their continuation comes back to this object's thread.
 */
class NetworkDiscovery : public QObject
{
    Q_OBJECT

public:
    /// @brief Result of listShares().
    struct ShareListing {
        QStringList uncPaths;              ///< "//server/share" for every visible disk share.
        QString     error;                 ///< Non-empty if the server could not be queried.
        bool        loginRequired = false; ///< Refused for lack of credentials — only meaningful
                                           ///< if probeServer() said Reachable (error 53 is ambiguous).
    };

    /// @brief Outcome of signIn().
    enum class SignInResult { Connected, Cancelled, Failed };

    /// @brief Result of signIn() — @c error is only set on Failed.
    struct SignInOutcome {
        SignInResult result = SignInResult::Failed;
        QString      error;
    };

    /// @brief Outcome of probeServer().
    enum class ServerState {
        NotFound,       ///< Name does not resolve.
        Unreachable,    ///< Resolves, but nothing answers on the SMB port.
        Reachable       ///< SMB port answers — any further refusal is about credentials.
    };

    explicit NetworkDiscovery(QObject *parent = nullptr);

    /// @brief Start a search. No-op while one is already running.
    void start();
    bool isRunning() const { return m_running; }

    /// @brief List the disk shares of @p server ("raspberrypi" or "192.168.1.20").
    /// Blocking — run off the GUI thread. Uses the current Windows credentials.
    static ShareListing listShares(const QString &server);

    /// @brief Check whether @p server exists and answers on the SMB port (445).
    ///
    /// Windows reports both "no such server" and "guest login refused" as error
    /// 53 — this tells them apart, so a sign-in is only offered for a real server.
    /// Blocking (name lookup + TCP connect with timeout) — run off the GUI thread.
    static ServerState probeServer(const QString &server);

    /// @return Error text for a server that is NotFound / Unreachable; empty for Reachable.
    static QString describeServerState(ServerState state, const QString &server);

    /// @brief Connect to @p uncPath ("//server/share" or "//server/IPC$") without a
    /// drive letter, after asking for credentials in the Windows sign-in dialog —
    /// the same fallback Explorer uses. Nothing is stored.
    ///
    /// Call on the GUI thread: the dialog is modal to @p parent and returns before
    /// the connect starts. The connect itself (SMB negotiation + login, ~10 s on a
    /// sleeping NAS) runs on QtConcurrent — chain a continuation with a context
    /// object. Cancelled / dialog errors come back as an already finished future.
    static QFuture<SignInOutcome> signIn(QWidget *parent, const QString &uncPath);

signals:
    /// @brief Search started / finished (all hosts answered or timed out).
    void runningChanged(bool running);
    /// @brief Disk shares of one host as UNC paths ("//raspberrypi/pi-ssd").
    void sharesFound(const QStringList &uncPaths);

private:
    QList<QUdpSocket *>  m_sockets;
    QSet<QString>        m_seenAddresses;      ///< Hosts of the current search (mDNS and WSD may both answer).
    QList<QHostAddress>  m_ownAddresses;       ///< This PC — Windows answers WS-Discovery itself.
    int                  m_pendingLookups = 0; ///< Share enumerations still running.
    bool                 m_running        = false;
    bool                 m_listening      = false;  ///< Sockets still open for answers.

    void openSockets();
    void closeSockets();
    void sendQueries();
    void readResponses(QUdpSocket *socket);
    void handleMdnsResponse(const QByteArray &datagram, const QHostAddress &sender);
    void handleWsdResponse(const QByteArray &datagram, const QHostAddress &sender);
    /// @brief Start the share lookup for a host. Empty @p hostName = resolve from @p address.
    void addHost(const QString &hostName, const QHostAddress &address);
    void finishIfDone();
};
