// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "HostCommandQuery_p.h"
#include "HostProcess_p.h"
#include "ProcessLaunch.h"
#include "ProcessQueryLimits_p.h"
#include "ProcessScope_p.h"

#include <KLocalizedString>
#include <KPtyDevice>
#include <KSandbox>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QFileInfo>
#include <QMap>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStringList>
#include <QtConcurrentRun>
#include <chrono>
#include <csignal>
#include <optional>
#include <sys/wait.h>
#include <utility>

namespace Kwi
{
namespace
{
namespace Protocol = HostProcessProtocol;

struct HostEnvironment {
    QProcessEnvironment values;
    QStringList commandPrefix{QStringLiteral("/usr/bin/env")};
};

HostEnvironment hostEnvironment(const QMap<QString, std::optional<QString>> &changes)
{
    HostEnvironment environment;

    for (auto change = changes.cbegin(); change != changes.cend(); ++change) {
        if (change.value()) {
            environment.values.insert(change.key(), *change.value());
        } else {
            environment.commandPrefix.append({QStringLiteral("-u"), change.key()});
        }
    }

    environment.commandPrefix.append(QStringLiteral("--"));

    return environment;
}

std::optional<Protocol::Observation> inspect(Protocol::ProcessIdentity identity)
{
    // Use blocking KSandbox calls because teardown must finish without a GUI
    // or worker event loop. HostCommandQuery needs event delivery for cleanup.
    QProcess query;
    const auto command = Protocol::boundedQueryCommand({QStringLiteral("/bin/sh"),
                                                        QStringLiteral("-c"),
                                                        Protocol::inspectProcessScript(),
                                                        QStringLiteral("kwi-host-query"),
                                                        QString::number(ProcessQueryLimits::MaximumCommandLineBytes + 1),
                                                        QString::number(identity.pid),
                                                        identity.startTime ? QString::number(identity.startTime) : QString(),
                                                        QStringLiteral("cleanup")});
    query.setProgram(command.first());
    query.setArguments(command.sliced(1));

    QObject::connect(&query, &QProcess::readyReadStandardOutput, &query, [&query] {
        if (query.bytesAvailable() > ProcessQueryLimits::MaximumOutputBytes) {
            query.kill();
        }
    });

    QObject::connect(&query, &QProcess::readyReadStandardError, &query, [&query, count = qint64(0)]() mutable {
        count += query.readAllStandardError().size();

        if (count > ProcessQueryLimits::MaximumOutputBytes) {
            query.kill();
        }
    });

    KSandbox::startHostProcess(query, QIODevice::ReadOnly);
    const bool completed{query.waitForFinished(ProcessQueryLimits::TimeoutMs)};

    if (!completed) {
        query.kill();
        query.waitForFinished(-1);
    }

    if (!completed || query.exitStatus() != QProcess::NormalExit || query.exitCode() != 0) {
        return std::nullopt;
    }

    return Protocol::decodeObservation(query.readAllStandardOutput(), identity.pid, identity.startTime);
}

// Retain the duplicated master until inspection has captured the foreground processes.
void cleanUpHostChild(QDBusPendingCall spawn, const QDBusConnection &bus, Protocol::ProcessIdentity identity, const QDBusUnixFileDescriptor &master)
{
    Q_UNUSED(master)
    spawn.waitForFinished();
    const QDBusPendingReply<uint> reply{spawn};

    if (reply.isError()) {
        return;
    }

    identity.pid = reply.value();
    const auto observation = inspect(identity);
    const bool owned{bus.call(Protocol::signal(identity.pid, 0), QDBus::Block, Protocol::CallTimeoutMs).type() != QDBusMessage::ErrorMessage};

    if (observation && owned && !observation->members.isEmpty()) {
        const auto cleanup = Protocol::command(QStringLiteral("/"),
                                               {QStringLiteral("/bin/sh"),
                                                QStringLiteral("-c"),
                                                Protocol::cleanupProcessScript(),
                                                QStringLiteral("kwi-host-cleanup"),
                                                QString::number(identity.pid),
                                                Protocol::encodeMembers(observation->members)});

        if (bus.call(cleanup, QDBus::Block, Protocol::CallTimeoutMs).type() != QDBusMessage::ErrorMessage) {
            return;
        }
    }

    // Use Flatpak's ownership check when inspection cannot safely identify process groups.
    bus.call(Protocol::signal(identity.pid, SIGKILL, true), QDBus::Block, Protocol::CallTimeoutMs);
}
}

HostProcess::HostProcess(QObject *parent)
    : ProcessController{parent}
    , m_bus{QDBusConnection::sessionBus()}
{
    m_bus.connect(QString::fromLatin1(Protocol::Service),
                  QString::fromLatin1(Protocol::Object),
                  QString::fromLatin1(Protocol::Interface),
                  QStringLiteral("HostCommandExited"),
                  this,
                  SLOT(hostExited(uint, uint)));
    m_startupTimer.setSingleShot(true);
    connect(&m_startupTimer, &QTimer::timeout, this, &HostProcess::probe);
}

HostProcess::~HostProcess()
{
    beginCleanup();
    cancelQueries();
}

bool HostProcess::open(int masterFd)
{
    if (!prepareMaster(masterFd)) {
        return false;
    }

    m_device = std::make_unique<KPtyDevice>();

    if (!(m_master.isValid() ? m_device->open(m_master.fileDescriptor()) : m_device->open())) {
        Q_EMIT errorOccurred(i18n("Could not create a terminal: %1", m_device->errorString()));

        return false;
    }

    return true;
}

KPtyDevice *HostProcess::device() const
{
    return m_device.get();
}

bool HostProcess::start(const TerminalLaunch &launch)
{
    m_phase = Phase::Resolving;
    m_info.directory = launch.directory;
    m_info.busy = true;

    if (launch.environmentChanges.contains(QStringLiteral("DBUS_SESSION_BUS_ADDRESS"))
        || launch.environmentChanges.contains(QStringLiteral("XDG_RUNTIME_DIR"))) {
        resolve(launch);

        return true;
    }

    auto *query = new HostCommandQuery(m_bus, this);
    m_launchQuery = query;

    connect(query, &HostCommandQuery::finished, this, [this, query, launch](HostCommandQuery::Result result, const QByteArray &) {
        m_launchQuery.clear();
        query->deleteLater();

        if (result == HostCommandQuery::Result::Success) {
            checkScopeManager(launch);
        } else {
            resolve(launch);
        }
    });

    // Check the launcher itself, without executing a command or parsing version strings.
    query->start({QStringLiteral("/usr/bin/systemd-run"), QStringLiteral("--expand-environment=no"), QStringLiteral("--version")});

    return true;
}

void HostProcess::checkScopeManager(const TerminalLaunch &launch)
{
    auto *query = new HostCommandQuery(m_bus, this);
    m_launchQuery = query;

    connect(query, &HostCommandQuery::finished, this, [this, query, launch](HostCommandQuery::Result result, const QByteArray &) {
        m_launchQuery.clear();
        query->deleteLater();
        m_useScopes = result == HostCommandQuery::Result::Success;
        resolve(launch);
    });

    query->start({QStringLiteral("/usr/bin/systemctl"),
                  QStringLiteral("--user"),
                  QStringLiteral("show"),
                  QStringLiteral("--property=Version"),
                  QStringLiteral("--value")});
}

bool HostProcess::waitingForLauncher() const
{
    return !m_startupCommands.isEmpty() && !m_startupDeadline.hasExpired();
}

bool HostProcess::isRunning() const
{
    return m_phase == Phase::Resolving || m_phase == Phase::Starting || m_phase == Phase::Running || m_phase == Phase::Stopping;
}

qint64 HostProcess::processId() const
{
    return isRunning() && m_pid ? qint64(m_pid) : -1;
}

QString HostProcess::program() const
{
    return m_program;
}

ProcessInfo HostProcess::processInfo() const
{
    auto info = m_info;

    if (!isRunning()) {
        info.foregroundPid = -1;
        info.foregroundName.clear();
        info.ssh = {};
        info.busy = false;
    } else {
        info.busy = info.busy || m_probePending || m_phase != Phase::Running;

        if (!info.busy) {
            info.foregroundPid = -1;
            info.foregroundName.clear();
            info.ssh = {};
        }
    }

    return info;
}

void HostProcess::cancelQueries()
{
    delete m_probeQuery.data();
    delete m_launchQuery.data();
    m_startupTimer.stop();
    m_startupCommands.clear();
    m_probePending = false;
}

void HostProcess::resolve(const TerminalLaunch &launch)
{
    const auto path = launch.environmentChanges.constFind(QStringLiteral("PATH"));
    const bool removePath{path != launch.environmentChanges.cend() && !path.value()};
    auto changes = launch.resolvedEnvironmentChanges();
    // A preset's SHELL variable must not change which default shell is launched.
    changes.remove(QStringLiteral("SHELL"));
    const auto environment = hostEnvironment(changes);

    auto *query = new HostCommandQuery(m_bus, this);
    m_launchQuery = query;

    connect(query, &HostCommandQuery::finished, this, [this, query, launch](HostCommandQuery::Result result, const QByteArray &output) {
        const auto resolved = Protocol::decodeCommand(output);
        m_launchQuery.clear();
        query->deleteLater();

        if (result == HostCommandQuery::Result::FailedToStart) {
            fail(i18n("Could not contact the host terminal service."));

            return;
        }

        if (result != HostCommandQuery::Result::Success || !resolved) {
            fail(launch.program.isEmpty() ? i18n("Could not find the system shell on the host.")
                                          : i18n("Could not find the terminal command on the host: %1", launch.program));

            return;
        }

        m_initialExecutable = resolved->executable;
        m_program = resolved->program;
        m_interactiveShell = resolved->shell && interactiveShellArguments(launch.arguments);
        spawn(launch);
    });

    query->start(environment.commandPrefix
                     + QStringList{QStringLiteral("/bin/sh"),
                                   QStringLiteral("-c"),
                                   Protocol::resolveCommandScript(),
                                   QStringLiteral("kwi-host-query"),
                                   launch.program,
                                   launch.program.isEmpty() ? QStringLiteral("1") : QStringLiteral("0"),
                                   removePath ? QStringLiteral("1") : QStringLiteral("0")},
                 launch.directory,
                 environment.values);
}

void HostProcess::spawn(const TerminalLaunch &launch)
{
    const auto environment = hostEnvironment(launch.resolvedEnvironmentChanges());
    QStringList arguments{environment.commandPrefix + QStringList{m_program} + launch.arguments};

    if (m_useScopes) {
        const auto scopedCommand = ProcessScope::hostCommand(QStringLiteral("/usr/bin/systemd-run"), arguments);
        m_startupCommands = {scopedCommand, arguments};
        m_startupDeadline = QDeadlineTimer{std::chrono::seconds{5}};
        arguments = scopedCommand;
    }

    m_phase = Phase::Starting;
    m_spawn = m_bus.asyncCall(Protocol::command(launch.directory, arguments, environment.values, m_device->slaveFd(), Protocol::CommandFlags::WatchBus),
                              Protocol::CallTimeoutMs);
    auto *watcher = new QDBusPendingCallWatcher(*m_spawn, this);

    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher] {
        QDBusPendingReply<uint> reply{*watcher};
        watcher->deleteLater();

        if (reply.isError()) {
            fail(i18n("Could not start the host terminal command: %1", reply.error().message()));

            return;
        }

        m_pid = reply.value();

        if (m_phase == Phase::Starting) {
            m_phase = Phase::Running;
        }

        const auto exit = m_earlyExits.constFind(m_pid);
        const std::optional<uint> exitStatus{exit == m_earlyExits.cend() ? std::nullopt : std::optional(exit.value())};
        m_earlyExits.clear();
        m_device->closeSlave();
        const QPointer<HostProcess> guard{this};

        Q_EMIT started();

        if (guard == nullptr) {
            return;
        }

        if (exitStatus) {
            hostExited(m_pid, *exitStatus);
        } else if (m_refreshCompletion) {
            probe();
        }
    });
}

