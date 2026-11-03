// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "NativeLaunch_p.h"
#include "NativeProcessInfo_p.h"
#include "NativeProcess_p.h"
#include "ProcessLaunch.h"
#include "ProcessQueryLimits_p.h"

#include <KLocalizedString>
#include <KPtyDevice>
#include <KPtyProcess>
#include <KUser>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QStandardPaths>
#include <QStringList>
#include <QThread>
#include <QtConcurrentRun>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <iterator>
#include <termios.h>
#include <unistd.h>
#include <utility>

namespace
{
bool registeredShell(const QString &executable, const QProcessEnvironment &inherited)
{
    QStringList shells{inherited.value(QStringLiteral("SHELL")), KUser().shell(), QStringLiteral("/bin/sh")};
    QFile registered{QStringLiteral("/etc/shells")};

    if (registered.open(QIODevice::ReadOnly)) {
        while (!registered.atEnd()) {
            const QString line{QString::fromLocal8Bit(registered.readLine()).section(QLatin1Char('#'), 0, 0).trimmed()};

            if (!line.isEmpty()) {
                shells.append(line);
            }
        }
    }

    return std::ranges::any_of(shells, [&executable](const QString &shell) {
        return !shell.isEmpty() && QFileInfo(shell).canonicalFilePath() == executable;
    });
}

pid_t processInGroup(pid_t group, pid_t session)
{
    const pid_t leaderSession{getsid(group)};

    if (leaderSession >= 0) {
        return leaderSession == session ? group : -1;
    }

    if (kill(-group, 0) < 0 && errno == ESRCH) {
        return -1;
    }

    // The group can outlive its leader, both during observation and cleanup.
    return Kwi::NativeProcessInfo::groupMember(group, session);
}

QStringList processArguments(pid_t pid)
{
    const QByteArray bytes{Kwi::NativeProcessInfo::commandLine(pid)};

    if (bytes.size() > Kwi::ProcessQueryLimits::MaximumCommandLineBytes || !bytes.endsWith('\0') || bytes.startsWith('\0')) {
        return {};
    }

    const auto fields = bytes.chopped(1).split('\0');
    QStringList arguments;
    arguments.reserve(fields.size());

    std::ranges::transform(fields, std::back_inserter(arguments), [](const QByteArray &field) {
        return QString::fromLocal8Bit(field);
    });

    return arguments;
}

QString findProgram(const QString &program, const QString &workingDirectory, const QProcessEnvironment &environment)
{
    const QDir directory{workingDirectory};

    if (program.contains(QLatin1Char('/'))) {
        return QStandardPaths::findExecutable(directory.absoluteFilePath(program));
    }

    QString path{environment.value(QStringLiteral("PATH"))};

    if (!environment.contains(QStringLiteral("PATH"))) {
        const size_t length{confstr(_CS_PATH, nullptr, 0)};
        QByteArray defaultPath(qsizetype(length), Qt::Uninitialized);

        if (length > 0 && confstr(_CS_PATH, defaultPath.data(), length) > 0) {
            path = QString::fromLocal8Bit(defaultPath.constData());
        }
    }

    auto directories = path.split(QLatin1Char(':'), Qt::KeepEmptyParts);
    std::ranges::transform(directories, directories.begin(), [&directory](const QString &entry) {
        return entry.isEmpty() ? directory.absolutePath() : directory.absoluteFilePath(entry);
    });

    return QStandardPaths::findExecutable(program, directories);
}
}

