// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "TerminalView.h"
#include "Diagnostics_p.h"
#include "SessionEvents_p.h"
#include "TerminalColors.h"
#include "TerminalSearch.h"
#ifdef Q_OS_MACOS
#include "MacInput_p.h"
#endif
#include <FontMetrics.h>
#include <PreeditLayout.h>
#include <TerminalNode.h>

#include <KColorUtils>
#include <KDirWatch>
#include <KLocalizedString>
#include <KModifierKeyInfo>
#include <KShell>
#include <KUrlMimeData>
#include <QClipboard>
#include <QCursor>
#include <QDir>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFontInfo>
#include <QGuiApplication>
#include <QHoverEvent>
#include <QInputMethod>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPalette>
#include <QQmlInfo>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QScopedValueRollback>
#include <QScreen>
#include <QStyleHints>
#include <QVector>
#include <QWheelEvent>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>
#include <utility>

namespace Kwi
{
namespace
{
int accumulateWheelSteps(qreal delta, qreal stepSize, qreal &remainder)
{
    // Like QAbstractSlider, discard the old fraction when direction reverses.
    if (delta * remainder < 0) {
        remainder = 0;
    }

    remainder += delta;
    const int steps{int(remainder / stepSize)};
    remainder -= steps * stepSize;

    return steps;
}

quint64 keyIdentity(const QKeyEvent *event)
{
#ifdef Q_OS_MACOS
    if (QGuiApplication::platformName() == QLatin1String("cocoa")) {
        if (event->nativeVirtualKey() == 0 && event->key() >= Qt::Key_Shift && event->key() <= Qt::Key_ScrollLock) {
            return quint64(event->key());
        }
        return (quint64(1) << 32) | event->nativeVirtualKey();
    }
#endif
    return event->nativeScanCode() ? (quint64(1) << 32) | event->nativeScanCode() : quint64(event->key());
}

void copyDrag(QQuickItem *source, QMimeData *mime)
{
    auto *drag = new QDrag(source->window() ? static_cast<QObject *>(source->window()) : source);
    drag->setMimeData(mime);
    const QPointer<QDrag> guard{drag};
    drag->exec(Qt::CopyAction, Qt::CopyAction);

    if (guard) {
        guard->deleteLater();
    }
}

struct TextLink {
    qsizetype start;
    qsizetype length;
    QUrl url;
    QString label;
};

QString trimLinkPunctuation(QString text)
{
    while (!text.isEmpty()) {
        const QChar last{text.back()};

        if (QStringLiteral(".,;:!?").contains(last) || (last == QLatin1Char(')') && text.count(last) > text.count(QLatin1Char('(')))
            || (last == QLatin1Char(']') && text.count(last) > text.count(QLatin1Char('[')))
            || (last == QLatin1Char('}') && text.count(last) > text.count(QLatin1Char('{')))) {
            text.chop(1);
        } else {
            break;
        }
    }

    return text;
}

QVector<TextLink> textLinks(const QString &line, const QString &directory, bool continuesBelowViewport, std::optional<qsizetype> position = std::nullopt)
{
    QVector<TextLink> result;
    static const QRegularExpression urls(
        QStringLiteral(
            R"(\b(?:[a-z][a-z0-9+.-]*://|www\.)[^\s<>"']+|mailto:[^\s<>"']+|[a-z0-9.!#$%&*+/=?^_{}|~-]+@[a-z0-9](?:[a-z0-9.-]*[a-z0-9])?\.[a-z]{2,63})"),
        QRegularExpression::CaseInsensitiveOption);
    auto matches = urls.globalMatch(line);

    while (matches.hasNext()) {
        const auto match = matches.next();

        if (continuesBelowViewport && match.capturedEnd() == line.size()) {
            continue;
        }

        const QString label{trimLinkPunctuation(match.captured())};

        if (position && (*position < match.capturedStart() || *position >= match.capturedStart() + label.size())) {
            continue;
        }

        const QUrl url{label.startsWith(QLatin1String("www."), Qt::CaseInsensitive) ? QStringLiteral("https://") + label
                           : label.contains(QLatin1Char(':'))                       ? label
                                                                                    : QStringLiteral("mailto:") + label,
                       QUrl::StrictMode};

        if (url.isValid() && !url.scheme().isEmpty()) {
            result.append({match.capturedStart(), label.size(), url, label});
        }
    }

    static const QRegularExpression paths(QStringLiteral(R"re("([^"]+)"|'([^']+)'|([^\s<>"']+))re"));
    static const QRegularExpression location(QStringLiteral(R"(:[0-9]+(?::[0-9]+)?$)"));
    matches = paths.globalMatch(line);

    while (matches.hasNext()) {
        const auto match = matches.next();
        const int capture{match.capturedStart(1) >= 0 ? 1 : match.capturedStart(2) >= 0 ? 2 : 3};
        const qsizetype start{match.capturedStart(capture)};
        const QString label{capture == 3 ? trimLinkPunctuation(match.captured(capture)) : match.captured(capture)};

        if (label.isEmpty() || (position && (*position < start || *position >= start + label.size()))) {
            continue;
        }

        if (continuesBelowViewport && match.capturedEnd(capture) == line.size()) {
            continue;
        }

        if (std::ranges::any_of(std::as_const(result), [start, &label](const auto &link) {
                return start < link.start + link.length && start + label.size() > link.start;
            })) {
            continue;
        }

        QString path{label};
        path.remove(location);

        if (path.startsWith(QLatin1String("~/"))) {
            path.replace(0, 1, QDir::homePath());
        }

        // Handle absolute paths without filesystem existence checks.
        if (QDir::isRelativePath(path)) {
            if (directory.isEmpty()) {
                continue;
            }

            path = QDir(directory).absoluteFilePath(path);
            const QFileInfo info{path};

            if (!info.isFile() && !info.isDir()) {
                continue;
            }
        }

        result.append({start, label.size(), QUrl::fromLocalFile(path), label});
    }

    std::sort(result.begin(), result.end(), [](const TextLink &a, const TextLink &b) {
        return a.start < b.start;
    });

    return result;
}
}

TerminalView::TerminalView(QQuickItem *parent)
    : QQuickItem{parent}
    , m_search{new TerminalSearch(this)}
#ifdef Q_OS_MACOS
    , m_modifierKeyInfo{nullptr} // KF has no Cocoa lock-state backend; native events carry Caps Lock.
#else
    , m_modifierKeyInfo{new KModifierKeyInfo(this)}
#endif
    , m_paletteWatch{new KDirWatch(this)}
{
    m_paletteReloadTimer.setSingleShot(true);
    m_paletteReloadTimer.setInterval(0);

    connect(&m_paletteReloadTimer, &QTimer::timeout, this, &TerminalView::reloadColors);
    const auto reloadPalette = [this] {
        m_paletteReloadTimer.start();
    };

    connect(m_paletteWatch, &KDirWatch::dirty, this, reloadPalette);
    connect(m_paletteWatch, &KDirWatch::created, this, reloadPalette);
    connect(m_paletteWatch, &KDirWatch::deleted, this, reloadPalette);
    connect(m_search, &TerminalSearch::statusChanged, this, &TerminalView::searchStatusChanged);
    connect(m_search, &TerminalSearch::matchesChanged, this, &TerminalView::refresh);
    connect(m_search, &TerminalSearch::matchApplied, this, [this] {
        setKeyboardSelectionMode(false);
    });

    installAccessibility();
    setFlag(ItemHasContents);
    setFlag(ItemAcceptsInputMethod);
    setFlag(ItemAcceptsDrops);
    setAcceptedMouseButtons(Qt::LeftButton | Qt::MiddleButton | Qt::RightButton);
    setAcceptHoverEvents(true);
    setActiveFocusOnTab(true);
    setClip(true);

    connect(&m_cursorBlinkTimer, &QTimer::timeout, this, [this] {
        m_cursorBlinkVisible = !m_cursorBlinkVisible;
        update();
    });

    m_textBlinkTimer.setInterval(std::chrono::milliseconds{500});

    connect(&m_textBlinkTimer, &QTimer::timeout, this, [this] {
        m_textBlinkVisible = !m_textBlinkVisible;
        update();
    });

    connect(QGuiApplication::styleHints(), &QStyleHints::cursorFlashTimeChanged, this, &TerminalView::resetCursorBlink);
    connect(&m_selectionScrollTimer, &QTimer::timeout, this, &TerminalView::scrollSelection);
    connect(this, &QQuickItem::visibleChanged, this, &TerminalView::resetCursorBlink);
    qGuiApp->installEventFilter(this);

    connect(this, &TerminalView::keyboardSelectionChanged, this, &TerminalView::updateAccessibility);
    connect(this, &QQuickItem::activeFocusChanged, this, &TerminalView::updateAccessibility);
    updateMetrics();
}

TerminalView::~TerminalView()
{
    // QQuickItem can emit these signals after our members are destroyed.
    disconnect(this, &QQuickItem::visibleChanged, this, &TerminalView::resetCursorBlink);
    disconnect(this, &QQuickItem::activeFocusChanged, this, &TerminalView::updateAccessibility);

    if (m_session) {
        if (m_session->terminal())
            m_session->terminal()->cancelSelectionGesture();

        m_session->m_view.clear();
    }
}

Session *TerminalView::session() const
{
    return m_session;
}

void TerminalView::watchScreen(QScreen *screen)
{
    disconnect(m_dpiConnection);

    if (screen) {
        m_dpiConnection = connect(screen, &QScreen::logicalDotsPerInchChanged, this, [this] {
            updateMetrics();
        });
    }

    updateMetrics();
}

void TerminalView::itemChange(ItemChange change, const ItemChangeData &data)
{
    QQuickItem::itemChange(change, data);

    if (change == ItemSceneChange) {
        disconnect(m_screenConnection);

        if (data.window) {
            m_screenConnection = connect(data.window, &QWindow::screenChanged, this, &TerminalView::watchScreen);
        }

        watchScreen(data.window ? data.window->screen() : nullptr);
    } else if (change == ItemDevicePixelRatioHasChanged) {
        updateMetrics();
    }
}

bool TerminalView::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == qGuiApp && event->type() == QEvent::ApplicationPaletteChange) {
        reloadColors();
    } else if (watched == qGuiApp && (event->type() == QEvent::ApplicationFontChange || event->type() == QEvent::ThemeChange)
               && (m_fontFamily.isEmpty() || m_pointSize == 0)) {
        updateMetrics();

        Q_EMIT fontChanged();
    }

    return QQuickItem::eventFilter(watched, event);
}

