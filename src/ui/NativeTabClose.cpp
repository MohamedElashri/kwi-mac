// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "NativeTabClose.h"

#include <Kirigami/Platform/PlatformTheme>

#include <QApplication>
#include <QEvent>
#include <QIcon>
#include <QPainter>
#include <QPalette>
#include <QQuickWindow>
#include <QSGImageNode>
#include <QSizeF>
#include <QStyle>
#include <QStyleOption>
#include <QtQml/qqml.h>

namespace Kwi
{
NativeTabClose::NativeTabClose(QQuickItem *parent)
    : QQuickItem{parent}
{
    setFlag(ItemHasContents);
    connect(this, &NativeTabClose::hoveredChanged, this, &QQuickItem::polish);
    connect(this, &NativeTabClose::pressedChanged, this, &QQuickItem::polish);
    connect(this, &NativeTabClose::selectedChanged, this, &QQuickItem::polish);
    updateStyle();
}

void NativeTabClose::updateStyle()
{
    if (!m_theme) {
        m_theme = static_cast<Kirigami::Platform::PlatformTheme *>(qmlAttachedPropertiesObject<Kirigami::Platform::PlatformTheme>(this, true));
        connect(m_theme, &Kirigami::Platform::PlatformTheme::colorsChanged, this, &QQuickItem::polish);
        connect(m_theme, &Kirigami::Platform::PlatformTheme::colorGroupChanged, this, &QQuickItem::polish);
    }

    auto *style = QApplication::style();

    if (m_style != style) {
        disconnect(m_styleConnection);
        m_style = style;
        // Defer repainting until the replacement style is available.
        m_styleConnection = connect(style, &QObject::destroyed, this, &QQuickItem::polish);
    }

    QStyleOption option;
    option.palette = m_theme->palette();
#ifdef Q_OS_MACOS
    const int size{style->pixelMetric(QStyle::PM_SmallIconSize, &option)};
    setImplicitSize(size, size);
#else
    setImplicitSize(style->pixelMetric(QStyle::PM_TabCloseIndicatorWidth, &option), style->pixelMetric(QStyle::PM_TabCloseIndicatorHeight, &option));
#endif
}

bool NativeTabClose::event(QEvent *incomingEvent)
{
    if (incomingEvent->type() == QEvent::StyleAnimationUpdate) {
        incomingEvent->accept();
        polish();

        return true;
    }

    return QQuickItem::event(incomingEvent);
}

void NativeTabClose::itemChange(ItemChange change, const ItemChangeData &data)
{
    QQuickItem::itemChange(change, data);

    if (change == ItemSceneChange || change == ItemVisibleHasChanged || change == ItemEnabledHasChanged || change == ItemDevicePixelRatioHasChanged) {
        polish();
    }
}

void NativeTabClose::geometryChange(const QRectF &geometry, const QRectF &oldGeometry)
{
    QQuickItem::geometryChange(geometry, oldGeometry);

    if (geometry.size() != oldGeometry.size()) {
        polish();
    }
}

void NativeTabClose::updatePolish()
{
    updateStyle();

    if (!window() || !isVisible() || width() < 1 || height() < 1) {
        m_image = {};
        update();

        return;
    }

    const qreal scale{window()->effectiveDevicePixelRatio()};
    m_image = QImage{QSizeF{width() * scale, height() * scale}.toSize(), QImage::Format_ARGB32_Premultiplied};
    m_image.setDevicePixelRatio(scale);
    m_image.fill(Qt::transparent);

    QStyleOption option;
    option.rect = boundingRect().toRect();
    option.palette = m_theme->palette();
    option.palette.setCurrentColorGroup(isEnabled() ? static_cast<QPalette::ColorGroup>(m_theme->colorGroup()) : QPalette::Disabled);
    option.styleObject = this;
    option.state = QStyle::State_AutoRaise;
    option.state.setFlag(QStyle::State_Enabled, isEnabled());
    option.state.setFlag(QStyle::State_Active, m_theme->colorGroup() == Kirigami::Platform::PlatformTheme::Active);
    option.state.setFlag(QStyle::State_MouseOver, m_hovered);
    option.state.setFlag(QStyle::State_Raised, isEnabled() && m_hovered && !m_pressed);
    option.state.setFlag(QStyle::State_Sunken, m_pressed);
    option.state.setFlag(QStyle::State_Selected, m_selected);

    // Paint on the GUI thread; updatePaintNode() uploads the image on the render thread.
    QPainter painter{&m_image};
#ifdef Q_OS_MACOS
    // Match the desktop close indicator without QMacStyle's round title-bar
    // icon (its tab primitive also requires a QWidget that Quick cannot supply).
    painter.setRenderHint(QPainter::Antialiasing);
    if (m_hovered || m_pressed) {
        auto background = option.palette.color(QPalette::Highlight);
        background.setAlphaF(m_pressed ? 0.25 : 0.12);
        painter.setPen(Qt::NoPen);
        painter.setBrush(background);
        painter.drawRoundedRect(boundingRect(), 2, 2);
    }
    painter.setPen(QPen{option.palette.color(QPalette::WindowText), 1.5, Qt::SolidLine, Qt::RoundCap});
    const QPointF center{boundingRect().center()};
    const qreal radius{qMin(width(), height()) / 4};
    painter.drawLine(center + QPointF{-radius, -radius}, center + QPointF{radius, radius});
    painter.drawLine(center + QPointF{-radius, radius}, center + QPointF{radius, -radius});
#else
    m_style->drawPrimitive(QStyle::PE_IndicatorTabClose, &option, &painter);
#endif
    painter.end();
    update();
}

QSGNode *NativeTabClose::updatePaintNode(QSGNode *node, UpdatePaintNodeData *)
{
    if (m_image.isNull()) {
        delete node;

        return nullptr;
    }

    auto *imageNode = static_cast<QSGImageNode *>(node);

    if (!imageNode) {
        imageNode = window()->createImageNode();
        imageNode->setOwnsTexture(true);
    }

    imageNode->setTexture(window()->createTextureFromImage(m_image, QQuickWindow::TextureCanUseAtlas));
    imageNode->setRect(boundingRect());

    return imageNode;
}
}
