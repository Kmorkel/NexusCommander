#include <QDir>
#include <QFuture>
#include <QHash>
#include <QHostInfo>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QTcpSocket>
#include <QTimer>
#include <QUdpSocket>
#include <QUuid>
#include <QWidget>
#include <QXmlStreamReader>
#include <QtConcurrent/QtConcurrentRun>
#include <QtEndian>
#include "networkDiscovery.h"

#ifdef Q_OS_WIN
#include <memory>
#include <string>
#include <windows.h>
#include <winnetwk.h>
#include <wincred.h>
#include <lm.h>
#endif

namespace {
// mDNS multicast group and port (RFC 6762).
const QHostAddress    kMdnsGroup(QStringLiteral("224.0.0.251"));
constexpr quint16     kMdnsPort         = 5353;
// WS-Discovery multicast group and port.
const QHostAddress    kWsdGroup(QStringLiteral("239.255.255.250"));
constexpr quint16     kWsdPort          = 3702;
// Re-send once — a single multicast packet is easily lost on Wi-Fi.
constexpr int         kResendDelayMs    = 1000;
// Windows Firewall lets unicast replies to a multicast query through for ~3 s.
constexpr int         kListenDurationMs = 3000;
// SMB port probe — a LAN host answers within milliseconds; this only bounds a dead one.
constexpr quint16     kSmbPort          = 445;
constexpr int         kSmbProbeTimeoutMs = 3000;

// A records are not needed — the datagram's sender address is the host's IP.
constexpr quint16     kTypePtr = 12;
constexpr quint16     kTypeSrv = 33;
const QString         kSmbService = QStringLiteral("_smb._tcp.local");

// One-shot PTR query for _smb._tcp.local. Sent from an ephemeral port, so
// responders answer by unicast directly to us ("legacy unicast", RFC 6762 §6.7)
// and we never have to bind 5353, which Windows' own mDNS responder holds.
QByteArray buildMdnsQuery()
{
    QByteArray query(12, '\0');
    query[5] = 1;                               // QDCOUNT = 1 (big-endian)
    for (const char *label : {"_smb", "_tcp", "local"}) {
        query += char(qstrlen(label));
        query += label;
    }
    query += '\0';
    query += QByteArray("\x00\x0c\x00\x01", 4); // QTYPE = PTR, QCLASS = IN
    return query;
}

// WS-Discovery Probe for computers — same types Windows' own probe uses.
// ProbeMatches come back by unicast to the sending port. The "wsdp" prefix
// must stay literal: wsdd (Synology / QNAP / Samba) matches the type string.
QByteArray buildWsdProbe()
{
    const QString messageId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    return QStringLiteral(
        R"(<?xml version="1.0" encoding="utf-8"?>)"
        R"(<soap:Envelope xmlns:soap="http://www.w3.org/2003/05/soap-envelope")"
        R"( xmlns:wsa="http://schemas.xmlsoap.org/ws/2004/08/addressing")"
        R"( xmlns:wsd="http://schemas.xmlsoap.org/ws/2005/04/discovery")"
        R"( xmlns:wsdp="http://schemas.xmlsoap.org/ws/2006/02/devprof")"
        R"( xmlns:pub="http://schemas.microsoft.com/windows/pub/2005/07">)"
        R"(<soap:Header>)"
        R"(<wsa:To>urn:schemas-xmlsoap-org:ws:2005:04:discovery</wsa:To>)"
        R"(<wsa:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe</wsa:Action>)"
        R"(<wsa:MessageID>urn:uuid:%1</wsa:MessageID>)"
        R"(</soap:Header>)"
        R"(<soap:Body><wsd:Probe><wsd:Types>wsdp:Device pub:Computer</wsd:Types></wsd:Probe></soap:Body>)"
        R"(</soap:Envelope>)").arg(messageId).toUtf8();
}

// WS-Discovery answers are XML, mDNS answers binary DNS (first byte = ID 0x00).
bool looksLikeXml(const QByteArray &data)
{
    QByteArrayView view(data);
    if (view.startsWith("\xEF\xBB\xBF"))        // UTF-8 BOM
        view = view.sliced(3);
    return view.trimmed().startsWith('<');
}

bool readUInt16(const QByteArray &data, int offset, quint16 &value)
{
    if (offset < 0 || offset + 2 > data.size())
        return false;
    value = qFromBigEndian<quint16>(data.constData() + offset);
    return true;
}

// Read a DNS name at @p offset, following compression pointers.
// @p offset is advanced past the name as it appears in the record.
bool readName(const QByteArray &data, int &offset, QString &name)
{
    constexpr int kMaxPointerJumps = 16;   // guards against pointer loops in malformed packets
    QStringList labels;
    int  pos    = offset;
    int  jumps  = 0;
    bool jumped = false;

    while (true) {
        if (pos >= data.size())
            return false;

        const quint8 length = quint8(data[pos]);
        if (length == 0) {
            ++pos;
            break;
        }

        if ((length & 0xC0) == 0xC0) {
            if (pos + 1 >= data.size() || ++jumps > kMaxPointerJumps)
                return false;
            if (!jumped)
                offset = pos + 2;
            jumped = true;
            pos = ((length & 0x3F) << 8) | quint8(data[pos + 1]);
            continue;
        }

        if (pos + 1 + length > data.size())
            return false;
        labels << QString::fromUtf8(data.constData() + pos + 1, length);
        pos += 1 + length;
    }

    if (!jumped)
        offset = pos;
    name = labels.join('.');
    return true;
}

#ifdef Q_OS_WIN
// Localized system message for a Win32 error. NERR_* codes (2100+) are not in
// the system table — they fall back to the number.
QString windowsErrorText(DWORD code)
{
    wchar_t *buffer = nullptr;
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
                                            | FORMAT_MESSAGE_IGNORE_INSERTS,
                                        nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    const QString text = length > 0
                             ? QString::fromWCharArray(buffer, int(length)).trimmed()
                             : QObject::tr("Error %1").arg(code);
    LocalFree(buffer);
    return text;
}

// What the sign-in dialog returned. Shared between the GUI thread and the
// connect on the pool; the password is wiped when the last owner lets go.
struct Credentials {
    wchar_t user[CREDUI_MAX_USERNAME_LENGTH + 1]        = {};
    wchar_t domain[CREDUI_MAX_DOMAIN_TARGET_LENGTH + 1] = {};
    wchar_t password[CREDUI_MAX_PASSWORD_LENGTH + 1]    = {};