void TerminalView::setSession(Session *newSession)
{
    if (m_session == newSession) {
        return;
    }

    if (newSession && newSession->m_view && newSession->m_view != this) {
        qmlWarning(this) << "A session can be attached to only one TerminalView. Detach its current view first.";

        return;
    }

    changeSession(newSession);
}

void TerminalView::changeSession(Session *newSession)
{
    const bool hadColors{m_frame.foreground.isValid() || m_frame.background.isValid()};
    resetPreedit();
    m_search->setSession(newSession);

    if (m_session) {
        if (m_session->terminal())
            m_session->terminal()->cancelSelectionGesture();

        m_session->m_view.clear();
        disconnect(m_session, nullptr, this, nullptr);
        disconnect(m_session->events(), nullptr, this, nullptr);
    }

    m_session = newSession;
    m_hadSelection = hasSelection();
    m_selectionTextDirty = false;
    m_zoom = m_session ? m_session->zoomOffset() : 0;
    m_frame = {};
    m_bidi.update(m_frame);
    m_searchMatches = {};
    m_hasBlinkingText = false;
    updateBlinkTimers();
    ++m_frameRevision;
    m_pressedKeys.clear();
    m_selecting = false;
    m_selectionRectangle = false;
    m_keyboardSelectionMode = false;
    m_traceMarkerSeen = false;
    m_selectionScrollTimer.stop();
    updateHoveredLink({}, Qt::NoModifier);
    m_dragPending = false;
    m_wheelAngleRemainder = 0;
    m_wheelPixelRemainder = 0;
    m_zoomWheelRemainder = 0;

    if (m_session) {
        m_session->m_view = this;

        connect(m_session, &QObject::destroyed, this, [this] {
            changeSession(nullptr);
        });
        connect(m_session->events(), &SessionEvents::frameChanged, this, [this] {
            // Update action availability now; invalidate selection text during polish.
            const bool selected{hasSelection()};

            if (selected != m_hadSelection) {
                m_hadSelection = selected;
                m_selectionTextDirty = false;

                Q_EMIT selectionChanged();
            } else if (selected) {
                m_selectionTextDirty = true;
            }

            refresh();
        });
        connect(m_session, &Session::stateChanged, this, [this] {
            resetPreedit();
            updateInputMethod(Qt::ImEnabled | Qt::ImCursorRectangle);
            resetCursorBlink();
        });
        connect(m_session, &Session::readOnlyChanged, this, [this] {
            resetPreedit();
            m_pressedKeys.clear();
            updateInputMethod(Qt::ImEnabled);
            updateAccessibility();
            resetCursorBlink();
        });

        connect(m_session, &Session::applicationTitleChanged, this, &TerminalView::updateAccessibility);
        connect(m_session->events(), &SessionEvents::zoomChanged, this, [this] {
            m_zoom = m_session->zoomOffset();
            updateMetrics();
        });

        reloadColors();
        updateMetrics();
    } else {
        update();
    }

    Q_EMIT sessionChanged();

    if (hadColors) {
        Q_EMIT colorsChanged();
    }

    m_selectionTextDirty = false;

    Q_EMIT selectionChanged();
    Q_EMIT keyboardSelectionChanged();

    {
        QScopedValueRollback publishingScroll(m_publishingScroll, true);

        Q_EMIT scrollChanged();
    }

    updateInputMethod(Qt::ImQueryAll);
    updateAccessibility();
}

QString TerminalView::fontFamily() const
{
    return m_font.family();
}

void TerminalView::setFontFamily(const QString &family)
{
    if (family == m_fontFamily) {
        return;
    }

    m_fontFamily = family;
    updateMetrics();

    Q_EMIT fontChanged();
}

qreal TerminalView::fontPointSize() const
{
    if (m_pointSize > 0) {
        return m_pointSize;
    }

    const QFont systemFont{QFontDatabase::systemFont(QFontDatabase::FixedFont)};

    return systemFont.pointSizeF() > 0 ? systemFont.pointSizeF() : QFontInfo(systemFont).pointSizeF();
}

void TerminalView::setFontPointSize(qreal size)
{
    if (!std::isfinite(size)) {
        return;
    }

    size = size == 0 ? 0 : qBound(6.0, size, 48.0);

    if (m_pointSize == size) {
        return;
    }

    m_pointSize = size;
    updateMetrics();

    Q_EMIT fontChanged();
}

QString TerminalView::colorScheme() const
{
    return m_colorScheme;
}

void TerminalView::setColorScheme(const QString &scheme)
{
    if (m_colorScheme == scheme) {
        return;
    }

    m_colorScheme = scheme;

    if (!m_palettePath.isEmpty()) {
        m_paletteWatch->removeFile(m_palettePath);
    }

    const QPalette applicationPalette{QGuiApplication::palette()};
    const auto palette = loadTerminalPalette(scheme, applicationPalette);
    m_palettePath = palette ? palette->path : ownedTerminalPalettePath(scheme);

    if (!m_palettePath.isEmpty()) {
        m_paletteWatch->addFile(m_palettePath);
    }

    applyColors(palette ? palette->colors : terminalColors(u"system", applicationPalette));

    Q_EMIT appearanceChanged();
}

QColor TerminalView::foregroundColor() const
{
    return m_frame.foreground;
}

QColor TerminalView::backgroundColor() const
{
    return m_frame.background;
}

qreal TerminalView::backgroundOpacity() const
{
    return m_backgroundOpacity;
}

void TerminalView::setBackgroundOpacity(qreal opacity)
{
    if (!std::isfinite(opacity)) {
        return;
    }

    opacity = qBound(0.0, opacity, 1.0);

    if (m_backgroundOpacity == opacity) {
        return;
    }

    m_backgroundOpacity = opacity;
    update();

    Q_EMIT backgroundOpacityChanged();
}

QString TerminalView::cursorStyle() const
{
    return m_cursorStyle;
}

void TerminalView::setCursorStyle(const QString &style)
{
    if (style != QLatin1String("block") && style != QLatin1String("beam") && style != QLatin1String("underline")) {
        qmlWarning(this) << "cursorStyle must be block, beam or underline.";

        return;
    }

    if (m_cursorStyle == style) {
        return;
    }

    m_cursorStyle = style;
    updateCursorDefaults();
    update();

    Q_EMIT appearanceChanged();
}

bool TerminalView::cursorBlinking() const
{
    return m_cursorBlinking;
}

void TerminalView::setCursorBlinking(bool blinking)
{
    if (m_cursorBlinking == blinking) {
        return;
    }

    m_cursorBlinking = blinking;
    updateCursorDefaults();
    resetCursorBlink();

    Q_EMIT appearanceChanged();
}

QString TerminalView::selectedText() const
{
    return m_session ? m_session->selectionText() : QString();
}

bool TerminalView::hasSelection() const
{
    return m_session && m_session->terminal() && m_session->terminal()->hasSelection();
}

QString TerminalView::searchStatus() const
{
    return m_search->status();
}

bool TerminalView::isSearchBusy() const
{
    return m_search->isBusy();
}

qsizetype TerminalView::searchMatchCount() const
{
    return m_search->matchCount();
}

qreal TerminalView::scrollPosition() const
{
    return m_frame.scrollTotal ? qreal(m_frame.scrollOffset) / m_frame.scrollTotal : 0;
}

qreal TerminalView::scrollSize() const
{
    return m_frame.scrollTotal ? qMin(1.0, qreal(m_frame.rows.size()) / m_frame.scrollTotal) : 1;
}

qreal TerminalView::scrollStepSize() const
{
    return m_frame.scrollTotal ? 1.0 / m_frame.scrollTotal : 1;
}

void TerminalView::refresh()
{
    polish();
}

