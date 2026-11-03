// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "Session.h"
#include "kwiterminal_export.h"
#include <BidiLayout.h>
#include <QColor>
#include <QFont>
#include <QInputMethodEvent>
#include <QList>
#include <QMetaObject>
#include <QPoint>
#include <QPointF>
#include <QPointer>
#include <QQuickItem>
#include <QRectF>
#include <QRegion>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <QtQmlIntegration/qqmlintegration.h>
#include <optional>

class QScreen;
class KDirWatch;
class KModifierKeyInfo;

namespace Kwi
{
class TerminalSearch;
struct TerminalColors;

class KWI_TERMINAL_EXPORT TerminalView : public QQuickItem
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(Kwi::Session *session READ session WRITE setSession NOTIFY sessionChanged)
    Q_PROPERTY(QString fontFamily READ fontFamily WRITE setFontFamily NOTIFY fontChanged)
    Q_PROPERTY(qreal fontPointSize READ fontPointSize WRITE setFontPointSize NOTIFY fontChanged)
    Q_PROPERTY(QString colorScheme READ colorScheme WRITE setColorScheme NOTIFY appearanceChanged)
    Q_PROPERTY(QColor foregroundColor READ foregroundColor NOTIFY colorsChanged)
    Q_PROPERTY(QColor backgroundColor READ backgroundColor NOTIFY colorsChanged)
    Q_PROPERTY(qreal backgroundOpacity READ backgroundOpacity WRITE setBackgroundOpacity NOTIFY backgroundOpacityChanged)
    Q_PROPERTY(QString cursorStyle READ cursorStyle WRITE setCursorStyle NOTIFY appearanceChanged)
    Q_PROPERTY(bool cursorBlinking READ cursorBlinking WRITE setCursorBlinking NOTIFY appearanceChanged)
    Q_PROPERTY(bool confirmMultilinePaste MEMBER m_confirmMultilinePaste NOTIFY confirmMultilinePasteChanged)
    Q_PROPERTY(bool copyOnSelect MEMBER m_copyOnSelect NOTIFY copyOnSelectChanged)
    Q_PROPERTY(bool requireCtrlForLinks READ requireCtrlForLinks WRITE setRequireCtrlForLinks NOTIFY requireCtrlForLinksChanged)

    Q_PROPERTY(bool bidirectionalText READ bidirectionalText WRITE setBidirectionalText NOTIFY bidirectionalTextChanged)
    Q_PROPERTY(bool optionAsAlt READ optionAsAlt WRITE setOptionAsAlt NOTIFY optionAsAltChanged)

    Q_PROPERTY(QString selectedText READ selectedText NOTIFY selectionChanged)
    Q_PROPERTY(bool hasSelection READ hasSelection NOTIFY selectionChanged)
    Q_PROPERTY(QString searchStatus READ searchStatus NOTIFY searchStatusChanged)
    Q_PROPERTY(bool searchBusy READ isSearchBusy NOTIFY searchStatusChanged)
    Q_PROPERTY(qsizetype searchMatchCount READ searchMatchCount NOTIFY searchStatusChanged)

    Q_PROPERTY(qreal scrollPosition READ scrollPosition NOTIFY scrollChanged)
    Q_PROPERTY(qreal scrollSize READ scrollSize NOTIFY scrollChanged)
    Q_PROPERTY(qreal scrollStepSize READ scrollStepSize NOTIFY scrollChanged)
    Q_PROPERTY(QUrl hoveredLink READ hoveredLink NOTIFY hoveredLinkChanged)

    Q_PROPERTY(bool keyboardSelectionMode READ keyboardSelectionMode WRITE setKeyboardSelectionMode NOTIFY keyboardSelectionChanged)
    Q_PROPERTY(QRectF selectionCursorRectangle READ selectionCursorRectangle NOTIFY keyboardSelectionChanged)

public:
    explicit TerminalView(QQuickItem *parent = nullptr);
    ~TerminalView() override;

    [[nodiscard]] Session *session() const;
    void setSession(Session *newSession);