    ~Credentials() { SecureZeroMemory(password, sizeof(password)); }
};
#endif

// Shares of one discovered host. The name is tried first so paths match what
// Explorer shows; the responder's IP is the fallback when the name does not
// resolve. Blocking — runs on the thread pool.
QStringList lookupShares(QString hostName, const QString &address)
{
    // WS-Discovery answers carry no name — ask Windows (DNS / NetBIOS / LLMNR).
    // QHostInfo returns the address itself when the reverse lookup fails.
    if (hostName.isEmpty()) {
        const QString resolved = QHostInfo::fromName(address).hostName();
        if (resolved != address)
            hostName = resolved;
    }

    if (!hostName.isEmpty()) {
        const QStringList paths = NetworkDiscovery::listShares(hostName).uncPaths;
        if (!paths.isEmpty() || address.isEmpty())
            return paths;
    }
    return NetworkDiscovery::listShares(address).uncPaths;
}
}

NetworkDiscovery::NetworkDiscovery(QObject *parent)
    : QObject(parent)
{}

NetworkDiscovery::ShareListing NetworkDiscovery::listShares(const QString &server)
{
    ShareListing listing;
#ifdef Q_OS_WIN
    // STYPE_MASK is missing from MinGW's lmshare.h — low byte holds the share type.
    constexpr DWORD kShareTypeMask = 0xFF;

    std::wstring serverName = (QStringLiteral("\\\\") + server).toStdWString();
    NET_API_STATUS status;
    DWORD resumeHandle = 0;

    do {
        LPBYTE buffer = nullptr;
        DWORD  read   = 0;
        DWORD  total  = 0;
        status = NetShareEnum(serverName.data(), 1, &buffer, MAX_PREFERRED_LENGTH,
                              &read, &total, &resumeHandle);
        if (status != NERR_Success && status != ERROR_MORE_DATA)
            break;

        const auto *infos = reinterpret_cast<const SHARE_INFO_1 *>(buffer);
        for (DWORD i = 0; i < read; ++i) {
            const QString name = QString::fromWCharArray(infos[i].shi1_netname);
            // Disk shares only; '$' marks hidden/admin shares (C$, IPC$, print$).
            if ((infos[i].shi1_type & kShareTypeMask) == STYPE_DISKTREE
                && !(infos[i].shi1_type & STYPE_SPECIAL)
                && !name.endsWith('$'))
                listing.uncPaths << QStringLiteral("//%1/%2").arg(server, name);
        }
        NetApiBufferFree(buffer);
    } while (status == ERROR_MORE_DATA);

    // Not defined in MinGW's winerror.h: Windows 11 refuses Samba's guest fallback.
    constexpr NET_API_STATUS kErrorGuestLogonBlocked = 1272;

    switch (status) {
    case NERR_Success:
        if (listing.uncPaths.isEmpty())
            listing.error = QObject::tr("No shared folders found on %1.").arg(server);
        break;
    case ERROR_ACCESS_DENIED:
    case ERROR_LOGON_FAILURE:
    case kErrorGuestLogonBlocked:
    case ERROR_BAD_NETPATH:
        // 53 is also what a refused guest login looks like — callers decide
        // via probeServer() whether the server exists at all.
        listing.error         = QObject::tr("%1 requires a login.").arg(server);
        listing.loginRequired = true;
        break;
    case ERROR_BAD_NET_NAME:
        listing.error = QObject::tr("Server \"%1\" not found.").arg(server);
        break;
    default:
        listing.error = QObject::tr("Could not list shares on %1: %2")
                            .arg(server, windowsErrorText(status));
        break;
    }
#else
    listing.error = QObject::tr("Listing shares is only supported on Windows.");
    Q_UNUSED(server);
#endif
    return listing;
}

