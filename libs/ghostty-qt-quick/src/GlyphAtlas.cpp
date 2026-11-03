// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "GlyphAtlas_p.h"
#include "Symbols_p.h"

#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QQuickWindow>
#include <QtGui/private/qrawfont_p.h>
#include <cmath>
#include <numbers>
#include <numeric>
#include <utility>

namespace GhosttyQt::Quick
{
GlyphAtlas::GlyphAtlas(QQuickWindow *window, quint64 *rasterizedGlyphs, std::unique_ptr<GlyphAtlas> source)
    : m_window{window}
    , m_rasterized{rasterizedGlyphs}
    , m_source{std::move(source)}
{
}

GlyphImage GlyphAtlas::find(const Key &key)
{
    const auto it = m_glyphs.constFind(key);

    if (it == m_glyphs.cend()) {
        if (m_source) {
            const auto old = m_source->find(key);

            if (old.page) {
                return insert(key, old.page->image.copy(old.pixels), old.bearing, old.format, false);
            }
        }

        return {};
    }

    if (auto page = it->page.lock()) {
        page->lastUsed = m_frame;

        return {std::move(page), it->pixels, it->bearing, it->format};
    }

    m_glyphs.remove(key);

    return {};
}

GlyphImage GlyphAtlas::insert(const Key &key, const QImage &image, QPointF bearing, GlyphImage::Format format, bool rasterized)
{
    if (image.isNull()) {
        return {};
    }

    std::shared_ptr<AtlasPage> page;

    for (auto it = m_pages.rbegin(); it != m_pages.rend(); ++it) {
        const auto &candidate = *it;

        if (candidate->x + image.width() + 1 > candidate->image.width()) {
            candidate->y += candidate->rowHeight + 1;
            candidate->x = 1;
            candidate->rowHeight = 0;
        }

        if (candidate->x + image.width() + 1 <= candidate->image.width() && candidate->y + image.height() + 1 <= candidate->image.height()) {
            page = candidate;
            break;
        }
    }

    if (!page) {
        page = std::make_shared<AtlasPage>();

        // Start small for ASCII sessions, then grow pages to reduce CJK texture batches.
        const int pageSize{m_pages.empty() ? 256 : 512};
        const int size{qMax(pageSize, int(qNextPowerOfTwo(quint32(qMax(image.width(), image.height()) + 2))))};
        page->image = QImage(size, size, QImage::Format_ARGB32_Premultiplied);
        page->image.fill(Qt::transparent);
        m_pages.push_back(page);
    }

    const QRect rect(page->x, page->y, image.width(), image.height());
    QPainter painter{&page->image};
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.drawImage(rect.topLeft(), image);
    painter.end();

    page->x += image.width() + 1;
    page->rowHeight = qMax(page->rowHeight, image.height());
    page->dirty = true;
    page->lastUsed = m_frame;
    m_glyphs.insert(key, CachedGlyph{page, rect, bearing, format});

    if (m_rasterized) {
        *m_rasterized += rasterized;
    }

    return {std::move(page), rect, bearing, format};
}

GlyphImage GlyphAtlas::glyph(const QRawFont &font, quint32 index, bool allowSubpixel)
{
    if (!font.isValid()) {
        return {};
    }

    // Request LCD rendering only when enabled by the platform's font policy.
    auto *engine = QRawFontPrivate::get(font)->fontEngine;
    const bool subpixel{allowSubpixel && engine->glyphFormat == QFontEngine::Format_A32};
    const Key key{font, index, Kind::Font, subpixel};

    if (auto found = find(key); found.page) {
        return found;
    }

    // CoreText bounds queries can be expensive. Do this only on an atlas miss,
    // and reuse the result for grayscale bearings rather than querying twice.
    const QRectF bounds{font.boundingRect(index)};
    if (bounds.isEmpty()) {
        return {};
    }

    QImage source{font.alphaMapForGlyph(index, subpixel ? QRawFont::SubPixelAntialiasing : QRawFont::PixelAntialiasing)};
    QPointF bearing;

    if (subpixel) {
        // QRawFont::boundingRect can omit the extra bearings needed for LCD filtering.
        const auto bounds = engine->alphaMapBoundingBox(index, {}, {}, QFontEngine::Format_A32);
        bearing = QPointF(bounds.x.truncate(), bounds.y.truncate());
    } else {
        bearing = QPointF(std::floor(bounds.left()), std::floor(bounds.top()));
    }

    if (subpixel && source.format() == QImage::Format_RGB32) {
        // Opaque alpha prevents premultiplication from altering the RGB coverage channels.
        return insert(key, source.convertToFormat(QImage::Format_ARGB32_Premultiplied), bearing, GlyphImage::Format::Subpixel);
    }

    const bool colored = source.format() == QImage::Format_ARGB32_Premultiplied || source.format() == QImage::Format_ARGB32
        || source.format() == QImage::Format_RGBA8888_Premultiplied || source.format() == QImage::Format_RGBA8888;

    if (colored) {
        return insert(key, source.convertToFormat(QImage::Format_ARGB32_Premultiplied), bearing, GlyphImage::Format::Color);
    }

    QImage image(source.size(), QImage::Format_ARGB32_Premultiplied);

    for (int y{0}; y < source.height(); ++y) {
        auto *destination = reinterpret_cast<QRgb *>(image.scanLine(y));

        for (int x{0}; x < source.width(); ++x) {
            const int alpha{source.format() == QImage::Format_Alpha8 ? source.constScanLine(y)[x] : qGray(source.pixel(x, y))};
            destination[x] = qRgba(alpha, alpha, alpha, alpha);
        }
    }

    return insert(key, image, bearing, GlyphImage::Format::Grayscale);
}

GlyphImage GlyphAtlas::symbol(char32_t cp, QSize cellSize, int strokeWidth)
{
    const Key key{{}, quint32(cp), Kind::Symbol};

    if (auto found = find(key); found.page) {
        return found;
    }

    return insert(key, symbolImage(cp, cellSize, strokeWidth), {}, GlyphImage::Format::Grayscale);
}

GlyphImage GlyphAtlas::underline(int style, QSize cellSize, int strokeWidth)
{
    const Key key{{}, quint32(style), Kind::Underline};

    if (auto found = find(key); found.page) {
        return found;
    }

    const int width{cellSize.width()};
    const qreal radius{strokeWidth / std::numbers::sqrt2};
    const int above{qCeil(style == 3 ? strokeWidth / 2.0 : radius - strokeWidth / 2.0)};
    const qreal extent{style == 3 ? width / std::numbers::pi + strokeWidth / 2.0 : radius + strokeWidth / 2.0};
    const int height{qMin(cellSize.height(), qCeil(above + extent))};
    constexpr int scale{4};
    QImage image(width * scale, height * scale, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter{&image};
    painter.scale(scale, scale);
    painter.setRenderHint(QPainter::Antialiasing);

    if (style == 3) {
        const qreal top{qreal(above)};
        const qreal bottom{qMin(top + width / std::numbers::pi, height - strokeWidth / 2.0)};
        QPainterPath wave;
        wave.moveTo(0, bottom);
        wave.cubicTo(width / 6.0, bottom, width / 3.0, top, width / 2.0, top);
        wave.cubicTo(2 * width / 3.0, top, 5 * width / 6.0, bottom, width, bottom);
        painter.setPen(QPen(Qt::white, strokeWidth, Qt::SolidLine, Qt::FlatCap));
        painter.drawPath(wave);
    } else {
        const int count{qMax(1, int(width / (2 * radius + 1)))};
        const qreal spacing{qreal(width) / count};
        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::white);

        for (int dot{0}; dot < count; ++dot) {
            painter.drawEllipse(QPointF((dot + 0.5) * spacing, above + strokeWidth / 2.0), radius, radius);
        }
    }

    painter.end();

    return insert(key, image.scaled(width, height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation), QPointF(0, -above), GlyphImage::Format::Grayscale);
}

void GlyphAtlas::beginFrame()
{
    ++m_frame;
}

void GlyphAtlas::flush(quint64 *uploadedBytes)
{
    m_source.reset();

    for (const auto &page : m_pages) {
        if (page->dirty) {
            page->texture.reset(m_window->createTextureFromImage(page->image, QQuickWindow::TextureHasAlphaChannel));

            if (page->texture) {
                page->texture->setFiltering(QSGTexture::Nearest);

                if (uploadedBytes) {
                    *uploadedBytes += page->image.sizeInBytes();
                }

                page->dirty = false;
            }
        }
    }
}

void GlyphAtlas::trim()
{
    qsizetype total{byteSize()};

    while (total > PixelBudget) {
        auto oldest = m_pages.end();

        for (auto it = m_pages.begin(); it != m_pages.end(); ++it) {
            if (it->use_count() == 1 && (oldest == m_pages.end() || (*it)->lastUsed < (*oldest)->lastUsed)) {
                oldest = it;
            }
        }

        if (oldest == m_pages.end()) {
            break;
        }

        total -= (*oldest)->image.sizeInBytes();
        m_pages.erase(oldest);
    }

    m_glyphs.removeIf([](auto it) {
        return it->page.expired();
    });
}

int GlyphAtlas::pageCount() const
{
    return int(m_pages.size());
}

qsizetype GlyphAtlas::byteSize() const
{
    return std::accumulate(m_pages.cbegin(), m_pages.cend(), qsizetype{0}, [](qsizetype total, const auto &page) {
        return total + page->image.sizeInBytes();
    });
}
}
