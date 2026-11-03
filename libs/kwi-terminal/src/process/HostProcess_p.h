// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "HostProcessProtocol_p.h"
#include "ProcessController_p.h"
#include <QDBusConnection>
#include <QDBusPendingCall>
#include <QDeadlineTimer>
#include <QHash>
#include <QList>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <functional>
#include <memory>
#include <optional>

namespace Kwi
{
class HostCommandQuery;

class HostProcess final : public ProcessController
{
    Q_OBJECT

public:
    explicit HostProcess(QObject *parent = nullptr);
    ~HostProcess() override;

    bool open(int masterFd) override;
    [[nodiscard]] KPtyDevice *device() const override;
    bool start(const TerminalLaunch &launch) override;
    void close() override;

    [[nodiscard]] bool isRunning() const override;
    [[nodiscard]] qint64 processId() const override;
    [[nodiscard]] QString program() const override;
    [[nodiscard]] ProcessInfo processInfo() const override;

    void refreshProcessInfo(std::function<void()> completed = {}) override;
    void invalidateProcessInfo(bool input) override;

private Q_SLOTS:
    void hostExited(uint pid, uint status);

private:
    enum class Phase {
        Idle,
        Resolving,
        Starting,
        Running,
        Stopping,
        Finished
    };

    void cancelQueries();
    void checkScopeManager(const TerminalLaunch &launch);
    void resolve(const TerminalLaunch &launch);
    void spawn(const TerminalLaunch &launch);
    void probe();
    void finishProbe(const std::optional<HostProcessProtocol::Observation> &observation, quint64 inputGeneration);
    void completeRefresh(bool changed = false);
    [[nodiscard]] bool waitingForLauncher() const;
    void beginCleanup();
    void finish(int exitCode);
    void fail(const QString &message);

    QDBusConnection m_bus;
    std::unique_ptr<KPtyDevice> m_device;
    QPointer<HostCommandQuery> m_launchQuery;
    QPointer<HostCommandQuery> m_probeQuery;
    std::optional<QDBusPendingCall> m_spawn;
    std::function<void()> m_refreshCompletion;
    QHash<uint, uint> m_earlyExits;
    QList<QStringList> m_startupCommands;
    QDeadlineTimer m_startupDeadline;
    QTimer m_startupTimer;
    ProcessInfo m_info;
    HostProcessProtocol::ProcessIdentity m_identity;
    QString m_initialExecutable;
    QString m_program;
    Phase m_phase{Phase::Idle};
    uint m_pid{0};
    quint64 m_inputGeneration{0};
    bool m_interactiveShell{false};
    bool m_useScopes{false};
    bool m_probePending{false};
};
}