NetworkDiscovery::ServerState NetworkDiscovery::probeServer(const QString &server)
{
    // IP literals are returned as-is; names go through DNS / LLMNR / NetBIOS / mDNS.
    const QHostInfo info = QHostInfo::fromName(server);
    if (info.error() != QHostInfo::NoError || info.addresses().isEmpty())
        return ServerState::NotFound;

    // Blocking waitForConnected() is fine here — this runs on the thread pool.
    for (const QHostAddress &address : info.addresses()) {
        QTcpSocket socket;
        socket.connectToHost(address, kSmbPort);
        if (socket.waitForConnected(kSmbProbeTimeoutMs))
            return ServerState::Reachable;
    }
    return ServerState::Unreachable;
}

QString NetworkDiscovery::describeServerState(ServerState state, const QString &server)
{
    switch (state) {
    case ServerState::NotFound:
        return tr("Server \"%1\" not found.").arg(server);
    case ServerState::Unreachable:
        return tr("%1 does not answer on the file-sharing port (SMB).").arg(server);
    case ServerState::Reachable:
        break;
    }
    return {};
}

QFuture<NetworkDiscovery::SignInOutcome> NetworkDiscovery::signIn(QWidget *parent, const QString &uncPath)
{
#ifdef Q_OS_WIN
    const QString      nativePath = QDir::toNativeSeparators(uncPath);
    const std::wstring caption    = QObject::tr("Connect to network share").toStdWString();
    const std::wstring message    = QObject::tr("Enter your credentials for %1.").arg(nativePath).toStdWString();

    CREDUI_INFOW info   = {};
    info.cbSize         = sizeof(info);
    info.hwndParent     = parent ? reinterpret_cast<HWND>(parent->window()->winId()) : nullptr;
    info.pszCaptionText = caption.c_str();
    info.pszMessageText = message.c_str();

    // Only the dialog runs here — it has its own message loop. Not
    // WNetAddConnection3(CONNECT_INTERACTIVE): that also connects on the
    // calling thread after OK, and the GUI froze ("Not Responding") meanwhile.
    // CREDUIWIN_GENERIC: plain user name + password, no "Remember" checkbox.
    ULONG  authPackage = 0;
    LPVOID authBuffer  = nullptr;
    ULONG  authSize    = 0;
    const DWORD status = CredUIPromptForWindowsCredentialsW(&info, 0, &authPackage, nullptr, 0,
                                                            &authBuffer, &authSize, nullptr,
                                                            CREDUIWIN_GENERIC);
    if (status == ERROR_CANCELLED)
        return QtFuture::makeReadyValueFuture(SignInOutcome{SignInResult::Cancelled, {}});
    if (status != NO_ERROR)
        return QtFuture::makeReadyValueFuture(SignInOutcome{SignInResult::Failed, windowsErrorText(status)});

    auto  credentials    = std::make_shared<Credentials>();
    DWORD userLength     = DWORD(std::size(credentials->user));
    DWORD domainLength   = DWORD(std::size(credentials->domain));
    DWORD passwordLength = DWORD(std::size(credentials->password));
    const BOOL unpacked  = CredUnPackAuthenticationBufferW(0, authBuffer, authSize,
                                                           credentials->user, &userLength,
                                                           credentials->domain, &domainLength,
                                                           credentials->password, &passwordLength);
    const DWORD unpackError = unpacked ? NO_ERROR : GetLastError();
    SecureZeroMemory(authBuffer, authSize);
    CoTaskMemFree(authBuffer);
    if (!unpacked)
        return QtFuture::makeReadyValueFuture(SignInOutcome{SignInResult::Failed, windowsErrorText(unpackError)});

    std::wstring userName = credentials->user;
    if (credentials->domain[0] != L'\0')
        userName = std::wstring(credentials->domain) + L'\\' + userName;

    return QtConcurrent::run([credentials, userName, remoteName = nativePath.toStdWString()]() mutable {
        NETRESOURCEW resource = {};
        resource.dwType       = RESOURCETYPE_ANY;   // IPC$ is not a disk share
        resource.lpRemoteName = remoteName.data();
        // lpLocalName stays null → no drive letter. No CONNECT_UPDATE_PROFILE →
        // not restored at logon. The connection belongs to the logon session,
        // so the GUI thread uses it too.
        const DWORD result = WNetAddConnection2W(&resource, credentials->password, userName.c_str(), 0);
        return result == NO_ERROR ? SignInOutcome{SignInResult::Connected, {}}
                                  : SignInOutcome{SignInResult::Failed, windowsErrorText(result)};
    });
#else
    Q_UNUSED(parent);
    Q_UNUSED(uncPath);
    return QtFuture::makeReadyValueFuture(
        SignInOutcome{SignInResult::Failed, QObject::tr("Signing in is only supported on Windows.")});
#endif
}

