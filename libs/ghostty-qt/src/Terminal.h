// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "Frame.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QClipboard>
#include <QColor>
#include <QObject>
#include <QPoint>
#include <QPointF>
#include <QRegion>
#include <QSize>
#include <QSizeF>
#include <QString>
#include <QVector>
#include <Qt>
#include <functional>
#include <memory>
#include <optional>

class QKeyEvent;
class QMimeData;

namespace GhosttyQt
{
class Search;
class HistoryReader;

enum class Mode {
    BracketedPaste,
    FocusEvents,
    AlternateScreen,
    AlternateScroll,
    SynchronizedOutput,
};

struct KeyboardLocks {
    bool capsLock{false};
    bool numLock{false};
};

struct SelectionEvent {
    enum class Type {
        Press,
        Release,
        Drag,
        Autoscroll,
    };

    Type type{Type::Press};
    QPoint cell;
    QPointF position; // Physical pixels relative to the terminal grid.
    quint64 timestamp{0}; // Monotonic input timestamp in milliseconds.
    int repeatInterval{0};
    int repeatDistance{0};
    bool rectangle{false};
};

enum class SelectionAdjustment {
    Left,
    Right,
    Up,
    Down,
    Start,
    End,
    PageUp,
    PageDown,
    StartOfLine,
    EndOfLine,
};

struct Progress {
    enum class State {
        None,
        Normal,
        Error,
        Indeterminate,
        Paused,
    };

    State state{State::None};
    int percent{-1}; // Unknown, or a value from 0 to 100.
};

struct Notification {
    QString title;
    QString body;
};

struct MouseEvent {
    enum class Action {
        Press,
        Release,
        Move
    };

    enum class WheelDirection {
        None,
        Up,
        Down,
        Left,
        Right
    };

    Action action{Action::Press};
    Qt::MouseButton button{Qt::NoButton};
    Qt::MouseButtons buttons{Qt::NoButton};
    WheelDirection wheel{WheelDirection::None};
    Qt::KeyboardModifiers modifiers{Qt::NoModifier};
    QPointF position{};
    QSizeF cellSize{};
    QSizeF size{};
};

class Terminal final : public QObject
{
    Q_OBJECT

public:
    enum class Error {
        None,
        AllocationFailed,
        RenderStateFailed,
    };
    Q_ENUM(Error)

    static constexpr qint64 UnlimitedScrollback{-1};

    explicit Terminal(int columns = 80, int rows = 24, qint64 scrollbackLines = UnlimitedScrollback, QObject *parent = nullptr);
    ~Terminal() override;

    [[nodiscard]] bool isValid() const;
    [[nodiscard]] Error error() const;

    bool feed(QByteArrayView bytes);
    bool resize(int columns, int rows, QSize cellPixels = {});
    bool snapshot(Frame &frame);

    void setScrollbackLines(qint64 lines);
    void setColors(QColor foreground, QColor background, const QVector<QRgb> &palette, QColor boldForeground = {});
    void setDecorations(QColor cursor, QColor selectionForeground, QColor selectionBackground);
    void setDefaultCursor(CursorStyle style, bool blinking);
    void setTerminfoName(QByteArrayView name);
    void setApplicationIdentity(QByteArrayView identity);

    // Runs synchronously during feed(). Null contents clears the destination.
    // Return false when the destination is unsupported. The handler owns contents.
    using ClipboardWriteHandler = std::function<bool(std::unique_ptr<QMimeData>, QClipboard::Mode)>;
    void setClipboardWriteHandler(ClipboardWriteHandler handler);

    void scroll(qint64 rows);
    void scrollTo(quint64 offset);
    void scrollBottom();
    void scrollPrompt(int delta);

    bool select(QPoint first, QPoint last, bool rectangle = false);
    bool
    selectCells(const QRegion &cells, qint64 firstScreenRow = -1, qint64 lastScreenRow = -1); // Discontiguous logical cells, for visual rectangular selection.
    bool selectionGesture(const SelectionEvent &event);
    [[nodiscard]] bool selectionAutoscroll() const;
    void cancelSelectionGesture();
    [[nodiscard]] std::optional<QPoint> adjustSelection(SelectionAdjustment adjustment, bool extendSelection);
    [[nodiscard]] bool hasSelection() const;
    void clearSelection();
    bool selectAll();
    [[nodiscard]] std::unique_ptr<Search> createSearch();
    [[nodiscard]] std::unique_ptr<HistoryReader> createHistoryReader();

    [[nodiscard]] QString selectionText() const;
    [[nodiscard]] QString historyText() const;
    [[nodiscard]] QString commandOutput() const;
    [[nodiscard]] QString linkAt(QPoint cell) const;
    [[nodiscard]] QString workingDirectoryUrl() const;

    void clearScrollback();
    void clearProcessMetadata(); // Retains screen, modes and history for a new process.
    void reset();

    [[nodiscard]] Progress progress() const;
    [[nodiscard]] std::optional<Notification> takeNotification();

    [[nodiscard]] QByteArray encodeKey(const QKeyEvent &event, KeyboardLocks locks = {}, bool optionAsAlt = false);
    [[nodiscard]] QByteArray encodeMouse(const MouseEvent &event);
    [[nodiscard]] QByteArray encodeFocus(bool focused) const;
    [[nodiscard]] QByteArray encodePaste(QByteArrayView bytes) const;

    [[nodiscard]] bool isModeEnabled(Mode mode) const;
    [[nodiscard]] Qt::CursorShape mouseCursorShape() const;
    [[nodiscard]] bool mouseTracking() const;

    [[nodiscard]] static QString version();

Q_SIGNALS:
    void writeRequested(const QByteArray &bytes);
    void titleChanged(const QString &title);
    void workingDirectoryChanged();
    void bell();
    void frameChanged();
    void geometryChanged();
    void errorOccurred(GhosttyQt::Terminal::Error error);

private:
    friend class Search;
    bool selectSearchMatch(const Search &search, bool forward);
    bool applySearchSelection(const Search &search);
    std::optional<QRegion> searchViewportMatches(const Search &search) const;
    void endSynchronizedOutput();

    class Private;
    std::unique_ptr<Private> d;
};
}
