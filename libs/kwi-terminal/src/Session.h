// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "SessionEvents_p.h"
#include "kwiterminal_export.h"
#include "process/PtyProcess.h"
#include <QByteArray>
#include <QColor>
#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>
#include <QVector>
#include <QtQmlIntegration/qqmlintegration.h>
#include <Terminal.h>
#include <functional>
#include <memory>

namespace Kwi
{
class SshMetadataResolver;
class TerminalView;
struct TerminalLaunch;

class KWI_TERMINAL_EXPORT Session : public QObject
{
    Q_OBJECT
    QML_NAMED_ELEMENT(Session)
    Q_PROPERTY(QString executable READ executable WRITE setExecutable NOTIFY launchChanged)
    Q_PROPERTY(QStringList arguments READ arguments WRITE setArguments NOTIFY launchChanged)
    Q_PROPERTY(QString initialDirectory READ initialDirectory WRITE setInitialDirectory NOTIFY launchChanged)
    Q_PROPERTY(QVariantMap environmentChanges READ environmentChanges WRITE setEnvironmentChanges NOTIFY launchChanged)
    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(bool active READ isActive NOTIFY activeChanged)
    Q_PROPERTY(bool finished READ isFinished NOTIFY exitStatusChanged)
    Q_PROPERTY(int exitCode READ exitCode NOTIFY exitStatusChanged)
    Q_PROPERTY(QString errorString READ errorString NOTIFY errorChanged)
    Q_PROPERTY(bool hasApplicationTitle READ hasApplicationTitle NOTIFY applicationTitleChanged)
    Q_PROPERTY(QString applicationTitle READ applicationTitle NOTIFY applicationTitleChanged)
    Q_PROPERTY(QString launchedProgramName READ launchedProgramName NOTIFY launchedProgramNameChanged)
    Q_PROPERTY(QString workingDirectory READ workingDirectory NOTIFY workingDirectoryChanged)
    Q_PROPERTY(QString localUserName READ localUserName CONSTANT)
    Q_PROPERTY(QString localHostName READ localHostName CONSTANT)
    Q_PROPERTY(QString homeDirectory READ homeDirectory CONSTANT)
    Q_PROPERTY(bool hasSshInfo READ hasSshInfo NOTIFY sshInfoChanged)
    Q_PROPERTY(QString sshUserName READ sshUserName NOTIFY sshInfoChanged)
    Q_PROPERTY(QString sshHostName READ sshHostName NOTIFY sshInfoChanged)
    Q_PROPERTY(int sshPort READ sshPort NOTIFY sshInfoChanged)
    Q_PROPERTY(QString sshRemoteCommand READ sshRemoteCommand NOTIFY sshInfoChanged)
    Q_PROPERTY(qint64 processId READ processId NOTIFY processInfoChanged)
    Q_PROPERTY(qint64 foregroundCommandId READ foregroundCommandId NOTIFY foregroundCommandChanged)
    Q_PROPERTY(QString foregroundCommandName READ foregroundCommandName NOTIFY foregroundCommandChanged)
    Q_PROPERTY(bool busy READ isBusy NOTIFY processInfoChanged)
    Q_PROPERTY(bool readOnly READ isReadOnly WRITE setReadOnly NOTIFY readOnlyChanged)
    Q_PROPERTY(int scrollbackLines READ scrollbackLines WRITE setScrollbackLines NOTIFY scrollbackLinesChanged)
    Q_PROPERTY(bool monitorActivity READ monitorActivity WRITE setMonitorActivity NOTIFY monitoringChanged)
    Q_PROPERTY(bool monitorSilence READ monitorSilence WRITE setMonitorSilence NOTIFY monitoringChanged)
    Q_PROPERTY(int silenceSeconds READ silenceSeconds WRITE setSilenceSeconds NOTIFY silenceSecondsChanged)
    Q_PROPERTY(ProgressState progressState READ progressState NOTIFY progressChanged)
    Q_PROPERTY(int progressPercent READ progressPercent NOTIFY progressChanged)

public:
    enum class State {
        NotStarted,
        Starting,
        Running,
        Closing,
        Exited,
        Failed
    };
    Q_ENUM(State)

    enum class ProgressState {
        None,
        Normal,
        Error,
        Indeterminate,
        Paused
    };
    Q_ENUM(ProgressState)

    explicit Session(QObject *parent = nullptr);
    ~Session() override;

    [[nodiscard]] QString executable() const;
    void setExecutable(const QString &newExecutable);
    [[nodiscard]] QStringList arguments() const;
    void setArguments(const QStringList &newArguments);
    [[nodiscard]] QString initialDirectory() const;
    void setInitialDirectory(const QString &directory);
    [[nodiscard]] QVariantMap environmentChanges() const;
    void setEnvironmentChanges(const QVariantMap &changes);

    /** Start with the configured executable and literal arguments (excluding argv[0]).
     * Empty executable/directory use the system shell/home directory. Environment
     * strings override inherited values; null removes inherited variables.
     * Configuration can be changed while inactive, until explicitly closed.
     * Returns false if rejected. Starting, running, closing and explicitly closed
     * sessions reject start(). */
    Q_INVOKABLE bool start();
    Q_INVOKABLE void close();