void NetworkDiscovery::start()
{
    if (m_running)
        return;

    m_running   = true;
    m_listening = true;
    m_seenAddresses.clear();
    m_ownAddresses = QNetworkInterface::allAddresses();
    emit runningChanged(true);

    openSockets();
    sendQueries();
    QTimer::singleShot(kResendDelayMs, this, &NetworkDiscovery::sendQueries);

    // start() is a no-op while running, so this timer can never end a later search.
    QTimer::singleShot(kListenDurationMs, this, [this] {
        closeSockets();
        m_listening = false;
        finishIfDone();
    });
}

void NetworkDiscovery::openSockets()
{
    // One socket per IPv4 address — a single socket would only query the
    // interface of the default route (wrong one with VPN / Hyper-V adapters).
    for (const QNetworkInterface &iface : QNetworkInterface::allInterfaces()) {
        const auto flags = iface.flags();
        if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::IsRunning)
            || !(flags & QNetworkInterface::CanMulticast) || (flags & QNetworkInterface::IsLoopBack))
            continue;

        for (const QNetworkAddressEntry &entry : iface.addressEntries()) {
            if (entry.ip().protocol() != QAbstractSocket::IPv4Protocol)
                continue;

            auto *socket = new QUdpSocket(this);
            if (!socket->bind(entry.ip(), 0)) {
                delete socket;
                continue;
            }
            socket->setMulticastInterface(iface);
            socket->setSocketOption(QAbstractSocket::MulticastTtlOption, 255);
            connect(socket, &QUdpSocket::readyRead, this, [this, socket] {
                readResponses(socket);
            });
            m_sockets << socket;
        }
    }
}

void NetworkDiscovery::closeSockets()
{
    for (QUdpSocket *socket : std::as_const(m_sockets))
        socket->deleteLater();
    m_sockets.clear();
}

void NetworkDiscovery::sendQueries()
{
    const QByteArray mdnsQuery = buildMdnsQuery();
    const QByteArray wsdProbe  = buildWsdProbe();
    for (QUdpSocket *socket : std::as_const(m_sockets)) {
        socket->writeDatagram(mdnsQuery, kMdnsGroup, kMdnsPort);
        socket->writeDatagram(wsdProbe,  kWsdGroup,  kWsdPort);
    }
}

