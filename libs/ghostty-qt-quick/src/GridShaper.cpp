// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "GridShaper_p.h"
#include "Symbols_p.h"

#include <QGlyphRun>
#include <QHash>
#include <QRectF>
#include <QTextCharFormat>
#include <QTextLayout>
#include <QTextOption>
#include <algorithm>
#include <array>
#include <utility>

namespace GhosttyQt::Quick
{
bool isFontIcon(const QString &text)
{
    if (text.isEmpty()) {
        return false;
    }

    const char32_t cp{text.size() > 1 && text[0].isHighSurrogate() && text[1].isLowSurrogate() ? QChar::surrogateToUcs4(text[0], text[1]) : text[0].unicode()};

    return QChar::category(cp) == QChar::Other_PrivateUse && !(cp >= 0xe0b0 && cp <= 0xe0d7) && !isProceduralSymbol(cp);
}

void GridShaper::setMetrics(const FontMetrics &metrics, qreal devicePixelRatio)
{
    m_metrics = metrics;
    m_dpr = devicePixelRatio;
    m_cache.clear();
}

int GridShaper::cacheSize() const
{
    return m_cache.size();
}

QVector<PositionedGlyph> GridShaper::shape(const QVector<Cell> &cells, int begin, int end, bool *cacheMiss)
{
    int iconColumns{isFontIcon(cells[begin].text) ? int(cells[begin].width) : 0};

    if (iconColumns == 1 && end < cells.size() && (begin == 0 || !isFontIcon(cells[begin - 1].text))) {
        const Cell &next = cells[end];

        if (next.width == 1 && !(next.flags & ImagePlaceholder) && (next.text.isEmpty() || next.text == QLatin1String(" ") || next.text == QChar(0x2002))) {
            iconColumns = 2;
        }
    }

    // LRO keeps Qt from reordering cells in Arabic/Hebrew runs while still shaping them.
    QString text(QChar(0x202d));
    QVector<int> columns{0};
    QString key;
    key += QChar(ushort(iconColumns));
    key += QChar(cells[begin].flags & (Bold | Italic));

    for (int column{begin}; column < end; ++column) {
        const Cell &cell = cells[column];

        if (cell.width == 0) {
            continue;
        }

        const QString contents{cell.text.isEmpty() ? QStringLiteral(" ") : cell.text};
        key += QChar(ushort(contents.size() & 0xffff));
        key += QChar(ushort(quint64(contents.size()) >> 16));
        key += QChar(ushort(cell.width));
        key += contents;
        text += contents;

        for (qsizetype unit{0}; unit < contents.size(); ++unit) {
            columns += column - begin;
        }
    }

    if (auto *cached = m_cache.object(key)) {
        if (cacheMiss) {
            *cacheMiss = false;
        }

        return *cached;
    }

    if (cacheMiss) {
        *cacheMiss = true;
    }

    const int textEnd{int(text.size())};
    columns += end - begin;
    text += QChar(0x202c);
    columns += end - begin;

    QFont font{m_metrics.pixelFont};

    if (cells[begin].flags & Bold) {
        constexpr std::array weights{QFont::Thin, QFont::Light, QFont::Normal, QFont::Bold, QFont::Black};
        const auto heavier = std::upper_bound(weights.cbegin(), weights.cend(), font.weight());
        const auto weight = heavier != weights.cend() ? *heavier : font.weight();

        if (weight != font.weight()) {
            // A named face can override weight/style matching. Keep it only for the unchanged base face.
            font.setStyleName({});
            font.setWeight(weight);
        }
    }

    if ((cells[begin].flags & Italic) && font.style() == QFont::StyleNormal) {
        font.setStyleName({});
        font.setStyle(QFont::StyleItalic);
    }

    QTextLayout layout{text, font};
    QTextOption option{Qt::AlignLeft};
    option.setWrapMode(QTextOption::NoWrap);
    option.setTextDirection(Qt::LeftToRight);
    option.setFlags(QTextOption::IncludeTrailingSpaces);
    layout.setTextOption(option);
    layout.beginLayout();
    QTextLine line{layout.createLine()};
    line.setLineWidth(1e9);
    layout.endLayout();
    const auto runs =
        layout.glyphRuns(1, textEnd - 1, QTextLayout::RetrieveGlyphIndexes | QTextLayout::RetrieveGlyphPositions | QTextLayout::RetrieveStringIndexes);
    QVector<int> boundaries{textEnd};

    for (const auto &run : runs) {
        const auto stringIndexes = run.stringIndexes();

        for (const auto index : stringIndexes) {
            boundaries += int(index);
        }
    }

    std::sort(boundaries.begin(), boundaries.end());
    boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
    QVector<PositionedGlyph> shaped;
    const qreal cellWidth{m_metrics.cellSize.width() * m_dpr};
    const qreal cellHeight{m_metrics.cellSize.height() * m_dpr};
    const qreal baseline{m_metrics.baseline * m_dpr};
    const QRawFont primary{QRawFont::fromFont(font)};
    struct FittedCluster {
        QRectF bounds;
        int columns{1};
    };
    QHash<int, FittedCluster> fittedClusters;

    for (const auto &run : runs) {
        const auto indexes = run.glyphIndexes();
        const auto positions = run.positions();
        const auto sources = run.stringIndexes();
        const QRawFont raw{run.rawFont()};
        const bool fallback{raw.familyName() != primary.familyName()};

        for (qsizetype i{0}; i < indexes.size(); ++i) {
            const int source{int(sources[i])};

            if (source < 1 || source >= textEnd) {
                continue;
            }

            const int next{*std::upper_bound(boundaries.cbegin(), boundaries.cend(), source)};
            const int firstColumn{columns[source]};
            const int span{qMax(1, columns[next] - firstColumn)};
            const qreal origin{line.cursorToX(source)};

            if (fallback || iconColumns) {
                fittedClusters.tryEmplace(firstColumn);
            }

            shaped.append(PositionedGlyph{raw,
                                          indexes[i],
                                          QPointF(firstColumn * cellWidth + positions[i].x() - origin, baseline + positions[i].y() - line.ascent()),
                                          1,
                                          firstColumn,
                                          span});
        }
    }

    if (!fittedClusters.isEmpty()) {
        // Include attached marks from other font runs in the cluster's ink bounds.
        for (const auto &glyph : std::as_const(shaped)) {
            const auto cluster = fittedClusters.find(glyph.firstColumn);

            if (cluster == fittedClusters.end()) {
                continue;
            }

            const QRectF bounds{glyph.font.boundingRect(glyph.index)};

            if (!bounds.isEmpty()) {
                cluster->bounds = cluster->bounds.united(bounds.translated(glyph.baseline - QPointF(glyph.firstColumn * cellWidth, 0)));
            }

            cluster->columns = qMax(cluster->columns, glyph.columnCount);
        }

        for (auto &glyph : shaped) {
            const auto cluster = fittedClusters.constFind(glyph.firstColumn);

            if (cluster == fittedClusters.cend() || cluster->bounds.isEmpty()) {
                continue;
            }

            const qreal width{cluster->columns * cellWidth};
            qreal scale{std::min({qreal(1), width / cluster->bounds.width(), cellHeight / cluster->bounds.height()})};

            if (iconColumns) {
                const qreal singleHeight{m_metrics.iconHeight * m_dpr};
                const qreal singleScale{qMin(cellWidth / cluster->bounds.width(), singleHeight / cluster->bounds.height())};
                const qreal availableHeight{iconColumns > 1 ? cellHeight : singleHeight};
                // Fit available space, but only enlarge small icons enough to fill one cell.
                scale = qMin(qMax(qreal(1), singleScale), qMin(iconColumns * cellWidth / cluster->bounds.width(), availableHeight / cluster->bounds.height()));
            }

            const QRectF ink{QPointF(cluster->bounds.left() * scale, baseline + (cluster->bounds.top() - baseline) * scale), cluster->bounds.size() * scale};
            const qreal horizontalSpace{qMax(qreal(0), width - ink.width())};
            const qreal verticalSpace{qMax(qreal(0), cellHeight - ink.height())};
            const qreal left{iconColumns ? qMax(qreal(0), (cellWidth - ink.width()) / 2)
                                         : (cluster->columns > 1 ? horizontalSpace / 2 : qBound(qreal(0), ink.left(), horizontalSpace))};
            const qreal top{iconColumns ? verticalSpace / 2 : qBound(qreal(0), ink.top(), verticalSpace)};
            const QPointF anchor{glyph.firstColumn * cellWidth, baseline};

            glyph.baseline = anchor + (glyph.baseline - anchor) * scale + QPointF(left - ink.left(), top - ink.top());
            glyph.scale = scale;
        }
    }

    m_cache.insert(key, new QVector<PositionedGlyph>(shaped), key.size());

    return shaped;
}
QVector<PositionedGlyph> GridShaper::shapeBidi(const BidiRow &row, bool *cacheMiss)
{
    QString key(1, QChar(0xffff)); // Distinct from ordinary grid-run keys.
    key += QChar(int(row.joinBefore));
    key += QChar(int(row.joinAfter));
    for (int x = 0; x < row.logicalCells.size(); ++x) {
        const auto &cell = row.logicalCells[x];
        key += QChar(ushort(cell.width));
        key += QChar(cell.flags & (Bold | Italic | ImagePlaceholder));
        key += QChar(ushort(row.levels[x]));
        key += QChar(ushort(row.logicalToVisual[x]));
        key += QChar(ushort(cell.text.size()));
        key += cell.text;
    }
    if (auto *cached = m_cache.object(key)) {
        if (cacheMiss)
            *cacheMiss = false;
        return *cached;
    }
    if (cacheMiss)
        *cacheMiss = true;

    struct Segment {
        int level;
        int left;
        int right;
        QString text;
        QVector<int> sources;
        QVector<QPair<int, int>> cells;
        QList<QTextLayout::FormatRange> formats;
    };
    QVector<Segment> segments;
    for (int x = 0; x < row.logicalCells.size(); ++x) {
        const auto &cell = row.logicalCells[x];
        if (!cell.width)
            continue;
        if (segments.isEmpty() || segments.last().level != row.levels[x]) {
            Segment segment{row.levels[x], row.logicalToVisual[x], row.logicalToVisual[x] + cell.width, {}, {}, {}, {}};
            segment.text += QChar((segment.level & 1) ? 0x202e : 0x202d);
            segment.sources += -1;
            segments += segment;
        }
        auto &segment = segments.last();
        segment.left = qMin(segment.left, row.logicalToVisual[x]);
        segment.right = qMax(segment.right, row.logicalToVisual[x] + cell.width);
        const int start = segment.text.size();
        if (x == 0 && row.joinBefore) {
            segment.text += QChar(0x200d);
            segment.sources += x;
        }
        const bool separate = (cell.flags & ImagePlaceholder) || isFontIcon(cell.text) || (cell.text.size() == 1 && isProceduralSymbol(cell.text[0].unicode()));
        const QString value = separate || cell.text.isEmpty() ? QStringLiteral(" ") : cell.text;
        int representative = -1;
        for (const QChar c : value) {
            const auto d = c.direction();
            // ICU already resolved direction controls; retain joining controls and marks.
            if (c == QChar(0x061c) || c == QChar(0x200e) || c == QChar(0x200f) || d == QChar::DirLRE || d == QChar::DirRLE || d == QChar::DirLRO
                || d == QChar::DirRLO || d == QChar::DirPDF || d == QChar::DirLRI || d == QChar::DirRLI || d == QChar::DirFSI || d == QChar::DirPDI)
                continue;
            if (representative < 0 && c != QChar(0x200c) && c != QChar(0x200d) && c.category() != QChar::Mark_NonSpacing)
                representative = segment.text.size();
            segment.text += c;
            segment.sources += x;
        }
        segment.cells += qMakePair(x, representative < 0 ? start : representative);
        QTextCharFormat format;
        if (cell.flags & Bold) {
            constexpr std::array weights{QFont::Thin, QFont::Light, QFont::Normal, QFont::Bold, QFont::Black};
            const auto heavier = std::upper_bound(weights.cbegin(), weights.cend(), m_metrics.pixelFont.weight());
            format.setFontStyleName(QString());
            format.setFontWeight(heavier != weights.cend() ? *heavier : m_metrics.pixelFont.weight());
        }
        if ((cell.flags & Italic) && m_metrics.pixelFont.style() == QFont::StyleNormal) {
            format.setFontStyleName(QString());
            format.setFontItalic(true);
        }
        if (!segment.formats.isEmpty() && segment.formats.last().format == format)
            segment.formats.last().length += int(segment.text.size()) - start;
        else
            segment.formats += QTextLayout::FormatRange{start, int(segment.text.size()) - start, format};
    }
    QVector<PositionedGlyph> result;
    const qreal baseline = m_metrics.baseline * m_dpr;
    for (int runIndex = 0; runIndex < segments.size(); ++runIndex) {
        auto &segment = segments[runIndex];
        if (runIndex + 1 == segments.size() && row.joinAfter) {
            segment.text += QChar(0x200d);
            segment.sources += segment.cells.last().first;
            ++segment.formats.last().length;
        }
        segment.text += QChar(0x202c);
        segment.sources += -1;
        QTextLayout layout(segment.text, m_metrics.pixelFont);
        const bool rtl = segment.level & 1;
        QTextOption option(Qt::AlignLeft);
        // Shape resolved runs independently. Matching the paragraph direction
        // prevents Qt from resetting run-edge carets to an adjacent LTR run.
        option.setTextDirection(rtl ? Qt::RightToLeft : Qt::LeftToRight);
        option.setWrapMode(QTextOption::NoWrap);
        option.setFlags(QTextOption::IncludeTrailingSpaces);
        layout.setTextOption(option);
        layout.setFormats(segment.formats);
        layout.beginLayout();
        auto line = layout.createLine();
        if (!line.isValid()) {
            layout.endLayout();
            continue;
        }
        line.setLineWidth(1e9);
        layout.endLayout();
        QVector<GlyphSpan> spans;
        // CoreText can report a single string index for an entire Arabic run.
        // Character carets still expose cluster edges, including ligature carets.
        // A continuous piecewise fit aligns the ink and preserves cursive joins.
        for (int index = 0; index < segment.cells.size(); ++index) {
            const auto [column, offset] = segment.cells[index];
            const qreal leading = index == 0 ? (rtl ? line.horizontalAdvance() : 0) : line.cursorToX(offset);
            const qreal trailing = index + 1 < segment.cells.size() ? line.cursorToX(segment.cells[index + 1].second) : (rtl ? 0 : line.horizontalAdvance());
            spans += GlyphSpan{qMin(leading, trailing), qMax(leading, trailing), row.logicalToVisual[column], qMax(1, int(row.logicalCells[column].width))};
        }
        std::sort(spans.begin(), spans.end(), [](const auto &a, const auto &b) {
            return a.firstColumn < b.firstColumn;
        });
        for (int index = 0; index < spans.size();) {
            auto &span = spans[index];
            if (span.sourceRight - span.sourceLeft > 0.01 || spans.size() == 1) {
                ++index;
                continue;
            }
            const int neighbor = index > 0 ? index - 1 : index + 1;
            auto &merged = spans[neighbor];
            merged.sourceLeft = qMin(merged.sourceLeft, span.sourceLeft);
            merged.sourceRight = qMax(merged.sourceRight, span.sourceRight);
            merged.firstColumn = qMin(merged.firstColumn, span.firstColumn);
            merged.columnCount += span.columnCount;
            spans.removeAt(index);
        }
        const auto runs = layout.glyphRuns(0,
                                           segment.text.size(),
                                           QTextLayout::RetrieveGlyphIndexes | QTextLayout::RetrieveGlyphPositions | QTextLayout::RetrieveStringIndexes);
        for (const auto &run : runs) {
            const auto indexes = run.glyphIndexes();
            const auto positions = run.positions();
            const auto strings = run.stringIndexes();
            for (int i = 0; i < indexes.size(); ++i) {
                const int source = strings[i];
                if (source < 0 || source >= segment.sources.size() || segment.sources[source] < 0)
                    continue;
                result += PositionedGlyph{run.rawFont(),
                                          indexes[i],
                                          QPointF(positions[i].x(), baseline + positions[i].y() - line.ascent()),
                                          1,
                                          segment.left,
                                          segment.right - segment.left,
                                          spans};
            }
        }
    }
    m_cache.insert(key, new QVector<PositionedGlyph>(result), qMax(1, int(key.size())));
    return result;
}

}