    [[nodiscard]] State state() const;
    /** True while Starting, Running or Closing. */
    [[nodiscard]] bool isActive() const;
    [[nodiscard]] bool isFinished() const;
    [[nodiscard]] int exitCode() const;
    [[nodiscard]] QString errorString() const;
    [[nodiscard]] QString applicationTitle() const;
    [[nodiscard]] bool hasApplicationTitle() const;
    [[nodiscard]] QString launchedProgramName() const;
    [[nodiscard]] QString workingDirectory() const;
    [[nodiscard]] QString localUserName() const;
    [[nodiscard]] QString localHostName() const;
    [[nodiscard]] QString homeDirectory() const;
    [[nodiscard]] bool hasSshInfo() const;
    [[nodiscard]] QString sshUserName() const;
    [[nodiscard]] QString sshHostName() const;
    [[nodiscard]] int sshPort() const;
    [[nodiscard]] QString sshRemoteCommand() const;
    [[nodiscard]] qint64 processId() const;
    /** Observed foreground command, excluding an idle shell. Unavailable is -1/empty. */
    [[nodiscard]] qint64 foregroundCommandId() const;
    [[nodiscard]] QString foregroundCommandName() const;
    /** Unresolved process observations count as busy. */
    [[nodiscard]] bool isBusy() const;
    Q_INVOKABLE void refreshProcessInfo();

    [[nodiscard]] bool isReadOnly() const;
    void setReadOnly(bool readOnly);
    [[nodiscard]] int scrollbackLines() const;
    void setScrollbackLines(int lines);
    [[nodiscard]] bool monitorActivity() const;
    void setMonitorActivity(bool enabled);
    [[nodiscard]] bool monitorSilence() const;
    void setMonitorSilence(bool enabled);
    [[nodiscard]] int silenceSeconds() const;
    void setSilenceSeconds(int seconds);
    [[nodiscard]] ProgressState progressState() const;
    [[nodiscard]] int progressPercent() const;

    /** Trusted host input, including control characters. This is not paste. */
    Q_INVOKABLE bool sendInput(const QString &text);
    /** Already approved paste: filters controls and honors bracketed paste.
     * The host must obtain any necessary confirmation before calling this. */
    Q_INVOKABLE bool pasteApprovedText(const QString &text);
    [[nodiscard]] Q_INVOKABLE QString historyText() const;
    Q_INVOKABLE void scrollPrompt(int delta);
    Q_INVOKABLE void clearScrollback();
    Q_INVOKABLE void resetTerminal();

    // Private C++ integration.
    [[nodiscard]] SessionEvents *events();
    /** Copy the launch configuration and start it. */
    bool start(const TerminalLaunch &launch, int masterFd = -1);
    bool attachTeletype(int masterFd);
    void refreshProcessInfo(std::function<void()> completed);
    void resize(int columns, int rows, QSize cellPixels = {});
    bool send(const QByteArray &bytes);
    void feed(const QByteArray &bytes);
    bool snapshot(GhosttyQt::Frame &frame);
    [[nodiscard]] GhosttyQt::Terminal *terminal() const;
    [[nodiscard]] QString selectionText() const;
    [[nodiscard]] QString commandOutput() const;
    void setColors(QColor foreground, QColor background, const QVector<QRgb> &palette, QColor boldForeground = {});
    [[nodiscard]] qreal zoomOffset() const;
    void setZoomOffset(qreal offset);

Q_SIGNALS:
    void launchChanged();
    void stateChanged();
    void activeChanged();
    void exitStatusChanged();
    void errorChanged();
    void started();
    void exited(int code);
    void applicationTitleChanged();
    void launchedProgramNameChanged();
    void workingDirectoryChanged(const QString &directory);
    void sshInfoChanged();
    void processInfoChanged();
    void foregroundCommandChanged();
    void processInfoRefreshed();
    void readOnlyChanged();
    void scrollbackLinesChanged();
    void monitoringChanged();
    void silenceSecondsChanged();
    void activityDetected();
    void silenceDetected();
    void progressChanged();
    void bell();
    void notificationRequested(const QString &title, const QString &body);

private:
    friend class TerminalView;
    QPointer<TerminalView> m_view;

    void setState(State newState);
    void finish(int code);
    void updateProgress();
    void refreshSessionMetadata();
    void reportError(const QString &message);
    [[nodiscard]] QString observedDirectory() const;

    SessionEvents m_events;
    std::unique_ptr<GhosttyQt::Terminal> m_terminal;
    PtyProcess m_process;
    std::unique_ptr<SshMetadataResolver> m_sshMetadata;
    QString m_executable;
    QStringList m_arguments;
    QString m_initialDirectory;
    QVariantMap m_environmentChanges;
    State m_state{State::NotStarted};
    bool m_started{false};
    bool m_closeRequested{false};
    int m_exitCode{0};
    QString m_error;
    QString m_applicationTitle;
    bool m_hasApplicationTitle{false};
    QString m_launchedProgramName;
    QString m_currentDirectory;
    const QString m_localUserName;
    const QString m_localHostName;
    const QString m_homeDirectory;
    qint64 m_notifiedForegroundId{-1};
    QString m_notifiedForegroundName;
    int m_scrollbackLines{-1};
    QTimer m_silenceTimer;
    QElapsedTimer m_lastActivity;
    bool m_monitorActivity{false};
    bool m_monitorSilence{false};
    int m_silenceSeconds{10};
    bool m_readOnly{false};
    qreal m_zoomOffset{0};
    GhosttyQt::Progress m_progress;
    int m_columns{80};
    QByteArray m_traceMarker;
    QByteArray m_traceMarkerTail;
    int m_rows{24};
    QSize m_cellPixels;
};
}