void TerminalView::updatePolish()
{
    const auto previousOffset = m_frame.scrollOffset;
    const auto previousCursor = m_frame.cursor;
    const QColor previousForeground{m_frame.foreground};
    const QColor previousBackground{m_frame.background};
    const bool cursorWasVisible{m_frame.cursorVisible};
    const QRectF previousSelectionCursor{m_keyboardSelectionMode ? selectionCursorRectangle() : QRectF()};

    // Change width and height together, even if Qt updates them one by one.
    resizeTerminal();

    const auto started{traceVerbose() ? traceTime() : 0};
    const bool captured{m_session && m_session->snapshot(m_frame)};
    if (traceEnabled() && captured) {
        trace(QStringLiteral("snapshot-generation"),
              {{QStringLiteral("generation"), qint64(m_frame.generation)}, {QStringLiteral("terminal_id"), qint64(m_frame.terminalId)}});
        if (traceVerbose())
            trace(QStringLiteral("snapshot"),
                  {{QStringLiteral("elapsed_ns"), traceTime() - started},
                   {QStringLiteral("generation"), qint64(m_frame.generation)},
                   {QStringLiteral("columns"), m_frame.columns},
                   {QStringLiteral("rows"), m_frame.rows.size()}});
        const auto marker = QCoreApplication::instance()->property("kwiTraceMarker").toString();
        if (!m_traceMarkerSeen && !marker.isEmpty() && m_session->property("kwiTraceMarkerFed").toBool()) {
            QString text;
            for (const auto &row : std::as_const(m_frame.rows)) {
                for (const auto &cell : row) {
                    text += cell.text;
                }
            }
            if (text.contains(marker)) {
                m_traceMarkerSeen = true;
                trace(QStringLiteral("marker-snapshot"),
                      {{QStringLiteral("generation"), qint64(m_frame.generation)}, {QStringLiteral("terminal_id"), qint64(m_frame.terminalId)}});
            }
        }
    }
    if (captured) {
        m_bidi.update(m_frame, m_bidirectionalText);
        m_searchMatches = m_search->viewportMatches(previousOffset != m_frame.scrollOffset);
        ++m_frameRevision;

        if (m_frame.foreground != previousForeground || m_frame.background != previousBackground) {
            Q_EMIT colorsChanged();
        }

        if (m_keyboardSelectionMode && !m_session->terminal()->hasSelection()) {
            setKeyboardSelectionMode(false);
        }

        if (m_keyboardSelectionMode && selectionCursorRectangle() != previousSelectionCursor) {
            Q_EMIT keyboardSelectionChanged();
        }

        m_hasBlinkingText = std::ranges::any_of(std::as_const(m_frame.rows), [](const auto &row) {
            return std::ranges::any_of(row, [](const auto &cell) {
                return (cell.flags & GhosttyQt::Blink) && !(cell.flags & GhosttyQt::Invisible);
            });
        });

        updateBlinkTimers();

        if (m_frame.cursor != previousCursor || (m_frame.cursorVisible && !cursorWasVisible)) {
            resetCursorBlink();
        }

        if (m_selectionTextDirty) {
            m_selectionTextDirty = false;

            Q_EMIT selectionChanged();
        }

        {
            // A size update can clamp the scrollbar position without a user scroll request.
            QScopedValueRollback publishingScroll(m_publishingScroll, true);

            Q_EMIT scrollChanged();
        }

        updateInputMethod(Qt::ImCursorRectangle);
        updateAccessibility();
        update();
    }

    updateHoveredLink(m_hoverPosition, QGuiApplication::keyboardModifiers());
}

void TerminalView::updateMetrics()
{
    m_font = m_fontFamily.isEmpty() ? QFontDatabase::systemFont(QFontDatabase::FixedFont) : QFont(m_fontFamily);
    m_font.setStyleHint(QFont::Monospace, m_font.styleStrategy());
    m_font.setPointSizeF(qBound(6.0, fontPointSize() + m_zoom, 72.0));
    const qreal dpr{window() ? window()->effectiveDevicePixelRatio() : 1};
    const qreal dpi{window() && window()->screen() ? window()->screen()->logicalDotsPerInch() : 96};

    // Align the clip and padding on a physical pixel to avoid a seam.
    m_padding = qRound(Padding * dpr) / dpr;
    const auto metrics = GhosttyQt::Quick::fontMetrics(m_font, dpr, dpi);
    m_cellWidth = metrics.cellSize.width();
    m_cellHeight = metrics.cellSize.height();

    refresh();
    update();
}

QSizeF TerminalView::sizeForGrid(int columns, int rows) const
{
    if (columns < 2 || rows < 1) {
        return {};
    }

    return {columns * m_cellWidth + 2 * m_padding, rows * m_cellHeight + 2 * m_padding};
}

void TerminalView::resizeTerminal()
{
    // Wait for layout instead of reflowing a live session through a 2×1 grid.
    if (m_session && width() >= 2 * m_padding + 2 * m_cellWidth && height() >= 2 * m_padding + m_cellHeight) {
        const qreal dpr{window() ? window()->effectiveDevicePixelRatio() : 1};
        m_session->resize(int((width() - 2 * m_padding) / m_cellWidth),
                          int((height() - 2 * m_padding) / m_cellHeight),
                          QSize(qRound(m_cellWidth * dpr), qRound(m_cellHeight * dpr)));
    }
}

void TerminalView::geometryChange(const QRectF &next, const QRectF &previous)
{
    QQuickItem::geometryChange(next, previous);

    if (next.size() != previous.size()) {
        refresh();
    }
}

void TerminalView::reloadColors()
{
    if (!m_session || !m_session->terminal()) {
        return;
    }

    applyColors(terminalColors(m_colorScheme, QGuiApplication::palette()));
}

void TerminalView::applyColors(const TerminalColors &colors)
{
    if (!m_session || !m_session->terminal()) {
        return;
    }

    m_session->setColors(colors.foreground, colors.background, colors.ansi, colors.boldForeground);
    m_session->terminal()->setDecorations(colors.cursor, colors.selectionForeground, colors.selectionBackground);
    updateCursorDefaults();
    refresh();
}

void TerminalView::updateBlinkTimers()
{
    const int flashTime{QGuiApplication::styleHints()->cursorFlashTime()};
    const bool cursor{hasActiveFocus() && m_frame.cursorVisible && m_frame.cursorBlinking && m_preedit.isEmpty() && flashTime > 0};

    if (isVisible() && cursor) {
        const int interval{qMax(1, flashTime / 2)};

        if (m_cursorBlinkTimer.interval() != interval) {
            m_cursorBlinkTimer.setInterval(interval);
        }

        if (!m_cursorBlinkTimer.isActive()) {
            m_cursorBlinkTimer.start();
        }
    } else {
        m_cursorBlinkTimer.stop();
        m_cursorBlinkVisible = true;
    }

    if (isVisible() && m_hasBlinkingText) {
        if (!m_textBlinkTimer.isActive()) {
            m_textBlinkTimer.start();
        }
    } else {
        m_textBlinkTimer.stop();
        m_textBlinkVisible = true;
    }
}

void TerminalView::resetCursorBlink()
{
    // Restart only the cursor blink so typing does not interrupt blinking text.
    m_cursorBlinkTimer.stop();
    m_cursorBlinkVisible = true;
    updateBlinkTimers();
    update();
}

void TerminalView::updateCursorDefaults()
{
    if (m_session && m_session->terminal()) {
        const auto style = m_cursorStyle == QLatin1String("beam") ? GhosttyQt::CursorStyle::Beam
            : m_cursorStyle == QLatin1String("underline")         ? GhosttyQt::CursorStyle::Underline
                                                                  : GhosttyQt::CursorStyle::Block;
        m_session->terminal()->setDefaultCursor(style, m_cursorBlinking);
        refresh();
    }
}

