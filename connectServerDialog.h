#pragma once

#include <QDialog>
#include <QIcon>
#include "networkDiscovery.h"

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

/**
 * @brief "Connect to server" dialog: enter "\\server", list its shares in the
 * background, pick one to open.
 *
 * A lookup runs probeServer() (name resolves? port 445 answers?) and then
 * listShares() on QtConcurrent. Only a reachable server whose listing is refused
 * gets Windows' sign-in dialog — once, no prompt loop. A typed "\\server\share"
 * can be opened directly (the server may refuse to list its shares).
 */
class ConnectServerDialog : public QDialog
{
    Q_OBJECT

public:
    /// @p initialServer: typed into the address bar — listed right away if not empty.
    /// @p shareIcon is shown for every listed share.
    ConnectServerDialog(const QString &initialServer, const QIcon &shareIcon,
                        QWidget *parent = nullptr);

    /// @return The share to open after accept() — a typed share path wins over
    /// the list. Empty if there is none.
    QString selectedShare() const;

signals:
    /// @brief Shares listed for a server — MainWindow keeps them in the drive dropdown for the session.
    void sharesFound(const QStringList &uncPaths);

private:
    QLineEdit   *m_serverEdit  = nullptr;
    QPushButton *m_listButton  = nullptr;
    QListWidget *m_shareList   = nullptr;
    QLabel      *m_statusLabel = nullptr;
    QPushButton *m_openButton  = nullptr;
    QIcon        m_shareIcon;

    /// @return Host part of the input — accepts "\\host", "//host", "host" and "\\host\share\…".
    QString serverName() const;
    /// @brief Open needs a listed share or a typed "\\host\share".
    void updateOpenButton();
    /// @brief List the shares of the entered server off the GUI thread.
    /// @p afterSignIn: follows a successful sign-in — if the server still
    /// refuses, that is reported instead of prompting again.
    void startLookup(bool afterSignIn = false);
    /// @brief The server wants credentials — show Windows' sign-in dialog, then list again.
    void signInAndRetry(const QString &server);
    /// @brief Fill the list from @p listing, or show its error.
    void showShares(const NetworkDiscovery::ShareListing &listing);
};
