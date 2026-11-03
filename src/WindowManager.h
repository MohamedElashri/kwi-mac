// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include "settings/LaunchSettings.h"

#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantMap>
#include <QtQmlIntegration/qqmlintegration.h>
#include <memory>

class QEventLoopLocker;
class QQuickWindow;

Q_MOC_INCLUDE("SessionModel.h")
Q_MOC_INCLUDE("TerminalSession.h")

namespace Kwi
{
class TerminalSettings;
class TerminalPresets;
class AppShortcuts;
class TerminalPaletteModel;
class SessionModel;
class TerminalSession;

struct LaunchOptions {
    LaunchRequest launch;
    QString presetName{};
    QString title{};
    bool hold{false};
    bool newTab{false};
    bool fullscreen{false};
};

class WindowManager final : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Window managers are provided by the application.")
    Q_PROPERTY(Kwi::TerminalSession *draggedSession READ draggedSession NOTIFY draggedSessionChanged)

public:
    WindowManager(TerminalSettings *settings, TerminalPresets *presets, AppShortcuts *shortcuts, TerminalPaletteModel *palettes, QObject *parent = nullptr);
    ~WindowManager() override;

    QQuickWindow *launch(const LaunchOptions &options);
    [[nodiscard]] int count() const;
    [[nodiscard]] SessionModel *sessions(const QQuickWindow *window) const;

    [[nodiscard]] TerminalSession *draggedSession() const;
    [[nodiscard]] Q_INVOKABLE QString beginTabDrag(Kwi::SessionModel *source, Kwi::TerminalSession *session);
    [[nodiscard]] Q_INVOKABLE bool acceptsTabDrag(const QString &token) const;
    Q_INVOKABLE bool dropTab(const QString &token, Kwi::SessionModel *destination, int insertionIndex);
    Q_INVOKABLE void finishTabDrag(const QString &token, int action);

Q_SIGNALS:
    void draggedSessionChanged();

private:
    bool eventFilter(QObject *watched, QEvent *event) override;
    class WindowContext;
    [[nodiscard]] WindowContext *createWindow();
    [[nodiscard]] WindowContext *contextForSessions(const SessionModel *sessionModel) const;
    void activate(WindowContext *context, bool fullscreen = false);
    void detach(WindowContext *source, TerminalSession *session);
    void completeTabDrop(const QString &token);
    void clearTabDrag();
    void disposeWindow(WindowContext *context);
    void updateProgress();
    void requestQuit(WindowContext *context, bool force);
    void cancelQuit(const SessionModel *source);
    [[nodiscard]] QList<QPointer<TerminalSession>> sessionSnapshot() const;

    TerminalSettings *m_settings;
    TerminalPresets *m_presets;
    AppShortcuts *m_shortcuts;
    TerminalPaletteModel *m_palettes;
    QList<WindowContext *> m_windows{};
    QPointer<QQuickWindow> m_activeWindow{};
    QVariantMap m_lastProgress{};

    QString m_dragToken{};
    QPointer<SessionModel> m_dragSource{};
    QPointer<TerminalSession> m_draggedSession{};
    std::unique_ptr<QEventLoopLocker> m_dragQuitLock;
    QPointer<SessionModel> m_dropDestination{};
    QPointer<TerminalSession> m_dropBeforeSession{};
    bool m_dropAtEnd{false};
    bool m_dropRecorded{false};
    bool m_dragFinishing{false};

    quint64 m_quitRequestSerial{0};
    QPointer<SessionModel> m_quitRequester{};
    bool m_quitting{false};
};
}