QSGNode *TerminalView::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *)
{
    if (m_frame.rows.isEmpty()) {
        delete oldNode;

        return nullptr;
    }

    auto *node = static_cast<GhosttyQt::Quick::TerminalNode *>(oldNode);

    if (!node) {
        GhosttyQt::Quick::TerminalNode::StatisticsCallback prepared;
        if (traceEnabled()) {
            prepared = [](const GhosttyQt::Quick::TerminalNode::Statistics &stats) {
                trace(QStringLiteral("render"),
                      {{QStringLiteral("terminal_id"), qint64(stats.terminalId)},
                       {QStringLiteral("generation"), qint64(stats.frameGeneration)},
                       {QStringLiteral("prepare_ns"), stats.prepareNanoseconds},
                       {QStringLiteral("shaping_ns"), stats.shapingNanoseconds},
                       {QStringLiteral("glyph_build_ns"), stats.glyphBuildNanoseconds},
                       {QStringLiteral("texture_flush_ns"), stats.textureFlushNanoseconds},
                       {QStringLiteral("shaped_runs"), qint64(stats.shapedRuns)},
                       {QStringLiteral("rasterized_glyphs"), qint64(stats.rasterizedGlyphs)},
                       {QStringLiteral("upload_bytes"), qint64(stats.textureUploadBytes)},
                       {QStringLiteral("rebuilt_rows"), qint64(stats.rebuiltRows)},
                       {QStringLiteral("atlas_bytes"), qint64(stats.atlasBytes)}});
            };
        }
        node = new GhosttyQt::Quick::TerminalNode(window(), traceEnabled(), std::move(prepared));
        if (traceEnabled()) {
            const QFontInfo resolved{m_font};
            trace(QStringLiteral("configuration"),
                  {{QStringLiteral("font_family"), resolved.family()},
                   {QStringLiteral("font_point_size"), resolved.pointSizeF()},
                   {QStringLiteral("font_pixel_size"), resolved.pixelSize()},
                   {QStringLiteral("opacity"), m_backgroundOpacity},
                   {QStringLiteral("scrollback_lines"), m_session ? m_session->scrollbackLines() : -2}});
        }
    }

    GhosttyQt::Quick::TerminalNode::Options options;
    options.rect = boundingRect();
    options.contentRect = boundingRect().adjusted(m_padding, m_padding, -m_padding, -m_padding);
    options.backgroundOpacity = m_backgroundOpacity;
    options.font = m_font;
    options.devicePixelRatio = window()->effectiveDevicePixelRatio();
    options.logicalDpi = window()->screen() ? window()->screen()->logicalDotsPerInch() : 96;
    options.focused = hasActiveFocus();
    options.cursorBlinkVisible = m_cursorBlinkVisible || !hasActiveFocus();
    options.textBlinkVisible = m_textBlinkVisible;
    options.preedit = m_preedit;
    options.preeditAttributes = m_preeditAttributes;
    options.bidiRows = m_bidi.rows();
    options.searchMatches = m_bidi.visualRegion(m_searchMatches);
    options.hoveredLink = m_bidi.visualRegion(m_highlightedLink);
    const QPalette palette{QGuiApplication::palette()};
    options.searchForeground = palette.color(QPalette::Active, QPalette::Text);
    options.searchBackground = KColorUtils::mix(palette.color(QPalette::Active, QPalette::Base), palette.color(QPalette::Active, QPalette::Highlight), 0.35);
    if (m_bidirectionalText && !m_preedit.isEmpty()) {
        const auto composed = GhosttyQt::Quick::frameWithPreedit(m_frame, GhosttyQt::Quick::layoutPreedit(m_frame, m_preedit, m_preeditAttributes));
        GhosttyQt::Quick::BidiLayout bidi;
        bidi.update(composed);
        options.bidiRows = bidi.rows();
        options.searchMatches = bidi.visualRegion(m_searchMatches);
        options.hoveredLink = bidi.visualRegion(m_highlightedLink);
        options.composedPreedit = true;
        node->synchronize(bidi.visualFrame(composed), options);
    } else {
        node->synchronize(m_bidi.visualFrame(m_frame), options);
    }

    return node;
}

void TerminalView::copy()
{
    const QString text{selectedText()};

    if (!text.isEmpty()) {
        QGuiApplication::clipboard()->setText(text);
    }
}

void TerminalView::openLink()
{
    if (m_hoveredLink.isValid() && !m_hoveredLink.scheme().isEmpty()) {
        Q_EMIT linkActivated(m_hoveredLink);
    }
}

void TerminalView::copyLink(const QUrl &url)
{
    if (url.isValid() && !url.scheme().isEmpty()) {
        QGuiApplication::clipboard()->setText(url.toString(QUrl::FullyEncoded));
    }
}

QUrl TerminalView::hoveredLink() const
{
    return m_hoveredLink;
}

QUrl TerminalView::linkAtPosition(QPointF position, QRegion *cells) const
{
    if (!m_session || !m_session->terminal() || position.x() < m_padding || position.y() < m_padding
        || position.x() >= m_padding + m_frame.columns * m_cellWidth || position.y() >= m_padding + m_frame.rows.size() * m_cellHeight) {
        return {};
    }

    const QPoint cell{glyphStart(cellAt(position))};
    const QString uri{m_session->terminal()->linkAt(cell)};

    if (!uri.isEmpty()) {
        const QUrl link{uri, QUrl::StrictMode};

        if (!link.isValid() || link.scheme().isEmpty()) {
            return {};
        }

        if (cells) {
            const int columns{m_frame.columns};
            int first{cell.y() * columns + cell.x()};
            int last{first};
            const auto uriAt = [&](int index) {
                return m_session->terminal()->linkAt(glyphStart({index % columns, index / columns}));
            };

            while (first > 0 && uriAt(first - 1) == uri) {
                --first;
            }

            while (last + 1 < columns * m_frame.rows.size() && uriAt(last + 1) == uri) {
                ++last;
            }

            for (int row{first / columns}; row <= last / columns; ++row) {
                const int left{row == first / columns ? first % columns : 0};
                const int right{row == last / columns ? last % columns : columns - 1};
                *cells += QRect(left, row, right - left + 1, 1);
            }
        }

        return link;
    }

    if (cell.y() >= m_frame.rows.size()) {
        return {};
    }

    QString line;
    qsizetype offset{0};
    int firstRow{cell.y()};
    int lastRow{cell.y()};

    while (firstRow > 0 && m_frame.rowWrapped.value(firstRow - 1)) {
        --firstRow;
    }

    while (lastRow + 1 < m_frame.rows.size() && m_frame.rowWrapped.value(lastRow)) {
        ++lastRow;
    }

    for (int y{firstRow}; y <= lastRow; ++y) {
        const auto &row = m_frame.rows[y];

        for (qsizetype column{0}; column < row.size(); ++column) {
            if (y == cell.y() && column == cell.x()) {
                offset = line.size();
            }

            if (row[column].width != 0) {
                line += row[column].text.isEmpty() ? QStringLiteral(" ") : row[column].text;
            }
        }
    }

    const auto matches = textLinks(line, m_session->workingDirectory(), m_frame.rowWrapped.value(lastRow), offset);

    const auto match = std::ranges::find_if(matches, [offset](const auto &candidate) {
        return offset >= candidate.start && offset < candidate.start + candidate.length;
    });

    if (match == matches.end()) {
        return {};
    }

    if (cells) {
        qsizetype start{0};

        for (int y{firstRow}; y <= lastRow; ++y) {
            const auto &row = m_frame.rows[y];

            for (int x{0}; x < row.size(); ++x) {
                const auto &entry = row[x];
                const qsizetype length{entry.width ? qMax(qsizetype(1), entry.text.size()) : 0};

                if (length && start < match->start + match->length && start + length > match->start) {
                    *cells += QRect(x, y, entry.width, 1);
                }

                start += length;
            }
        }
    }

    return match->url;
}

QVariantList TerminalView::links() const
{
    QVariantList links;

    if (!m_session || !m_session->terminal()) {
        return links;
    }

    GhosttyQt::Frame frame;

    if (!m_session->snapshot(frame)) {
        return links;
    }

    QSet<QString> seen;
    const auto append = [&](const QUrl &url, const QString &label) {
        const QString key{url.toString(QUrl::FullyEncoded)};

        if (!url.isValid() || url.scheme().isEmpty() || seen.contains(key)) {
            return;
        }

        seen.insert(key);
        const QString displayLabel{label.trimmed()};
        links.append(QVariantMap{{QStringLiteral("url"), url}, {QStringLiteral("label"), displayLabel.isEmpty() ? key : displayLabel}});
    };

    QString logicalLine;
    const QString directory{m_session->workingDirectory()};

    for (int y{0}; y < frame.rows.size(); ++y) {
        const auto &row = frame.rows.at(y);
        QUrl previousExplicit;
        QString explicitLabel;

        for (int x{0}; x < row.size(); ++x) {
            const auto &cell = row[x];

            if (cell.width == 0) {
                continue;
            }

            const QString cellText{cell.text.isEmpty() ? QStringLiteral(" ") : cell.text};
            const QUrl explicitUrl{m_session->terminal()->linkAt({x, y}), QUrl::StrictMode};
            logicalLine += explicitUrl.isEmpty() ? cellText : QString(cellText.size(), QLatin1Char(' '));

            if (explicitUrl != previousExplicit) {
                append(previousExplicit, explicitLabel);
                previousExplicit = explicitUrl;
                explicitLabel.clear();
            }

            if (!explicitUrl.isEmpty()) {
                explicitLabel += cellText;
            }
        }

        append(previousExplicit, explicitLabel);

        if (!frame.rowWrapped.value(y) || y + 1 == frame.rows.size()) {
            const auto matches = textLinks(logicalLine, directory, frame.rowWrapped.value(y));

            for (const auto &link : matches) {
                append(link.url, link.label);
            }

            logicalLine.clear();
        }
    }

    return links;
}

void TerminalView::updateHoveredLink(std::optional<QPointF> position, Qt::KeyboardModifiers modifiers)
{
    m_hoverPosition = position;
    QRegion cells;
    const bool allowed{linkActivationAllowed(modifiers)};
    const QUrl link{position ? linkAtPosition(*position, allowed ? &cells : nullptr) : QUrl()};

    if (m_highlightedLink != cells) {
        m_highlightedLink = cells;
        update();
    }

    if (m_hoveredLink != link) {
        m_hoveredLink = link;

        Q_EMIT hoveredLinkChanged();
    }

    const auto shape = !link.isEmpty() && allowed ? Qt::PointingHandCursor
        : m_session && m_session->terminal()      ? m_session->terminal()->mouseCursorShape()
                                                  : Qt::IBeamCursor;

    if (cursor().shape() != shape) {
        setCursor(shape);
    }
}

bool TerminalView::requireCtrlForLinks() const
{
    return m_requireCtrlForLinks;
}

void TerminalView::setRequireCtrlForLinks(bool required)
{
    if (m_requireCtrlForLinks == required) {
        return;
    }

    m_requireCtrlForLinks = required;
    updateHoveredLink(m_hoverPosition, QGuiApplication::keyboardModifiers());

    Q_EMIT requireCtrlForLinksChanged();
}