void HostProcess::fail(const QString &message)
{
    const QPointer<HostProcess> guard{this};

    Q_EMIT errorOccurred(message);

    if (guard != nullptr) {
        closeTransport();
    }

    // closeTransport() emits transportClosed(). Its handlers can destroy this object.
    // cppcheck-suppress duplicateCondition
    if (guard != nullptr) {
        finish(127);
    }
}

void HostProcess::finish(int exitCode)
{
    if (m_phase == Phase::Finished) {
        return;
    }

    m_phase = Phase::Finished;
    m_earlyExits.clear();
    cancelQueries();
    m_info.foregroundPid = -1;
    m_info.foregroundName.clear();
    m_info.ssh = {};
    m_info.busy = false;
    const QPointer<HostProcess> guard{this};
    completeRefresh();

    if (guard != nullptr) {
        Q_EMIT finished(exitCode);
    }
}

void HostProcess::hostExited(uint pid, uint status)
{
    if (!m_pid && m_spawn && isRunning()) {
        m_earlyExits.insert(pid, status);
    } else if (pid == m_pid && isRunning()) {
        finish(WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0));
    }
}

void HostProcess::invalidateProcessInfo(bool input)
{
    if (input) {
        ++m_inputGeneration;
        m_info.busy = true;
    }

    if (m_startupTimer.isActive()) {
        m_startupTimer.start(std::chrono::milliseconds{0});
    }
}

