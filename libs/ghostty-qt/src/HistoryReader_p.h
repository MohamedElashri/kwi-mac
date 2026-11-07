// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "HistoryReader.h"
#include "Terminal.h"
#include <QObject>
#include <QPointer>
#include <ghostty/vt.h>

namespace GhosttyQt
{
class HistoryReader::Private
{
public:
    QObject context;
    QPointer<Terminal> owner;
    GhosttyTerminal terminal{nullptr};
    GhosttyTrackedGridRef next{nullptr};
    GhosttyTrackedGridRef end{nullptr};
    GhosttyTerminalScreen screen{};
    uint16_t columns{0};
    bool resized{false};
    qsizetype pendingLines{0};
    qsizetype pendingSpaces{0};
    qsizetype outputLines{0};
    qsizetype outputSpaces{0};
    QByteArray outputText;
    qsizetype outputOffset{0};
    bool finished{false};

    bool initialize();
    void drain(QByteArray &output);
    [[nodiscard]] bool formatRow(GhosttyGridRef reference);
    [[nodiscard]] bool drained() const;
};
}