bool TerminalView::linkActivationAllowed(Qt::KeyboardModifiers modifiers) const
{
    if (modifiers.testFlag(Qt::AltModifier)) {
        return false;
    }

    if (modifiers.testFlag(Qt::ControlModifier)) {
        return true;
    }

    return !m_requireCtrlForLinks && !modifiers.testFlag(Qt::ShiftModifier)
        && (!m_session || m_session->isReadOnly() || !m_session->terminal() || !m_session->terminal()->mouseTracking());
}

void TerminalView::requestPaste(const QString &text)
{
    if (!m_session || m_session->isReadOnly()) {
        return;
    }

    if (m_confirmMultilinePaste && (text.contains(QLatin1Char('\n')) || text.contains(QLatin1Char('\r')))) {
        Q_EMIT multilinePasteRequested(m_session, text);
    } else {
        m_session->pasteApprovedText(text);
        refresh();
        forceActiveFocus();

        if (!text.isEmpty()) {
            resetCursorBlink();
        }
    }
}

void TerminalView::paste()
{
    requestPaste(QGuiApplication::clipboard()->text());
}

void TerminalView::pasteSelection()
{
    if (QGuiApplication::clipboard()->supportsSelection())
        requestPaste(QGuiApplication::clipboard()->text(QClipboard::Selection));
}

bool TerminalView::copyCommandOutput()
{
    if (!m_session) {
        return false;
    }

    const QString output{m_session->commandOutput()};

    if (output.isEmpty()) {
        return false;
    }

    QGuiApplication::clipboard()->setText(output);

    return true;
}

void TerminalView::selectAll()
{
    setKeyboardSelectionMode(false);

    if (m_session && m_session->terminal()) {
        m_session->terminal()->selectAll();
    }
}

void TerminalView::find(const QString &text, bool forward)
{
    if (!m_session) {
        return;
    }

    if (text.isEmpty()) {
        clearSearch();

        return;
    }

    setKeyboardSelectionMode(false);
    m_search->find(text, forward);
}

void TerminalView::clearSearch()
{
    setKeyboardSelectionMode(false);
    m_search->clear();

    if (m_session && m_session->terminal()) {
        m_session->terminal()->clearSelection();
    }
}

void TerminalView::zoomIn()
{
    if (m_session) {
        m_session->setZoomOffset(qMin(72.0 - fontPointSize(), m_zoom + 1.0));
    }
}

void TerminalView::zoomOut()
{
    if (m_session) {
        m_session->setZoomOffset(qMax(6.0 - fontPointSize(), m_zoom - 1.0));
    }
}

void TerminalView::resetZoom()
{
    if (m_session) {
        m_session->setZoomOffset(0);
    }
}

void TerminalView::scrollTo(qreal position)
{
    if (!m_session || !m_session->terminal() || m_publishingScroll || !std::isfinite(position)) {
        return;
    }

    // Round to avoid scrolling backward when the scrollbar echoes its current position.
    const auto offset = quint64(std::round(qBound(0.0, position, 1.0) * m_frame.scrollTotal));

    m_session->terminal()->scrollTo(offset);
    refresh();
}

void TerminalView::scrollHistory(QPoint pixelDelta, QPoint angleDelta, Qt::KeyboardModifiers modifiers)
{
    if (!m_session || !m_session->terminal()) {
        return;
    }

    int rows{0};

    if (modifiers.testFlag(Qt::ShiftModifier)) {
        // Match QScrollBar: cap at one page per event and discard fractional rows.
        m_wheelAngleRemainder = 0;
        m_wheelPixelRemainder = 0;
        m_zoomWheelRemainder = 0;
        const qreal pages{qBound(-1.0, angleDelta.y() / 120.0, 1.0)};
        rows = int(pages * m_frame.rows.size());
    } else {
        rows = wheelSteps(pixelDelta, angleDelta, QGuiApplication::styleHints()->wheelScrollLines());
    }

    if (rows != 0) {
        m_session->terminal()->scroll(-rows);
        refresh();
    }
}

bool TerminalView::bidirectionalText() const
{
    return m_bidirectionalText;
}

void TerminalView::setBidirectionalText(bool enabled)
{
    if (m_bidirectionalText == enabled)
        return;
    m_bidirectionalText = enabled;
    m_bidi.update(m_frame, enabled);
    ++m_frameRevision;
    updateInputMethod(Qt::ImCursorRectangle);
    updateAccessibility();
    update();
    Q_EMIT bidirectionalTextChanged();
    Q_EMIT keyboardSelectionChanged();
}

QRectF TerminalView::logicalCellRectangle(QPoint cell) const
{
    const QPoint logical = glyphStart(cell);
    int width = 1;
    if (logical.y() >= 0 && logical.y() < m_frame.rows.size() && logical.x() >= 0 && logical.x() < m_frame.rows[logical.y()].size())
        width = qMax(1, int(m_frame.rows[logical.y()][logical.x()].width));
    const QPoint visual = m_bidi.visualCell(logical);
    return QRectF(m_padding + visual.x() * m_cellWidth, m_padding + visual.y() * m_cellHeight, width * m_cellWidth, m_cellHeight);
}

bool TerminalView::optionAsAlt() const
{
    return m_optionAsAlt;
}

void TerminalView::setOptionAsAlt(bool enabled)
{
    if (std::exchange(m_optionAsAlt, enabled) == enabled)
        return;
    resetPreedit();
    updateInputMethod(Qt::ImEnabled);
    Q_EMIT optionAsAltChanged();
}

void TerminalView::encodeKey(QKeyEvent *event)
{
    if (!m_session || !m_session->terminal()) {
        return;
    }

    if (m_session->isReadOnly()) {
        event->accept();

        return;
    }

#ifdef Q_OS_MACOS
    const GhosttyQt::KeyboardLocks locks{.capsLock = bool(event->nativeModifiers() & (1UL << 16))};
#else
    const GhosttyQt::KeyboardLocks locks{.capsLock = m_modifierKeyInfo->isKeyLocked(Qt::Key_CapsLock),
                                         .numLock = m_modifierKeyInfo->isKeyLocked(Qt::Key_NumLock)};
#endif
    const auto output = m_session->terminal()->encodeKey(*event, locks, m_optionAsAlt);

    if (!output.isEmpty()) {
        m_session->send(output);

        if (event->type() != QEvent::KeyRelease) {
            m_session->terminal()->scrollBottom();
            m_session->terminal()->clearSelection();
            refresh();
        }
    }

    if (event->type() == QEvent::KeyPress) {
        resetCursorBlink();
    }

    event->accept();
}

bool TerminalView::keyboardSelectionMode() const
{
    return m_keyboardSelectionMode;
}

QPoint TerminalView::glyphStart(QPoint cell) const
{
    if (cell.y() >= 0 && cell.y() < m_frame.rows.size() && cell.x() >= 0 && cell.x() < m_frame.rows[cell.y()].size()) {
        const auto &row = m_frame.rows[cell.y()];

        while (cell.x() > 0 && row[cell.x()].width == 0)
            cell.rx() -= 1;
    }

    return cell;
}

QRectF TerminalView::selectionCursorRectangle() const
{
    const QPoint cell{qMin(m_keyboardCursor.x(), m_frame.columns - 1), m_keyboardCursor.y()};
    int width{1};

    if (cell.y() >= 0 && cell.y() < m_frame.rows.size() && cell.x() >= 0 && cell.x() < m_frame.rows[cell.y()].size()) {
        width = qMax(1, int(m_frame.rows[cell.y()][cell.x()].width));
    }

    const QPoint visual = m_bidi.visualCell(cell);
    return QRectF(m_padding + visual.x() * m_cellWidth, m_padding + visual.y() * m_cellHeight, width * m_cellWidth, m_cellHeight);
}

void TerminalView::toggleSelectionMode()
{
    setKeyboardSelectionMode(!m_keyboardSelectionMode);
}

void TerminalView::setKeyboardSelectionMode(bool enabled)
{
    if (m_keyboardSelectionMode == enabled || (enabled && (!m_session || !m_session->terminal() || m_frame.rows.isEmpty()))) {
        return;
    }

    m_keyboardSelectionMode = enabled;
    m_pressedKeys.clear();
    resetPreedit();
    updateInputMethod(Qt::ImEnabled);

    if (enabled) {
        const QPoint cell{qBound(0, m_frame.cursor.x(), m_frame.columns - 1), qBound(0, m_frame.cursor.y(), int(m_frame.rows.size()) - 1)};
        m_session->terminal()->select(cell, cell, false);
        m_keyboardCursor = glyphStart(cell);
        m_keyboardAnchor = m_keyboardCursor;

        forceActiveFocus();
    }

    Q_EMIT keyboardSelectionChanged();
}