void HostProcess::refreshProcessInfo(std::function<void()> completed)
{
    Q_ASSERT(!m_refreshCompletion);
    m_refreshCompletion = std::move(completed);
    probe();
}

void HostProcess::probe()
{
    if (!isRunning() || m_phase == Phase::Stopping) {
        m_info.busy = isRunning();
        completeRefresh();

        return;
    }

    if (m_phase != Phase::Running || m_probePending) {
        return;
    }

    m_probePending = true;
    const quint64 inputGeneration{m_inputGeneration};
    auto *query = new HostCommandQuery(m_bus, this);
    m_probeQuery = query;

    connect(query, &HostCommandQuery::finished, this, [this, query, inputGeneration](HostCommandQuery::Result result, const QByteArray &output) {
        const auto observation = Protocol::decodeObservation(output, m_pid, m_identity.startTime);
        m_probeQuery.clear();
        query->deleteLater();

        if (m_phase != Phase::Running || result != HostCommandQuery::Result::Success || !observation) {
            finishProbe({}, inputGeneration);

            return;
        }

        // Verify we still own this host child before trusting the inspection result.
        auto *watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(Protocol::signal(m_pid, 0), Protocol::CallTimeoutMs), this);

        connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, observation, inputGeneration] {
            const QDBusPendingReply<> reply{*watcher};
            watcher->deleteLater();
            finishProbe(reply.isError() ? std::nullopt : observation, inputGeneration);
        });
    });

    query->start({QStringLiteral("/bin/sh"),
                  QStringLiteral("-c"),
                  Protocol::inspectProcessScript(),
                  QStringLiteral("kwi-host-query"),
                  QString::number(ProcessQueryLimits::MaximumCommandLineBytes + 1),
                  QString::number(m_pid),
                  m_identity.startTime ? QString::number(m_identity.startTime) : QString(),
                  waitingForLauncher()     ? QStringLiteral("startup")
                      : m_interactiveShell ? QStringLiteral("shell")
                                           : QStringLiteral("command"),
                  QString::number(m_info.ssh.pid),
                  QString::number(m_info.ssh.startTime),
                  m_info.ssh.executable});
}

