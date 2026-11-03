// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "BidiLayout.h"
#include "FontMetrics.h"

#include <Frame.h>

#include <QCache>
#include <QPointF>
#include <QRawFont>
#include <QString>
#include <QVector>

namespace GhosttyQt::Quick
{
[[nodiscard]] bool isFontIcon(const QString &text);

struct GlyphSpan {
    qreal sourceLeft{0};
    qreal sourceRight{0};
    int firstColumn{0};
    int columnCount{1};
};

struct PositionedGlyph {
    QRawFont font;
    quint32 index{0};
    QPointF baseline; // Physical pixels relative to the run's first cell.
    qreal scale{1};
    int firstColumn{0};
    int columnCount{1};
    QVector<GlyphSpan> spans{}; // Piecewise horizontal fit, shared by glyphs in a bidi run.
};

class GridShaper
{
public:
    void setMetrics(const FontMetrics &metrics, qreal devicePixelRatio);
    [[nodiscard]] QVector<PositionedGlyph> shape(const QVector<Cell> &cells, int begin, int end, bool *cacheMiss = nullptr);
    [[nodiscard]] QVector<PositionedGlyph> shapeBidi(const BidiRow &row, bool *cacheMiss = nullptr);
    [[nodiscard]] int cacheSize() const;

private:
    FontMetrics m_metrics;
    qreal m_dpr{1};
    QCache<QString, QVector<PositionedGlyph>> m_cache{32768}; // Cost in UTF-16 code units.
};
}