bool TerminalView::handleSelectionKey(QKeyEvent *event)
{
    if (!m_keyboardSelectionMode || !m_session || !m_session->terminal()) {
        return false;
    }

    // Output can invalidate the native selection before the next frame is polished.
    if (!m_session->terminal()->hasSelection()) {
        setKeyboardSelectionMode(false);

        return false;
    }

    const int key{event->key()};

    if (key == Qt::Key_Escape) {
        setKeyboardSelectionMode(false);
        event->accept();

        return true;
    }

    if (key == Qt::Key_Return || key == Qt::Key_Enter || (key == Qt::Key_C && event->modifiers().testFlag(Qt::ControlModifier))) {
        copy();
        setKeyboardSelectionMode(false);
        event->accept();

        return true;
    }

    if (key == Qt::Key_Tab || key == Qt::Key_Backtab) {
        setKeyboardSelectionMode(false);
        event->ignore();

        return true;
    }

    const QPoint visual = m_bidi.visualCell(m_keyboardCursor);
    const bool visualNavigation = !m_bidi.rows().value(visual.y()).logicalToVisual.isEmpty()
        || ((key == Qt::Key_Up || key == Qt::Key_Down) && !m_bidi.rows().value(visual.y() + (key == Qt::Key_Up ? -1 : 1)).logicalToVisual.isEmpty());
    if (visualNavigation
        && (key == Qt::Key_Left || key == Qt::Key_Right || key == Qt::Key_Up || key == Qt::Key_Down
            || ((key == Qt::Key_Home || key == Qt::Key_End) && !event->modifiers().testFlag(Qt::ControlModifier)))) {
        QPoint next = visual;
        if (key == Qt::Key_Left)
            --next.rx();
        if (key == Qt::Key_Right)
            next.rx() += qMax(1, int(m_frame.rows[m_keyboardCursor.y()][qBound(0, m_keyboardCursor.x(), m_frame.columns - 1)].width));
        if (key == Qt::Key_Up)
            --next.ry();
        if (key == Qt::Key_Down)
            ++next.ry();
        if (key == Qt::Key_Home)
            next.setX(0);
        if (key == Qt::Key_End)
            next.setX(m_frame.columns - 1);
        if (next.x() < 0)
            next = {m_frame.columns - 1, next.y() - 1};
        if (next.x() >= m_frame.columns)
            next = {0, next.y() + 1};
        if (next.y() >= 0 && next.y() < m_frame.rows.size()) {
            m_keyboardCursor = glyphStart(m_bidi.logicalCell(next));
            if (!event->modifiers().testFlag(Qt::ShiftModifier))
                m_keyboardAnchor = m_keyboardCursor;
            m_session->terminal()->select(m_keyboardAnchor, m_keyboardCursor);
            Q_EMIT keyboardSelectionChanged();
            event->accept();
            return true;
        }
    }

    using Adjustment = GhosttyQt::SelectionAdjustment;
    Adjustment adjustment;
    const bool control{event->modifiers().testFlag(Qt::ControlModifier)};

    switch (key) {
    case Qt::Key_Left:
        adjustment = Adjustment::Left;
        break;
    case Qt::Key_Right:
        adjustment = Adjustment::Right;
        break;
    case Qt::Key_Up:
        adjustment = Adjustment::Up;
        break;
    case Qt::Key_Down:
        adjustment = Adjustment::Down;
        break;
    case Qt::Key_Home:
        adjustment = control ? Adjustment::Start : Adjustment::StartOfLine;
        break;
    case Qt::Key_End:
        adjustment = control ? Adjustment::End : Adjustment::EndOfLine;
        break;
    case Qt::Key_PageUp:
        adjustment = Adjustment::PageUp;
        break;
    case Qt::Key_PageDown:
        adjustment = Adjustment::PageDown;
        break;
    default:
        event->accept();

        return true;
    }

    // Keep the logical end-of-text position when Right cannot advance.
    if (key == Qt::Key_Right && m_keyboardCursor == QPoint{m_frame.columns, int(m_frame.rows.size()) - 1}
        && m_frame.scrollOffset + m_frame.rows.size() == m_frame.scrollTotal) {
        event->accept();

        return true;
    }

    // A logical line-end caret is one position beyond the native selection.
    const auto next = key == Qt::Key_Left && m_keyboardCursor.x() == m_frame.columns
        ? std::optional<QPoint>{glyphStart({m_frame.columns - 1, m_keyboardCursor.y()})}
        : m_session->terminal()->adjustSelection(adjustment, event->modifiers().testFlag(Qt::ShiftModifier));

    if (!next) {
        event->accept();

        return true;
    }

    if (m_keyboardCursor != *next) {
        m_keyboardCursor = *next;
        if (!event->modifiers().testFlag(Qt::ShiftModifier))
            m_keyboardAnchor = m_keyboardCursor;
        Q_EMIT keyboardSelectionChanged();
    }

    event->accept();

    return true;
}

void TerminalView::showContextMenu()
{
    const QRectF cursor{m_keyboardSelectionMode ? selectionCursorRectangle() : inputMethodQuery(Qt::ImCursorRectangle).toRectF()};
    const QPointF position{cursor.center()};
    updateHoveredLink(position, QGuiApplication::keyboardModifiers());

    Q_EMIT contextMenuRequested(position);
}

void TerminalView::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Menu || (event->key() == Qt::Key_F10 && event->modifiers() == Qt::ShiftModifier)) {
        showContextMenu();
        event->accept();

        return;
    }

    if (handleSelectionKey(event)) {
        return;
    }

    if (event->key() == Qt::Key_Control || event->key() == Qt::Key_Shift || event->key() == Qt::Key_Alt) {
        updateHoveredLink(m_hoverPosition, event->modifiers());
    }

    if (m_session && m_session->terminal() && event->modifiers() == Qt::ShiftModifier && (event->key() == Qt::Key_PageUp || event->key() == Qt::Key_PageDown)) {
        m_session->terminal()->scroll((event->key() == Qt::Key_PageUp ? -1 : 1) * qMax(1, int(m_frame.rows.size()) - 1));
        refresh();
        event->accept();

        return;
    }

    if (m_session) {
        m_pressedKeys.insert(keyIdentity(event));
    }

    encodeKey(event);
}

void TerminalView::keyReleaseEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Control || event->key() == Qt::Key_Shift || event->key() == Qt::Key_Alt) {
        updateHoveredLink(m_hoverPosition, event->modifiers());
    }

    // Application shortcuts can consume a press before it reaches this item.
    // Do not leak an unmatched release into Kitty's keyboard protocol.
    if (!event->isAutoRepeat() && m_pressedKeys.remove(keyIdentity(event))) {
        encodeKey(event);
    }

    event->accept();
}

void TerminalView::resetPreedit()
{
    // Reset the input context too, or it may commit composition after the overlay is cleared.
    if (!m_preedit.isEmpty()) {
        // Cocoa can synchronously commit its marked text during reset. This is
        // cancellation (focus/session/read-only change), not a user commit.
        const QScopedValueRollback resetting{m_resettingPreedit, true};
        QGuiApplication::inputMethod()->reset();
    }

    m_preedit.clear();
    m_preeditAttributes.clear();
}

void TerminalView::inputMethodEvent(QInputMethodEvent *event)
{
    if (m_resettingPreedit || !hasActiveFocus() || !m_session || m_session->state() != Session::State::Running || !m_session->terminal()
        || m_session->isReadOnly() || m_keyboardSelectionMode) {
        event->accept();

        return;
    }

    if (!event->commitString().isEmpty()) {
        m_session->send(event->commitString().toUtf8());
    }

    if (!event->commitString().isEmpty() || !event->preeditString().isEmpty()) {
        m_session->terminal()->scrollBottom();
        m_session->terminal()->clearSelection();
        refresh();
    }

    m_preedit = event->preeditString();
    m_preeditAttributes = event->attributes();
    resetCursorBlink();
    updateInputMethod(Qt::ImCursorRectangle);
    update();
    event->accept();
}

void TerminalView::dragEnterEvent(QDragEnterEvent *event)
{
    if (m_session && m_session->isActive() && !m_session->isReadOnly() && event->possibleActions().testFlag(Qt::CopyAction)
        && (event->mimeData()->hasText() || event->mimeData()->hasFormat(QStringLiteral("application/vnd.portal.filetransfer"))
            || std::ranges::any_of(KUrlMimeData::mimeDataTypes(), [event](const QString &type) {
                   return event->mimeData()->hasFormat(type);
               }))) {
        event->setDropAction(Qt::CopyAction);
        event->accept();
    } else {
        event->ignore();
    }
}

void TerminalView::dragMoveEvent(QDragMoveEvent *event)
{
    if (m_session && m_session->isActive() && !m_session->isReadOnly() && event->possibleActions().testFlag(Qt::CopyAction)) {
        event->setDropAction(Qt::CopyAction);
        event->accept();
    } else {
        event->ignore();
    }
}

void TerminalView::dropEvent(QDropEvent *event)
{
    if (!m_session || !m_session->isActive() || m_session->isReadOnly() || !event->possibleActions().testFlag(Qt::CopyAction)) {
        event->ignore();

        return;
    }

    const auto *mime = event->mimeData();
    const auto urls = KUrlMimeData::urlsFromMimeData(mime, KUrlMimeData::PreferLocalUrls);
    QString text;

    if (!urls.isEmpty()) {
        for (const QUrl &url : urls) {
            if (!url.isValid()) {
                continue;
            }

            text += KShell::quoteArg(url.isLocalFile() ? url.toLocalFile() : url.toString(QUrl::FullyEncoded));
            text += QLatin1Char(' ');
        }
    } else if (mime->hasText()) {
        text = mime->text();
    } else {
        event->ignore();

        return;
    }

    requestPaste(text);
    event->setDropAction(Qt::CopyAction);
    event->accept();
}