void NetworkDiscovery::readResponses(QUdpSocket *socket)
{
    while (socket->hasPendingDatagrams()) {
        const QNetworkDatagram datagram = socket->receiveDatagram();
        // Both protocols answer on the same socket — tell them apart by content,
        // not by source port (wsdd may reply from a different port than 3702).
        if (looksLikeXml(datagram.data()))
            handleWsdResponse(datagram.data(), datagram.senderAddress());
        else
            handleMdnsResponse(datagram.data(), datagram.senderAddress());
    }
}

void NetworkDiscovery::handleMdnsResponse(const QByteArray &datagram, const QHostAddress &sender)
{
    quint16 flags = 0, questions = 0, answers = 0, authorities = 0, additionals = 0;
    if (!readUInt16(datagram, 2, flags) || !(flags & 0x8000))    // QR bit: response
        return;
    readUInt16(datagram, 4,  questions);
    readUInt16(datagram, 6,  answers);
    readUInt16(datagram, 8,  authorities);
    readUInt16(datagram, 10, additionals);

    int offset = 12;
    for (int i = 0; i < questions; ++i) {
        QString ignored;
        if (!readName(datagram, offset, ignored))
            return;
        offset += 4;                                    // QTYPE + QCLASS
    }

    // PTR "_smb._tcp.local" → instance; SRV instance → host name ("raspberrypi.local").
    QStringList             instances;
    QHash<QString, QString> srvTargets;

    const int records = answers + authorities + additionals;
    for (int i = 0; i < records; ++i) {
        QString owner;
        if (!readName(datagram, offset, owner))
            return;

        quint16 type = 0, dataLength = 0;
        if (!readUInt16(datagram, offset, type) || !readUInt16(datagram, offset + 8, dataLength))
            return;
        offset += 10;                                   // TYPE, CLASS, TTL, RDLENGTH
        const int dataStart = offset;
        if (dataStart + dataLength > datagram.size())
            return;

        if (type == kTypePtr && owner.compare(kSmbService, Qt::CaseInsensitive) == 0) {
            int pos = dataStart;
            QString instance;
            if (readName(datagram, pos, instance))
                instances << instance;
        } else if (type == kTypeSrv && dataLength > 6) {
            int pos = dataStart + 6;                    // skip priority, weight, port
            QString target;
            if (readName(datagram, pos, target))
                srvTargets.insert(owner.toLower(), target);
        }

        offset = dataStart + dataLength;
    }

    for (const QString &instance : std::as_const(instances)) {
        QString host = srvTargets.value(instance.toLower());
        if (host.endsWith(QLatin1String(".local"), Qt::CaseInsensitive))
            host.chop(6);
        // No SRV in this packet: the instance label is usually the host name too.
        if (host.isEmpty())
            host = instance.section('.', 0, 0);
        addHost(host, sender);
    }
}

void NetworkDiscovery::handleWsdResponse(const QByteArray &datagram, const QHostAddress &sender)
{
    // Only ProbeMatches for computers count — printers / scanners also speak WSD.
    QXmlStreamReader xml(datagram);
    bool isProbeMatch = false;
    bool isComputer   = true;    // no <Types> in the match = assume a computer

    while (!xml.atEnd()) {
        if (xml.readNext() != QXmlStreamReader::StartElement)
            continue;
        if (xml.name() == u"ProbeMatch")
            isProbeMatch = true;
        else if (isProbeMatch && xml.name() == u"Types")
            isComputer = xml.readElementText().contains(QLatin1String("Computer"));
    }

    if (isProbeMatch && isComputer)
        addHost({}, sender);
}

void NetworkDiscovery::addHost(const QString &hostName, const QHostAddress &address)
{
    if (m_ownAddresses.contains(address))
        return;   // this PC answering our own WS-Discovery probe

    // Keyed by IP: the same NAS often answers both mDNS and WSD, and the
    // re-sent query triggers a second answer.
    const QString key = address.toString();
    if (key.isEmpty() || m_seenAddresses.contains(key))
        return;
    m_seenAddresses.insert(key);

    ++m_pendingLookups;
    QtConcurrent::run(&lookupShares, hostName, key)
        .then(this, [this](const QStringList &paths) {
            --m_pendingLookups;
            if (!paths.isEmpty())
                emit sharesFound(paths);
            finishIfDone();
        });
}

void NetworkDiscovery::finishIfDone()
{
    if (!m_running || m_listening || m_pendingLookups > 0)
        return;

    m_running = false;
    emit runningChanged(false);
}