namespace Kwi
{
namespace
{
/** Observation of a local PTY. An attached PTY has no owned leader. */
ProcessInfo localProcessInfo(KPtyDevice *device, qint64 leader, const QString &initialExecutable, bool interactiveShell, const ProcessInfo &previous)
{
    ProcessInfo info;
    info.directory = previous.directory;
    info.busy = true; // An unavailable foreground observation is not an idle shell.

    if (!device || device->masterFd() < 0) {
        return info;
    }

    const qint64 foreground{tcgetpgrp(device->masterFd())};
    if (foreground <= 0) {
        return info;
    }
    const qint64 target{foreground > 0 ? processInGroup(pid_t(foreground), leader > 0 ? pid_t(leader) : tcgetsid(device->masterFd())) : -1};

    for (const qint64 pid : {target, leader}) {
        if (pid <= 0) {
            continue;
        }

        const QString directory{NativeProcessInfo::directory(pid_t(pid))};

        if (!directory.isEmpty()) {
            info.directory = directory;
            break;
        }
    }

    info.busy = foreground > 0 && (leader <= 0 || foreground != leader);

    if (leader > 0 && !info.busy) {
        const QString executable{NativeProcessInfo::executable(pid_t(leader))};
        QStringList arguments{processArguments(pid_t(leader))};
        const bool validArguments{!arguments.isEmpty()};

        if (validArguments) {
            arguments.removeFirst();
        }

        info.busy = !interactiveShell || executable.isEmpty() || executable != initialExecutable || !validArguments || !interactiveShellArguments(arguments);
    }

    if (info.busy && foreground > 0) {
        info.foregroundPid = foreground;
        const QString executable{target > 0 ? NativeProcessInfo::executable(pid_t(target)) : QString{}};
        info.foregroundName = QFileInfo(executable).fileName();

        if (info.foregroundName == QLatin1String("ssh")) {
            const quint64 startTime{NativeProcessInfo::startTime(pid_t(target))};

            if (startTime) {
                // exec/re-written argv can change without changing PID or birth
                // time. Metadata refresh is already bounded/coalesced; reread
                // SSH argv here and reject a lifetime race around that query.
                const auto arguments = processArguments(pid_t(target));
                if (!arguments.isEmpty() && NativeProcessInfo::startTime(pid_t(target)) == startTime
                    && NativeProcessInfo::executable(pid_t(target)) == executable) {
                    info.ssh = {target, startTime, executable, info.directory, arguments};
                }
            }
        }
    }

    return info;
}
}

NativeProcess::NativeProcess()
{
    m_terminateTimer.setSingleShot(true);
    m_killTimer.setSingleShot(true);

    connect(&m_terminateTimer, &QTimer::timeout, this, [this] {
        signalSession(SIGTERM);
    });

    connect(&m_killTimer, &QTimer::timeout, this, [this] {
        signalSession(SIGKILL);
    });
}

NativeProcess::~NativeProcess()
{
    if (isRunning()) {
#ifdef Q_OS_MACOS
        // The started callback normally closes this parent-side slave. Teardown
        // during Starting must do it too, before disconnecting that callback.
        m_process->pty()->closeSlave();
#endif
        signalSession(SIGKILL);
        m_process->disconnect(); // clazy:exclude=unused-result-check
        m_process->pty()->disconnect(); // clazy:exclude=unused-result-check
        m_process->kill();
        m_process->moveToThread(nullptr);

        // Retain the PTY master until QProcess reaps the child, without blocking the GUI.
        (void)QtConcurrent::run([process = std::move(m_process), master = m_master]() mutable {
            process->moveToThread(QThread::currentThread());
#ifdef Q_OS_MACOS
            if (process->state() == QProcess::Starting) {
                process->waitForStarted(2000);
                process->kill();
            }
            if (!process->waitForFinished(5000)) {
                process->kill();
                process->waitForFinished(1000);
            }
#else
            process->waitForFinished(-1);
#endif
            process.reset();
        });
    }
}

bool NativeProcess::open(int masterFd)
{
    if (!prepareMaster(masterFd)) {
        return false;
    }

    m_process = m_master.isValid() ? std::make_unique<KPtyProcess>(m_master.fileDescriptor()) : std::make_unique<KPtyProcess>();

    if (device()->masterFd() < 0) {
        Q_EMIT errorOccurred(i18n("Could not create a terminal: %1", device()->errorString()));

        return false;
    }

    return true;
}

KPtyDevice *NativeProcess::device() const
{
    return m_process ? m_process->pty() : nullptr;
}

bool NativeProcess::start(const TerminalLaunch &launch)
{
    const QString requested{launch.program.isEmpty() ? systemShell(launch.inheritedEnvironment) : launch.program};
    const auto environment = launch.environment();
    m_info.directory = launch.directory.isEmpty() ? QDir::homePath() : launch.directory;
    const QString executable{findProgram(requested, m_info.directory, environment)};

    if (executable.isEmpty()) {
        Q_EMIT errorOccurred(i18n("Could not find executable: %1", requested));

        return false;
    }

    const QStringList arguments{nativeLaunchArguments(launch, executable)};
    m_initialExecutable = QFileInfo(executable).canonicalFilePath();
    m_interactiveShell = registeredShell(m_initialExecutable, launch.inheritedEnvironment) && interactiveShellArguments(arguments);
#ifdef Q_OS_MACOS
    // /bin/sh is an Apple exec wrapper, not the shell image reported by libproc.
    // Follow only its documented system selector; don't accept arbitrary execs as
    // idle shells or weaken the executable/argv checks for other commands.
    if (m_initialExecutable == QLatin1String("/bin/sh")) {
        const QString selected{QFileInfo(QStringLiteral("/private/var/select/sh")).canonicalFilePath()};
        const QStringList variants{QStringLiteral("/bin/bash"), QStringLiteral("/bin/dash"), QStringLiteral("/bin/zsh")};
        m_initialExecutable = variants.contains(selected) ? selected : QStringLiteral("/bin/bash");
    }
#endif
    m_info.busy = !m_interactiveShell;
    m_process->setPtyChannels(KPtyProcess::AllChannels);
    m_process->setInputChannelMode(QProcess::ForwardedInputChannel);
    m_process->setUseUtmp(false);
    m_process->setProgram(executable, arguments);
    m_process->setWorkingDirectory(m_info.directory);
    m_process->setUnixProcessParameters(QProcess::UnixProcessFlag::ResetSignalHandlers | QProcess::UnixProcessFlag::CloseFileDescriptors);
    m_process->setProcessEnvironment(environment);

    connect(m_process.get(), &QProcess::started, this, [this] {
        m_pid = m_process->processId();
        device()->closeSlave();
        const QPointer<NativeProcess> guard{this};

        if (m_closing) {
            beginClose();
        }

        if (guard != nullptr) {
            Q_EMIT started();
        }
    });

    connect(m_process.get(), &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        if (m_closing) {
            signalSession(SIGKILL);
        }

        m_terminateTimer.stop();
        m_killTimer.stop();

        Q_EMIT finished(status == QProcess::CrashExit ? 128 + code : code);
    });

