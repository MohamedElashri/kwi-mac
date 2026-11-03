// SPDX-FileCopyrightText: 2026 Kwi contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <Frame.h>
#include <QCache>
#include <QRegion>

namespace GhosttyQt::Quick
{
// The VT grid stays logical. These permutations belong only to presentation.
struct BidiRow {
    QVector<int> logicalToVisual;
    QVector<int> visualToLogical;
    QVector<quint8> levels;
    QVector<Cell> logicalCells;
    bool joinBefore{false};
    bool joinAfter{false};
    bool operator==(const BidiRow &) const = default;
};

class BidiLayout
{
public:
    void update(const Frame &frame, bool enabled = true);
    [[nodiscard]] QPoint visualCell(QPoint logical) const;
    [[nodiscard]] QPoint logicalCell(QPoint visual) const;
    [[nodiscard]] QRegion visualRegion(const QRegion &logical) const;
    [[nodiscard]] Frame visualFrame(const Frame &logical) const;
    [[nodiscard]] const QVector<BidiRow> &rows() const
    {
        return m_rows;
    }
    [[nodiscard]] static bool needsLayout(const QString &text);

private:
    QVector<BidiRow> m_rows;
    QCache<QString, QVector<BidiRow>> m_cache{131072};
};
}
