#include "clipboardUtils.h"

#include <QByteArray>
#include <QMimeData>
#include <QString>
#include <QtGlobal>
#include <cstring>

namespace {

// Windows Preferred DropEffect layout: 4-byte little-endian DWORD.
// DROPEFFECT_MOVE = 2 means "cut". We only ever look at this one bit —
// other flags (DROPEFFECT_COPY = 1, DROPEFFECT_LINK = 4) are treated as
// "not a cut" and fall through to the default copy semantics.
constexpr int     DropEffectSize = 4;
constexpr quint32 DropEffectMove = 2;

const QString kDropEffectMime = QStringLiteral("Preferred DropEffect");

}

void ClipboardUtils::writeCutFlag(QMimeData *mime)
{
    if (!mime) return;

    QByteArray effect(DropEffectSize, 0);
    effect[0] = char(DropEffectMove);
    mime->setData(kDropEffectMime, effect);
}

bool ClipboardUtils::hasCutFlag(const QMimeData *mime)
{
    if (!mime || !mime->hasFormat(kDropEffectMime))
        return false;

    const QByteArray data = mime->data(kDropEffectMime);
    if (data.size() < DropEffectSize)
        return false;

    // memcpy into aligned storage — direct cast would be UB on strict-alignment
    // platforms and tripping sanitizers on x86 too.
    quint32 effect = 0;
    std::memcpy(&effect, data.constData(), sizeof(effect));

    return (effect & DropEffectMove) != 0;
}