    connect(m_process.get(), &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::Crashed) {
            return;
        }

        const QPointer<NativeProcess> guard{this};

        // QPointer construction cannot invalidate this context-bound callback's receiver.
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage)
        Q_EMIT errorOccurred(i18n("Could not run the terminal command: %1", m_process->errorString()));

        if (guard != nullptr && error == QProcess::FailedToStart) {
            closeTransport();

            if (guard != nullptr) {
                // Let Qt finish failed-exec cleanup before owners react to exit.
                QTimer::singleShot(0, this, [this] {
                    Q_EMIT finished(127);
                });
            }
        }
    });

    const QPointer<NativeProcess> guard{this};
    m_process->start();

    if (guard != nullptr) {
        m_pid = m_process->processId();
    }

    return bool(guard);
}

bool NativeProcess::isRunning() const
{
    return m_process && m_process->state() != QProcess::NotRunning;
}

qint64 NativeProcess::processId() const
{
    return isRunning() ? m_pid : -1;
}

QString NativeProcess::program() const
{
    return m_process ? m_process->program().value(0) : QString();
}

ProcessInfo NativeProcess::processInfo() const
{
    auto info = m_info;

    if (!isRunning()) {
        info.foregroundPid = -1;
        info.foregroundName.clear();
        info.ssh = {};
        info.busy = false;
    }

    return info;
}

void NativeProcess::invalidateProcessInfo(bool input)
{
    if (input && isRunning()) {
        m_info.busy = true;
    }
}

// The virtual contract owns callbacks for asynchronous backends. clazy:exclude-next-line=function-args-by-ref
void NativeProcess::refreshProcessInfo(std::function<void()> completed)
{
    const auto info = m_process && m_process->state() == QProcess::Running ? localProcessInfo(device(), m_pid, m_initialExecutable, m_interactiveShell, m_info)
                                                                           : processInfo();
    const bool changed{info != m_info};
    m_info = info;
    const QPointer<NativeProcess> guard{this};

    if (changed) {
        Q_EMIT processInfoChanged();
    }

    if (guard != nullptr && completed) {
        completed();
    }
}

