// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "BidiLayout.h"
#include <Frame.h>
#include <QString>

#include <QColor>
#include <QFont>
#include <QInputMethodEvent>
#include <QList>
#include <QRectF>
#include <QRegion>
#include <QSGNode>
#include <functional>
#include <memory>

class QQuickWindow;

namespace GhosttyQt::Quick
{
class TerminalNode final : public QSGNode
{
public:
    struct Options {
        QRectF rect;
        QRectF contentRect;
        QFont font;
        qreal devicePixelRatio{1};
        qreal logicalDpi{96};
        qreal backgroundOpacity{1};
        bool focused{false};
        bool cursorBlinkVisible{true};
        bool textBlinkVisible{true};
        QRegion searchMatches; // Viewport cell coordinates.
        QRegion hoveredLink; // Viewport cell coordinates.
        QColor searchForeground;
        QColor searchBackground;
        QVector<BidiRow> bidiRows;
        bool composedPreedit{false};
        QString preedit;
        QList<QInputMethodEvent::Attribute> preeditAttributes;
    };

    struct Statistics {
        quint64 terminalId{0};
        quint64 frameGeneration{0};
        quint64 shapedRuns{0};
        quint64 rasterizedGlyphs{0};
        quint64 textureUploadBytes{0};
        quint64 rebuiltRows{0};
        quint64 geometryBytes{0};
        int atlasPages{0};
        int cachedRuns{0};
        qsizetype atlasBytes{0};
        qint64 prepareNanoseconds{0};
        qint64 shapingNanoseconds{0};
        qint64 glyphBuildNanoseconds{0};
        qint64 textureFlushNanoseconds{0};
    };

    using StatisticsCallback = std::function<void(const Statistics &)>;
    // Called on the render thread after preparation; the callback must not access GUI objects.
    explicit TerminalNode(QQuickWindow *window, bool collectStatistics = false, StatisticsCallback prepared = {});
    ~TerminalNode() override;

    void synchronize(const Frame &frame, const Options &options);
    [[nodiscard]] Statistics statistics() const;
    void preprocess() override;

private:
    void prepare(const Frame &frame, const Options &options);

    class Private;
    std::unique_ptr<Private> d;
};
}
