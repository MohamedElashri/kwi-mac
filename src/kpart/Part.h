// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include "settings/LaunchSettings.h"

#include <KParts/ReadOnlyPart>
#include <QDBusUnixFileDescriptor>
#include <QHash>
#include <QKeySequence>
#include <QList>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>
#include <kde_terminal_interface.h>
#include <memory>

class KPluginMetaData;
class KXMLGUIClient;
class QAction;
class QEvent;
class QKeyEvent;
class QPoint;
class QTabWidget;
class QUrl;
class QWidget;

namespace Kwi
{
class DesktopIntegration;
class SessionModel;
class TerminalPresets;
class TerminalSession;
class TerminalSettings;
class TerminalWidget;

class Part final : public KParts::ReadOnlyPart, public TerminalInterface
{
    Q_OBJECT
    Q_INTERFACES(TerminalInterface)

public:
    explicit Part(QWidget *parentWidget, QObject *parent, const KPluginMetaData &metadata, const QVariantList &arguments = {});
    ~Part() override;

    void startProgram(const QString &program, const QStringList &arguments) override;
    void showShellInDir(const QString &directory) override;
    void sendInput(const QString &text) override;
    [[nodiscard]] int terminalProcessId() override;
    [[nodiscard]] int foregroundProcessId() override;
    [[nodiscard]] QString foregroundProcessName() override;
    [[nodiscard]] QString currentWorkingDirectory() const override;

    // TerminalInterface keeps KonsolePart's Profile names for drop-in host
    // compatibility. Their values describe Kwi presets.
    [[nodiscard]] QStringList availableProfiles() const override;
    [[nodiscard]] QString currentProfileName() const override;
    bool setCurrentProfile(const QString &name) override;
    [[nodiscard]] QVariant profileProperty(const QString &name) const override;

    Q_INVOKABLE void setContextMenuAdditionalActions(const QList<QAction *> &actions);
    bool openUrl(const QUrl &url) override;

public Q_SLOTS:
    void createSession(const QString &presetName = {}, const QString &directory = {});
    void newTab();

    void showSettings(QWidget *parent = nullptr);

    // Hosts invoke these KonsolePart slot names through the meta-object API.
    // Keep Profile spelling for compatibility. Both dialogs edit Kwi presets.
    void showManageProfilesDialog(QWidget *parent);
    void showEditCurrentProfileDialog(QWidget *parent);

    // Compatibility no-op: Kwi does not support temporary per-session settings.
    // Keep this instance slot for KonsolePart's meta-object API.
    // cppcheck-suppress functionStatic
    void changeSessionSettings(const QString &)
    {
    }

    void openTeletype(int ptyMasterFd, bool runShell = true);

    void setMonitorSilenceEnabled(bool enabled);
    void setMonitorActivityEnabled(bool enabled);
    [[nodiscard]] bool isBlurEnabled();

Q_SIGNALS:
    // Konsole's synchronous host shortcut negotiation requires bool&.
    void overrideShortcut(QKeyEvent *event, bool &override); // clazy:exclude=signal-with-return-value
    void silenceDetected();
    void activityDetected();
    void currentDirectoryChanged(const QString &directory);

protected:
    bool openFile() override;
    bool eventFilter(QObject *object, QEvent *event) override;

private:
    class Page;
    [[nodiscard]] TerminalSession *activeSession() const;
    [[nodiscard]] TerminalWidget *activeWidget() const;
    void addSession(const QString &presetName, const QString &directory, bool start);
    void addSessionWidget(TerminalSession *session);
    void removeSessionWidget(TerminalSession *session);
    void activateSession();
    void startShell(TerminalSession *session, const QString &directory = {});

    void setupActions();
    void updateActions();
    void contextMenu(QPoint position, const QUrl &link);
    void tabContextMenu(QPoint position, TerminalSession *session);
    void renameSession(TerminalSession *session);
    void updateHostWindow();
    void updateHostFocus();

    void saveOutput();
    void showCloseConfirmation(quint64 requestId, const QString &title);

    TerminalSettings *m_settings;
    TerminalPresets *m_presets;
    SessionModel *m_sessions;
    DesktopIntegration *m_desktop;
    QTabWidget *m_tabs;
    std::unique_ptr<KXMLGUIClient> m_terminalClient;

    QHash<TerminalSession *, Page *> m_pages{};
    QHash<TerminalSession *, QDBusUnixFileDescriptor> m_pendingPtys{};
    QHash<TerminalSession *, LaunchRequest> m_launchRequests{};
    QList<QPointer<QAction>> m_additionalActions{};

    QPointer<QObject> m_shortcutOrigin{};
    QKeySequence m_negotiatedShortcut{};
    bool m_syncingTabs{false};
    bool m_destroying{false};
    bool m_embeddingPending{false};
};
}
