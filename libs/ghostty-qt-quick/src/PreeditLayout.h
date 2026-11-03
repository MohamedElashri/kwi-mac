// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <Frame.h>
#include <QColor>
#include <QPoint>
#include <QString>
#include <QVector>

#include <QInputMethodEvent>
#include <QList>

namespace GhosttyQt::Quick
{
// Additional underline patterns used by Qt input-method text formats.
inline constexpr quint8 DashDotUnderline = 6;
inline constexpr quint8 DashDotDotUnderline = 7;

struct PreeditRow {
    QPoint cell;
    QVector<Cell> cells;
};

struct PreeditLayout {
    QVector<PreeditRow> rows;
    QPoint caret;
    bool caretVisible{true};
    QColor caretColor;
};

[[nodiscard]] Frame frameWithPreedit(const Frame &frame, const PreeditLayout &preedit);
[[nodiscard]] PreeditLayout layoutPreedit(const Frame &frame, const QString &text, const QList<QInputMethodEvent::Attribute> &attributes);
}
