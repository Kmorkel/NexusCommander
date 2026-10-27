#include <QDialogButtonBox>
#include <QDir>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>
#include "connectServerDialog.h"
#include "fileSystemUtils.h"

namespace {
constexpr int kDialogWidth  = 440;
constexpr int kDialogHeight = 340;

/// @brief Result of the background part of a server lookup.
struct ServerLookup {
    NetworkDiscovery::ServerState  state = NetworkDiscovery::ServerState::NotFound;
    NetworkDiscovery::ShareListing listing;
};

// Probe first, list only if the server exists — so "requires a login" is
// never claimed for a server that isn't there. Blocking, runs on the pool.
ServerLookup lookupServer(const QString &server)
{
    ServerLookup result;
    result.state = NetworkDiscovery::probeServer(server);
    if (result.state == NetworkDiscovery::ServerState::Reachable)
        result.listing = NetworkDiscovery::listShares(server);
    return result;
}
}

ConnectServerDialog::ConnectServerDialog(const QString &initialServer, const QIcon &shareIcon,
                                         QWidget *parent)
    : QDialog(parent)
    , m_shareIcon(shareIcon)
{
    setWindowTitle(tr("Connect to server"));
    resize(kDialogWidth, kDialogHeight);

    auto *layout   = new QVBoxLayout(this);
    auto *inputRow = new QHBoxLayout;
    m_serverEdit = new QLineEdit(initialServer.trimmed(), this);
    m_serverEdit->setPlaceholderText(tr(R"(\\raspberrypi  or  \\raspberrypi\share)"));
    m_listButton = new QPushButton(tr("List shares"), this);
    // Enter in the line edit lists the shares; opening needs a selection.
    m_listButton->setDefault(true);
    inputRow->addWidget(m_serverEdit, 1);
    inputRow->addWidget(m_listButton);
    layout->addLayout(inputRow);

    m_shareList = new QListWidget(this);
    layout->addWidget(m_shareList, 1);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setWordWrap(true);
    layout->addWidget(m_statusLabel);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Open | QDialogButtonBox::Cancel, this);
    m_openButton = buttons->button(QDialogButtonBox::Open);
    m_openButton->setAutoDefault(false);
    m_openButton->setEnabled(false);
    layout->addWidget(buttons);

    connect(m_listButton, &QPushButton::clicked, this, [this] { startLookup(); });
    // Typing a new server name makes Enter list shares again.
    connect(m_serverEdit, &QLineEdit::textEdited, this, [this] {
        m_listButton->setDefault(true);
        updateOpenButton();
    });
    connect(m_shareList, &QListWidget::currentItemChanged, this, &ConnectServerDialog::updateOpenButton);
    connect(m_shareList, &QListWidget::itemDoubleClicked,  this, &QDialog::accept);
    connect(buttons,     &QDialogButtonBox::accepted,      this, &QDialog::accept);
    connect(buttons,     &QDialogButtonBox::rejected,      this, &QDialog::reject);

    // Opened from the address bar with "\\server" — list right away.
    if (!m_serverEdit->text().isEmpty())
        startLookup();
}

QString ConnectServerDialog::selectedShare() const
{
    // A typed share path wins over the list (it is what the user just entered).
    const QString typed = m_serverEdit->text();
    if (isSharePath(typed))
        return typed.trimmed();

    const QListWidgetItem *item = m_shareList->currentItem();
    return item ? item->data(Qt::UserRole).toString() : QString();
}

QString ConnectServerDialog::serverName() const
{
    QString server = QDir::fromNativeSeparators(m_serverEdit->text().trimmed());
    while (server.startsWith('/'))
        server.remove(0, 1);
    return server.section('/', 0, 0);
}

void ConnectServerDialog::updateOpenButton()
{
    m_openButton->setEnabled(m_shareList->currentItem() || isSharePath(m_serverEdit->text()));
}

void ConnectServerDialog::startLookup(bool afterSignIn)
{
    const QString server = serverName();
    if (server.isEmpty())
        return;

    m_shareList->clear();
    m_listButton->setEnabled(false);
    m_statusLabel->setText(tr("Looking up %1…").arg(server));
    updateOpenButton();

    // Name lookup, port probe and NetShareEnum can each take seconds — never on the GUI thread.
    // Context this: the continuation is dropped if the dialog is already closed.
    QtConcurrent::run(&lookupServer, server)
        .then(this, [this, server, afterSignIn](const ServerLookup &lookup) {
            m_listButton->setEnabled(true);

            if (lookup.state != NetworkDiscovery::ServerState::Reachable)
                m_statusLabel->setText(NetworkDiscovery::describeServerState(lookup.state, server));
            else if (lookup.listing.loginRequired && !afterSignIn)
                signInAndRetry(server);
            else
                showShares(lookup.listing);
        });
}

void ConnectServerDialog::signInAndRetry(const QString &server)
{
    // The server exists and wants credentials — ask right away, like Explorer.
    m_statusLabel->setText(tr("%1 requires a login.").arg(server));
    // Returns once the dialog is closed; the connect runs on the pool.
    auto signedIn = NetworkDiscovery::signIn(this, QStringLiteral("//%1/IPC$").arg(server));
    m_listButton->setEnabled(false);
    m_statusLabel->setText(tr("Signing in to %1…").arg(server));

    signedIn.then(this, [this](const NetworkDiscovery::SignInOutcome &outcome) {
        m_listButton->setEnabled(true);
        switch (outcome.result) {
        case NetworkDiscovery::SignInResult::Connected: startLookup(/*afterSignIn=*/true);               return;
        case NetworkDiscovery::SignInResult::Cancelled: m_statusLabel->setText(tr("Sign-in cancelled.")); return;
        case NetworkDiscovery::SignInResult::Failed:    m_statusLabel->setText(outcome.error);           return;
        }
    });
}

void ConnectServerDialog::showShares(const NetworkDiscovery::ShareListing &listing)
{
    for (const QString &path : listing.uncPaths) {
        auto *item = new QListWidgetItem(m_shareIcon, QDir::toNativeSeparators(path), m_shareList);
        item->setData(Qt::UserRole, path);
    }

    if (m_shareList->count() > 0) {
        m_shareList->setCurrentRow(0);
        m_shareList->setFocus();
        // Enter now opens the selected share instead of listing again.
        m_openButton->setDefault(true);
        m_statusLabel->setText(tr("%n share(s) found.", nullptr, m_shareList->count()));
        emit sharesFound(listing.uncPaths);
    } else {
        m_statusLabel->setText(listing.error);
    }
    updateOpenButton();
}