void NativeProcess::signalSession(int signal) const
{
    if (!m_process || m_pid <= 0) {
        return;
    }

    const int master{device()->masterFd()};
    const int foreground{master >= 0 ? tcgetpgrp(master) : m_closingForegroundGroup};

    if (foreground > 0 && foreground != m_pid && foreground != getpgrp() && (master >= 0 || processInGroup(foreground, pid_t(m_pid)) > 0)) {
        kill(-foreground, signal);
    }

    if (isRunning()) {
        kill(-pid_t(m_pid), signal);
        kill(pid_t(m_pid), signal);
    } else if (processInGroup(pid_t(m_pid), pid_t(m_pid)) > 0) {
        kill(-pid_t(m_pid), signal);
    }
}

void NativeProcess::beginClose()
{
    if (device() && device()->masterFd() >= 0) {
        m_closingForegroundGroup = tcgetpgrp(device()->masterFd());
    }

    signalSession(SIGHUP);
    const QPointer<NativeProcess> guard{this};
    closeTransport();

    if (guard != nullptr && isRunning()) {
        m_terminateTimer.start(std::chrono::milliseconds{500});
        m_killTimer.start(std::chrono::milliseconds{1500});
    }
}

void NativeProcess::close()
{
    if (std::exchange(m_closing, true)) {
        return;
    }

    if (!m_process || m_process->state() != QProcess::Starting) {
        beginClose();
    }
}

namespace
{
class AttachedProcess final : public ProcessController // clazy:exclude=ctor-missing-parent-argument,missing-qobject-macro
{
public:
    bool open(int masterFd) override
    {
        if (masterFd < 0) {
            Q_EMIT errorOccurred(i18n("Could not attach to an invalid terminal descriptor."));

            return false;
        }

        if (!prepareMaster(masterFd)) {
            return false;
        }

        m_device = std::make_unique<KPtyDevice>();

        if (!m_device->open(m_master.fileDescriptor())) {
            Q_EMIT errorOccurred(i18n("Could not attach to the terminal: %1", m_device->errorString()));

            return false;
        }

        connect(m_device.get(), &KPtyDevice::readEof, this, [this] {
            finish();
        });

        return true;
    }

    KPtyDevice *device() const override
    {
        return m_device.get();
    }

    bool start(const TerminalLaunch &) override
    {
        m_active = true;
        device()->closeSlave();

        Q_EMIT started();

        return true;
    }

    void close() override
    {
        const QPointer<AttachedProcess> guard{this};
        closeTransport();

        if (guard != nullptr) {
            QTimer::singleShot(0, this, [this] {
                finish();
            });
        }
    }

    bool isRunning() const override
    {
        return m_active;
    }

    qint64 processId() const override
    {
        return -1;
    }

    QString program() const override
    {
        return {};
    }

    ProcessInfo processInfo() const override
    {
        return m_active ? m_info : ProcessInfo{};
    }

    void invalidateProcessInfo(bool input) override
    {
        if (input && m_active) {
            m_info.busy = true;
        }
    }

    // The virtual contract owns callbacks for asynchronous backends. clazy:exclude-next-line=function-args-by-ref
    void refreshProcessInfo(std::function<void()> completed = {}) override
    {
        const auto info = m_active ? localProcessInfo(device(), -1, {}, false, m_info) : ProcessInfo{};
        const bool changed{info != m_info};
        m_info = info;
        const QPointer<AttachedProcess> guard{this};

        if (changed) {
            Q_EMIT processInfoChanged();
        }

        if (guard != nullptr && completed) {
            completed();
        }
    }

private:
    void finish()
    {
        if (std::exchange(m_active, false)) {
            Q_EMIT finished(0);
        }
    }

    std::unique_ptr<KPtyDevice> m_device;
    ProcessInfo m_info;
    bool m_active{false};
};
}

std::unique_ptr<ProcessController> attachedProcess()
{
    return std::make_unique<AttachedProcess>();
}
}