    [[nodiscard]] QString fontFamily() const;
    void setFontFamily(const QString &family);
    [[nodiscard]] qreal fontPointSize() const;
    void setFontPointSize(qreal size);
    [[nodiscard]] QString colorScheme() const;
    void setColorScheme(const QString &scheme);
    // Effective default colors of the displayed frame, including OSC overrides.
    [[nodiscard]] QColor foregroundColor() const;
    [[nodiscard]] QColor backgroundColor() const;
    [[nodiscard]] qreal backgroundOpacity() const;
    void setBackgroundOpacity(qreal opacity);
    [[nodiscard]] QString cursorStyle() const;
    void setCursorStyle(const QString &style);
    [[nodiscard]] bool cursorBlinking() const;
    void setCursorBlinking(bool blinking);
    [[nodiscard]] bool requireCtrlForLinks() const;
    void setRequireCtrlForLinks(bool required);

    [[nodiscard]] bool bidirectionalText() const;
    void setBidirectionalText(bool enabled);

    [[nodiscard]] bool optionAsAlt() const;
    void setOptionAsAlt(bool enabled);

    [[nodiscard]] QString selectedText() const;
    [[nodiscard]] bool hasSelection() const;
    [[nodiscard]] QString searchStatus() const;
    [[nodiscard]] bool isSearchBusy() const;
    // -1 when no search result is available.
    [[nodiscard]] qsizetype searchMatchCount() const;

    [[nodiscard]] qreal scrollPosition() const;
    [[nodiscard]] qreal scrollSize() const;
    [[nodiscard]] qreal scrollStepSize() const;
    [[nodiscard]] QUrl hoveredLink() const;

    [[nodiscard]] bool keyboardSelectionMode() const;
    void setKeyboardSelectionMode(bool enabled);
    [[nodiscard]] QRectF selectionCursorRectangle() const;
    Q_INVOKABLE void toggleSelectionMode();

    [[nodiscard]] Q_INVOKABLE QVariantList links() const;
    Q_INVOKABLE void copyLink(const QUrl &url);

    Q_INVOKABLE void copy();
    Q_INVOKABLE bool copyCommandOutput();
    Q_INVOKABLE void paste();
    Q_INVOKABLE void pasteSelection();
    /** Applies the view's confirmation policy to a host-supplied paste. */
    Q_INVOKABLE void requestPaste(const QString &text);
    Q_INVOKABLE void selectAll();

    Q_INVOKABLE void find(const QString &text, bool forward = true);
    Q_INVOKABLE void clearSearch();

    Q_INVOKABLE void zoomIn();
    Q_INVOKABLE void zoomOut();
    Q_INVOKABLE void resetZoom();

    Q_INVOKABLE void scrollTo(qreal position);
    Q_INVOKABLE void scrollHistory(QPoint pixelDelta, QPoint angleDelta, Qt::KeyboardModifiers modifiers = Qt::NoModifier);