QVariant TerminalView::inputMethodQuery(Qt::InputMethodQuery query) const
{
    switch (query) {
    case Qt::ImEnabled:
#ifdef Q_OS_MACOS
        // Bypass Cocoa composition only for a native Option event in this view's
        // window. Other keys/IMEs retain the normal input context. Qt queries
        // ImEnabled before interpreting dead keys, after checking shortcuts.
        if (m_optionAsAlt && nativeOptionKeyEvent(window()))
            return false;
#endif
        return m_session && m_session->state() == Session::State::Running && m_session->terminal() && !m_session->isReadOnly() && !m_keyboardSelectionMode;
    case Qt::ImCursorRectangle: {
        const auto preedit = GhosttyQt::Quick::layoutPreedit(m_frame, m_preedit, m_preeditAttributes);
        QPoint caret = m_bidi.visualCell(preedit.caret);
        if (m_bidirectionalText && !m_preedit.isEmpty()) {
            GhosttyQt::Quick::BidiLayout bidi;
            bidi.update(GhosttyQt::Quick::frameWithPreedit(m_frame, preedit));
            caret = bidi.visualCell(preedit.caret);
        }
        return QRectF(m_padding + caret.x() * m_cellWidth, m_padding + caret.y() * m_cellHeight, m_cellWidth, m_cellHeight);
    }
    case Qt::ImFont:
        return m_font;
    case Qt::ImHints:
        return int(Qt::ImhNoPredictiveText | Qt::ImhNoAutoUppercase);
    case Qt::ImSurroundingText:
        return QString();
    case Qt::ImCursorPosition:
    case Qt::ImAnchorPosition:
        return 0;
    default:
        return QQuickItem::inputMethodQuery(query);
    }
}

QPoint TerminalView::cellAt(QPointF position) const
{
    return m_bidi.logicalCell({qBound(0, int((position.x() - m_padding) / m_cellWidth), qMax(0, m_frame.columns - 1)),
                               qBound(0, int((position.y() - m_padding) / m_cellHeight), qMax(0, int(m_frame.rows.size()) - 1))});
}

void TerminalView::updateSelection(GhosttyQt::SelectionEvent::Type type, QPointF position, quint64 timestamp)
{
    if (!m_session || !m_session->terminal()) {
        return;
    }

    if (m_selectionRectangle && m_bidirectionalText) {
        const QPoint visual = m_bidi.visualCell(cellAt(position));
        if (type == GhosttyQt::SelectionEvent::Type::Press) {
            m_rectangleAnchor = visual;
            m_rectangleAnchorOffset = m_frame.scrollOffset;
        }
        const qint64 anchor = m_rectangleAnchorOffset + m_rectangleAnchor.y();
        const qint64 endpoint = qint64(m_frame.scrollOffset) + visual.y();
        const qint64 first = qMin(anchor, endpoint);
        const qint64 last = qMax(anchor, endpoint);
        const int left = qMin(m_rectangleAnchor.x(), visual.x());
        const int right = qMax(m_rectangleAnchor.x(), visual.x());
        QRegion logical;
        for (qint64 screenRow = qMax(first, qint64(m_frame.scrollOffset)); screenRow <= qMin(last, qint64(m_frame.scrollOffset + m_frame.rows.size() - 1));
             ++screenRow)
            for (int x = left; x <= right; ++x) {
                const QPoint cell = glyphStart(m_bidi.logicalCell({x, int(screenRow - m_frame.scrollOffset)}));
                const int width = qMax(1, int(m_frame.rows[cell.y()][cell.x()].width));
                logical += QRect(cell, QSize(width, 1));
            }
        m_session->terminal()->selectCells(logical, first, last);
        m_selectionPointer = position;
        const qreal bottom = m_padding + m_frame.rows.size() * m_cellHeight;
        const int scroll = position.y() < m_padding ? -1 : position.y() >= bottom ? 1 : 0;
        if (scroll && type != GhosttyQt::SelectionEvent::Type::Release) {
            m_session->terminal()->scroll(scroll);
            if (!m_selectionScrollTimer.isActive())
                m_selectionScrollTimer.start(30);
        } else {
            m_selectionScrollTimer.stop();
        }
        return;
    }

    m_selectionPointer = position;
    const qreal dpr{window() ? window()->effectiveDevicePixelRatio() : 1};
    const QPoint logicalCell = cellAt(position);
    const QPoint visualCell = m_bidi.visualCell(logicalCell);
    QPointF logicalPosition = position - QPointF(m_padding, m_padding);
    qreal fraction = logicalPosition.x() / m_cellWidth - visualCell.x();
    const auto &levels = m_bidi.rows().value(logicalCell.y()).levels;
    if (logicalCell.x() >= 0 && logicalCell.x() < levels.size() && (levels[logicalCell.x()] & 1))
        fraction = 1 - fraction;
    // Keep a pointer exactly on the RTL cell edge inside that cell's last
    // physical pixel rather than wrapping the native selection fraction to zero.
    fraction = qBound(qreal(0), fraction, qreal(1) - qreal(0.5) / (m_cellWidth * dpr));
    logicalPosition.setX((logicalCell.x() + fraction) * m_cellWidth);
    m_session->terminal()->selectionGesture({
        .type = type,
        .cell = logicalCell,
        .position = logicalPosition * dpr,
        .timestamp = timestamp,
        .repeatInterval = qGuiApp->styleHints()->mouseDoubleClickInterval(),
        .repeatDistance = qRound(qGuiApp->styleHints()->mouseDoubleClickDistance() * dpr),
        .rectangle = m_selectionRectangle,
    });

    if (m_session->terminal()->selectionAutoscroll()) {
        const qreal bottom{m_padding + m_frame.rows.size() * m_cellHeight};
        const qreal distance{qMax(m_padding - position.y(), position.y() - bottom)};
        const int acceleration{qBound(1, int(distance / m_cellHeight) + 1, 10)};
        // Ghostty scrolls one row per tick, so vary the tick rate to accelerate selection scrolling.
        m_selectionScrollTimer.setInterval(30 / acceleration);

        if (!m_selectionScrollTimer.isActive())
            m_selectionScrollTimer.start();
    } else {
        m_selectionScrollTimer.stop();
    }
}

void TerminalView::scrollSelection()
{
    updateSelection(GhosttyQt::SelectionEvent::Type::Autoscroll, m_selectionPointer);
}

void TerminalView::sendMouse(GhosttyQt::MouseEvent event)
{
    if (!m_session || !m_session->terminal())
        return;

    const qreal dpr{window() ? window()->effectiveDevicePixelRatio() : 1};
    const QPoint logical = cellAt(event.position);
    const QPoint visual = m_bidi.visualCell(logical);
    event.position.rx() += (logical.x() - visual.x()) * m_cellWidth;
    event.position = (event.position - QPointF(m_padding, m_padding)) * dpr;
    event.cellSize = QSizeF(m_cellWidth * dpr, m_cellHeight * dpr);
    event.size = QSizeF(m_frame.columns * m_cellWidth * dpr, m_frame.rows.size() * m_cellHeight * dpr);
    m_session->send(m_session->terminal()->encodeMouse(event));
}

bool TerminalView::reportMouse(QMouseEvent *event, bool release, bool motion)
{
    if (!m_session || !m_session->terminal() || m_session->isReadOnly() || event->modifiers().testFlag(Qt::ShiftModifier)) {
        return false;
    }

    if (!m_session->terminal()->mouseTracking()) {
        return false;
    }

    Qt::MouseButton button{event->button()};

    if (motion) {
        if (event->buttons().testFlag(Qt::LeftButton)) {
            button = Qt::LeftButton;
        } else if (event->buttons().testFlag(Qt::MiddleButton)) {
            button = Qt::MiddleButton;
        } else if (event->buttons().testFlag(Qt::RightButton)) {
            button = Qt::RightButton;
        }
    }

    sendMouse({
        .action = motion ? GhosttyQt::MouseEvent::Action::Move
            : release    ? GhosttyQt::MouseEvent::Action::Release
                         : GhosttyQt::MouseEvent::Action::Press,
        .button = button,
        .buttons = event->buttons(),
        .modifiers = event->modifiers(),
        .position = event->position(),
    });

    return true;
}

