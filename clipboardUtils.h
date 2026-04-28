#pragma once

class QMimeData;

/**
 * @brief Windows "Preferred DropEffect" helpers for cut/copy clipboard detection.
 *
 * The Windows shell stores a raw 4-byte DWORD under the clipboard format
 * "Preferred DropEffect" to signal whether a selection was cut (Ctrl+X) or
 * copied (Ctrl+C). NexusCommander mirrors that convention so copy/paste
 * interoperates with Explorer and third-party shell clients.
 *
 * These helpers hide the raw byte layout and the DROPEFFECT_MOVE magic value.
 */
namespace ClipboardUtils {

/// @brief Mark mime data as "cut" by attaching Preferred DropEffect with MOVE flag.
void writeCutFlag(QMimeData *mime);

/// @brief Return true if mime carries a Preferred DropEffect with the MOVE bit set.
bool hasCutFlag(const QMimeData *mime);

}