    [[nodiscard]] QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;

Q_SIGNALS:
    void sessionChanged();
    void fontChanged();
    void appearanceChanged();
    void colorsChanged();
    void backgroundOpacityChanged();
    void confirmMultilinePasteChanged();
    void copyOnSelectChanged();
    void requireCtrlForLinksChanged();
    void optionAsAltChanged();
    void bidirectionalTextChanged();
    void selectionChanged();
    void searchStatusChanged();
    void scrollChanged();
    void multilinePasteRequested(Kwi::Session *origin, const QString &text);
    void contextMenuRequested(const QPointF &position);
    void hoveredLinkChanged();
    void linkActivated(const QUrl &url);
    void keyboardSelectionChanged();
    void focusRequested();

protected:
    void itemChange(ItemChange change, const ItemChangeData &data) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void updatePolish() override;
    QSGNode *updatePaintNode(QSGNode *node, UpdatePaintNodeData *) override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;

    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void inputMethodEvent(QInputMethodEvent *event) override;

    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void mouseUngrabEvent() override;
    void hoverMoveEvent(QHoverEvent *event) override;
    void hoverLeaveEvent(QHoverEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;

private:
    static constexpr qreal Padding{1};
    bool m_optionAsAlt{false};

    void changeSession(Session *newSession);
    int wheelSteps(QPoint pixelDelta, QPoint angleDelta, int stepsPerNotch);

    friend class TerminalAccessible;
    static void installAccessibility();
    void updateAccessibility();

    void refresh();
    void reloadColors();
    void applyColors(const TerminalColors &colors);
    void updateMetrics();
    void watchScreen(QScreen *screen);
    void resizeTerminal();
    void updateCursorDefaults();
    void updateBlinkTimers();
    void resetCursorBlink();

    void resetPreedit();
    void encodeKey(QKeyEvent *event);
    void showContextMenu();
    bool handleSelectionKey(QKeyEvent *event);
    [[nodiscard]] QPoint glyphStart(QPoint cell) const;
    [[nodiscard]] QPoint cellAt(QPointF position) const;
    [[nodiscard]] QUrl linkAtPosition(QPointF position, QRegion *cells = nullptr) const;
    bool reportMouse(QMouseEvent *event, bool release, bool motion);
    void sendMouse(GhosttyQt::MouseEvent event);
    void updateSelection(GhosttyQt::SelectionEvent::Type type, QPointF position, quint64 timestamp = 0);
    void updateHoveredLink(std::optional<QPointF> position, Qt::KeyboardModifiers modifiers);
    [[nodiscard]] bool linkActivationAllowed(Qt::KeyboardModifiers modifiers) const;
    void openLink();
    void scrollSelection();

    QPointer<Session> m_session;
    GhosttyQt::Frame m_frame;
    GhosttyQt::Quick::BidiLayout m_bidi;
    bool m_bidirectionalText{true};
    [[nodiscard]] QRectF logicalCellRectangle(QPoint cell) const;
    quint64 m_frameRevision{0};
    QFont m_font;
    QString m_fontFamily;
    QString m_colorScheme{QStringLiteral("system")};
    QString m_cursorStyle{QStringLiteral("block")};

    TerminalSearch *m_search;
    QRegion m_searchMatches;
    bool m_resettingPreedit{false};
    QString m_preedit;
    QList<QInputMethodEvent::Attribute> m_preeditAttributes;

    QTimer m_cursorBlinkTimer;
    QTimer m_textBlinkTimer;
    QTimer m_selectionScrollTimer;

    KModifierKeyInfo *m_modifierKeyInfo;
    KDirWatch *m_paletteWatch;
    QTimer m_paletteReloadTimer;
    QString m_palettePath;

    QMetaObject::Connection m_screenConnection;
    QMetaObject::Connection m_dpiConnection;

    QUrl m_hoveredLink;
    QRegion m_highlightedLink;
    std::optional<QPointF> m_hoverPosition;

    qreal m_cellWidth{10};
    qreal m_cellHeight{20};
    qreal m_padding{Padding};
    qreal m_zoom{0};
    qreal m_pointSize{0};
    qreal m_backgroundOpacity{1};

    qreal m_wheelAngleRemainder{0};
    qreal m_wheelPixelRemainder{0};
    qreal m_zoomWheelRemainder{0};
    int m_wheelStepsPerNotch{0};
    QSet<quint64> m_pressedKeys;

    bool m_cursorBlinking{false};
    bool m_cursorBlinkVisible{true};
    bool m_textBlinkVisible{true};
    bool m_hasBlinkingText{false};
    bool m_publishingScroll{false};

    bool m_selecting{false};
    bool m_hadSelection{false};
    bool m_selectionTextDirty{false};
    bool m_selectionRectangle{false};

    // Track consumed presses across focus/grab changes during native drags.
    bool m_mousePressConsumed{false};
    bool m_dragPending{false};
    QPointF m_dragStart;

    bool m_confirmMultilinePaste{true};
    bool m_copyOnSelect{false};
    bool m_requireCtrlForLinks{true};

    bool m_keyboardSelectionMode{false};
    bool m_traceMarkerSeen{false};
    QPoint m_keyboardCursor;
    QPoint m_keyboardAnchor;
    QPoint m_rectangleAnchor;
    qint64 m_rectangleAnchorOffset{0};
    QPointF m_selectionPointer;
};
}