void TerminalView::mousePressEvent(QMouseEvent *event)
{
    setKeyboardSelectionMode(false);
    forceActiveFocus();
    m_selecting = false;
    m_selectionRectangle = event->modifiers().testFlag(Qt::ControlModifier) && event->modifiers().testFlag(Qt::AltModifier);
    m_selectionScrollTimer.stop();

    if (event->button() == Qt::LeftButton) {
        m_mousePressConsumed = false;
    }

    m_dragPending = false;
    updateHoveredLink(event->position(), event->modifiers());
    const QPoint pressedCell{cellAt(event->position())};

    if (event->button() == Qt::LeftButton && event->modifiers().testFlag(Qt::AltModifier) && !m_selectionRectangle && !m_hoveredLink.isEmpty()) {
        m_session->terminal()->cancelSelectionGesture();
        m_mousePressConsumed = true;
        auto *mime = new QMimeData;
        mime->setUrls({m_hoveredLink});

        if (!m_hoveredLink.isLocalFile()) {
            mime->setText(m_hoveredLink.toString());
        }

        event->accept();
        copyDrag(this, mime);

        return;
    }

    if (event->button() == Qt::LeftButton && event->modifiers().testFlag(Qt::ControlModifier) && !m_selectionRectangle && hasSelection()
        && pressedCell.y() < m_frame.rows.size() && pressedCell.x() < m_frame.rows.at(pressedCell.y()).size()
        && (m_frame.rows.at(pressedCell.y()).at(pressedCell.x()).flags & GhosttyQt::Selected)) {
        m_session->terminal()->cancelSelectionGesture();
        m_dragPending = true;
        m_mousePressConsumed = true;
        m_dragStart = event->position();
        event->accept();

        return;
    }

    if (event->button() == Qt::LeftButton && event->modifiers().testFlag(Qt::ControlModifier) && !m_selectionRectangle && !m_hoveredLink.isEmpty()) {
        if (m_session && m_session->terminal())
            m_session->terminal()->cancelSelectionGesture();

        m_mousePressConsumed = true;
        openLink();
        event->accept();

        return;
    }

    if (reportMouse(event, false, false)) {
        m_session->terminal()->cancelSelectionGesture();
        event->accept();

        return;
    }

    if (event->button() == Qt::LeftButton) {
        m_selecting = true;
        updateSelection(GhosttyQt::SelectionEvent::Type::Press, event->position(), event->timestamp());
    } else {
        if (m_session && m_session->terminal())
            m_session->terminal()->cancelSelectionGesture();

        if (event->button() == Qt::MiddleButton) {
            pasteSelection();
        } else if (event->button() == Qt::RightButton) {
            Q_EMIT contextMenuRequested(event->position());
        }
    }

    event->accept();
}

void TerminalView::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragPending) {
        event->accept();

        if ((event->position() - m_dragStart).manhattanLength() < qGuiApp->styleHints()->startDragDistance()) {
            return;
        }

        m_dragPending = false;
        const QString selected{selectedText()};

        if (!selected.isEmpty()) {
            auto *mime = new QMimeData;
            mime->setText(selected);
            copyDrag(this, mime);
        }

        return;
    }

    if (m_mousePressConsumed && event->buttons().testFlag(Qt::LeftButton)) {
        event->accept();

        return;
    }

    if (m_selecting) {
        updateSelection(GhosttyQt::SelectionEvent::Type::Drag, event->position());
        event->accept();

        return;
    }

    if (reportMouse(event, false, true)) {
        event->accept();

        return;
    }

    event->accept();
}

void TerminalView::mouseReleaseEvent(QMouseEvent *event)
{
    m_selectionScrollTimer.stop();

    if (m_dragPending && event->button() == Qt::LeftButton) {
        m_dragPending = false;
        m_mousePressConsumed = false;

        if (event->modifiers().testFlag(Qt::ControlModifier) && !m_hoveredLink.isEmpty()) {
            openLink();
        }

        event->accept();

        return;
    }

    if (m_mousePressConsumed && event->button() == Qt::LeftButton) {
        m_mousePressConsumed = false;
        event->accept();

        return;
    }

    if (m_selecting && m_session) {
        updateSelection(GhosttyQt::SelectionEvent::Type::Release, event->position());
        m_selecting = false;

        if (event->button() == Qt::LeftButton && !m_requireCtrlForLinks && !m_hoveredLink.isEmpty() && !hasSelection()
            && linkActivationAllowed(event->modifiers()) && m_hoveredLink == linkAtPosition(event->position())) {
            openLink();
        }

        auto *clipboard = QGuiApplication::clipboard();
        const bool primarySelection{clipboard->supportsSelection()};

        if (primarySelection || m_copyOnSelect) {
            const QString text{selectedText()};

            if (!text.isEmpty()) {
                if (primarySelection) {
                    clipboard->setText(text, QClipboard::Selection);
                }

                if (m_copyOnSelect) {
                    clipboard->setText(text);
                }
            }
        }
    } else if (reportMouse(event, true, false)) {
        event->accept();

        return;
    }

    event->accept();
}

void TerminalView::mouseDoubleClickEvent(QMouseEvent *event)
{
    // Qt Quick already delivered this click as a press. Ghostty counts presses.
    event->accept();
}

void TerminalView::mouseUngrabEvent()
{
    if (m_selecting && m_session && m_session->terminal())
        m_session->terminal()->cancelSelectionGesture();

    m_selecting = false;
    m_selectionScrollTimer.stop();
    m_dragPending = false;
    QQuickItem::mouseUngrabEvent();
}

void TerminalView::hoverMoveEvent(QHoverEvent *event)
{
    updateHoveredLink(event->position(), event->modifiers());

    if (m_session && m_session->terminal() && !m_session->isReadOnly() && !event->modifiers().testFlag(Qt::ShiftModifier)
        && m_session->terminal()->mouseTracking()) {
        // Don't send mouse motion updates on frame changes when the pointer pos didn't
        // change. Sending updates would not be wrong per-se, but other terminals tend
        // to not do this, and it confuses some clients who can't handle complex mixed
        // keyboard and motion input.
        const auto &point = event->point(0);

        if (point.globalPosition() != point.globalLastPosition()) {
            sendMouse({.action = GhosttyQt::MouseEvent::Action::Move, .modifiers = event->modifiers(), .position = event->position()});
        }

        event->accept();
    } else {
        event->ignore();
    }
}

void TerminalView::hoverLeaveEvent(QHoverEvent *event)
{
    updateHoveredLink({}, Qt::NoModifier);
    event->accept();
}

int TerminalView::wheelSteps(QPoint pixelDelta, QPoint angleDelta, int stepsPerNotch)
{
    m_zoomWheelRemainder = 0;

    if (!angleDelta.isNull()) {
        m_wheelPixelRemainder = 0;

        // History rows and protocol wheel reports have different step sizes.
        if (m_wheelStepsPerNotch != stepsPerNotch) {
            m_wheelAngleRemainder = 0;
            m_wheelStepsPerNotch = stepsPerNotch;
        }

        return accumulateWheelSteps(qreal(angleDelta.y()) * stepsPerNotch, 120, m_wheelAngleRemainder);
    }

    if (!pixelDelta.isNull()) {
        m_wheelAngleRemainder = 0;

        return accumulateWheelSteps(pixelDelta.y(), m_cellHeight, m_wheelPixelRemainder);
    }

    return 0;
}

void TerminalView::wheelEvent(QWheelEvent *event)
{
    if (!m_session || !m_session->terminal()) {
        return;
    }

    if (event->modifiers().testFlag(Qt::ControlModifier)) {
        m_wheelAngleRemainder = 0;
        m_wheelPixelRemainder = 0;
        const int steps{accumulateWheelSteps(event->angleDelta().y(), 120, m_zoomWheelRemainder)};

        if (steps != 0) {
            m_session->setZoomOffset(qBound(6.0 - fontPointSize(), m_zoom + steps, 72.0 - fontPointSize()));
        }

        event->accept();

        return;
    }

    const bool reportsMouse{!m_session->isReadOnly() && m_session->terminal()->mouseTracking() && !event->modifiers().testFlag(Qt::ShiftModifier)};

    if (reportsMouse) {
        const int steps{wheelSteps(event->pixelDelta(), event->angleDelta(), 1)};

        for (int i{0}; i < qAbs(steps); ++i) {
            sendMouse({.wheel = steps > 0 ? GhosttyQt::MouseEvent::WheelDirection::Up : GhosttyQt::MouseEvent::WheelDirection::Down,
                       .modifiers = event->modifiers(),
                       .position = event->position()});
        }
    } else if (!m_session->isReadOnly() && m_session->terminal()->isModeEnabled(GhosttyQt::Mode::AlternateScreen)
               && m_session->terminal()->isModeEnabled(GhosttyQt::Mode::AlternateScroll)) {
        const int steps{wheelSteps(event->pixelDelta(), event->angleDelta(), QGuiApplication::styleHints()->wheelScrollLines())};

        for (int i{0}; i < qAbs(steps); ++i) {
            QKeyEvent key(QEvent::KeyPress, steps > 0 ? Qt::Key_Up : Qt::Key_Down, Qt::NoModifier);
            encodeKey(&key);
        }
    } else {
        scrollHistory(event->pixelDelta(), event->angleDelta(), event->modifiers());
    }

    event->accept();
}

void TerminalView::focusInEvent(QFocusEvent *event)
{
    QQuickItem::focusInEvent(event);

    if (m_session && m_session->terminal() && m_session->terminal()->isModeEnabled(GhosttyQt::Mode::FocusEvents)) {
        m_session->send(m_session->terminal()->encodeFocus(true));
    }

    resetCursorBlink();
}

void TerminalView::focusOutEvent(QFocusEvent *event)
{
    QQuickItem::focusOutEvent(event);
    m_pressedKeys.clear();
    resetPreedit();

    if (m_session && m_session->terminal())
        m_session->terminal()->cancelSelectionGesture();

    m_selecting = false;
    m_selectionRectangle = false;
    m_selectionScrollTimer.stop();
    m_dragPending = false;

    if (m_session && m_session->terminal() && m_session->terminal()->isModeEnabled(GhosttyQt::Mode::FocusEvents)) {
        m_session->send(m_session->terminal()->encodeFocus(false));
    }

    resetCursorBlink();
}
}