void HostProcess::finishProbe(const std::optional<Protocol::Observation> &observation, quint64 inputGeneration)
{
    m_probePending = false;
    const bool observingStartup{m_phase == Phase::Running && !m_startupCommands.isEmpty()};
    ProcessInfo info;

    if (m_phase == Phase::Running && observation) {
        // systemd-run and env execute in place without a readiness notification.
        const bool keepObservingLauncher{waitingForLauncher() && (observation->arguments.isEmpty() || m_startupCommands.contains(observation->arguments))};

        if (!keepObservingLauncher) {
            m_startupCommands.clear();
        }

        info.directory = observation->directory.isEmpty() ? m_info.directory : observation->directory;
        info.foregroundPid = observation->foreground;
        info.foregroundName = QFileInfo(observation->foregroundExecutable).fileName();

        if (observation->ssh.pid) {
            info.ssh = {observation->ssh.pid, observation->ssh.startTime, observation->foregroundExecutable, info.directory, observation->arguments};

            if (m_info.ssh.pid == info.ssh.pid && m_info.ssh.startTime == info.ssh.startTime && m_info.ssh.executable == info.ssh.executable) {
                info.ssh.arguments = m_info.ssh.arguments;
            }
        }

        const bool idleShell{!observation->arguments.isEmpty() && observation->foreground == m_pid && m_interactiveShell
                             && observation->foregroundExecutable == m_initialExecutable && interactiveShellArguments(observation->arguments.sliced(1))};
        info.busy = !idleShell || inputGeneration != m_inputGeneration;
        m_identity = observation->leader;
    } else {
        info = m_info;
        info.busy = isRunning();
    }

    const bool changed{info != m_info};
    m_info = std::move(info);

    if (observingStartup) {
        if (!waitingForLauncher()) {
            m_startupCommands.clear();
        }

        // Read once more after observing exec: argv and executable are separate proc reads.
        m_startupTimer.start(m_startupCommands.isEmpty() ? std::chrono::milliseconds{0} : std::chrono::milliseconds{100});
    }

    completeRefresh(changed);
}

void HostProcess::completeRefresh(bool changed)
{
    const auto completed = m_startupTimer.isActive() ? std::function<void()>{} : std::exchange(m_refreshCompletion, {});
    const QPointer<HostProcess> guard{this};

    if (changed) {
        Q_EMIT processInfoChanged();
    }

    if (guard != nullptr && completed) {
        completed();
    }
}

void HostProcess::beginCleanup()
{
    if ((m_phase != Phase::Starting && m_phase != Phase::Running) || !m_spawn) {
        return;
    }

    m_phase = Phase::Stopping;

    const QDBusUnixFileDescriptor master(m_device->masterFd());
    (void)QtConcurrent::run(cleanUpHostChild, *m_spawn, m_bus, m_identity, master);
}

void HostProcess::close()
{
    if (m_phase == Phase::Stopping) {
        return;
    }

    if (m_phase == Phase::Resolving) {
        cancelQueries();
        const QPointer<HostProcess> guard{this};
        closeTransport();

        if (guard != nullptr) {
            finish(0);
        }

        return;
    }

    beginCleanup();
    cancelQueries();
    const QPointer<HostProcess> guard{this};
    closeTransport();

    if (guard != nullptr) {
        completeRefresh();
    }
}
}
