// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "PtyProcess.h"
#include "AttachedProcess_p.h"
#include "Diagnostics_p.h"
#include "HostProcess_p.h"
#include "NativeProcess_p.h"
#include "ProcessLaunch.h"

#include <KLocalizedString>
#include <KPtyDevice>
#include <KSandbox>
#include <QElapsedTimer>
#include <QPointer>
#include <QSocketNotifier>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstring>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <utility>

namespace
{
constexpr qsizetype maximumPendingInput = 4 * 1024 * 1024;
constexpr qint64 readBudget = 256 * 1024;
constexpr qint64 deviceWriteBudget = 64 * 1024;
QString systemError(int error)
{
    return QString::fromLocal8Bit(strerror(error));
}
}

namespace Kwi
{
PtyProcess::PtyProcess(QObject *parent)
    : QObject{parent}
{
    m_metadataTimer.setSingleShot(true);
    m_metadataTimer.setInterval(std::chrono::seconds{1});

    connect(&m_metadataTimer, &QTimer::timeout, this, [this] {
        refreshProcessInfo();
    });
}

PtyProcess::~PtyProcess()
{
    m_readNotifier.reset();
    if (m_process) {
        m_process->disconnect(this); // clazy:exclude=unused-result-check

        if (device()) {
            device()->disconnect(this); // clazy:exclude=unused-result-check
        }
    }

    m_process.reset();
}

KPtyDevice *PtyProcess::device() const
{
    return m_process ? m_process->device() : nullptr;
}

bool PtyProcess::initialize(std::unique_ptr<ProcessController> controller, int masterFd, int columns, int rows, [[maybe_unused]] bool directRead)
{
    if (isRunning() || m_exitPending || m_refreshPending) {
        Q_EMIT errorOccurred(i18n("A terminal process is already running."));

        return false;
    }

    ++m_runGeneration;
    m_readNotifier.reset();
    m_process = std::move(controller);
    m_metadataTimer.stop();
    m_ioGeneration = 0;
    m_closing = false;
    m_exitPending = false;
    m_exitEmitted = false;
    m_readScheduled = false;
    m_exitCode = 0;
    m_pendingInput.clear();
    m_inputOffset = 0;

    connect(m_process.get(), &ProcessController::errorOccurred, this, &PtyProcess::errorOccurred);
    const QPointer<PtyProcess> guard{this};

    if (!m_process->open(masterFd)) {
        if (guard != nullptr) {
            m_process.reset();
        }

        return false;
    }

#ifdef Q_OS_MACOS
    if (directRead) {
        // Own the read notification for ordinary Darwin child sessions. Mixing
        // direct reads with KPty's reader can turn a drained packet into a false
        // EOF. KPty still owns the descriptor, writes and terminal operations.
        device()->setSuspended(true);
        m_readNotifier = std::make_unique<QSocketNotifier>(device()->masterFd(), QSocketNotifier::Read);
        connect(m_readNotifier.get(), &QSocketNotifier::activated, this, &PtyProcess::readOutput);
    } else
#endif
    {
        connect(device(), &QIODevice::readyRead, this, &PtyProcess::readOutput);
    }
    connect(device(), &QIODevice::bytesWritten, this, &PtyProcess::flushInput);
    connect(device(), &KPtyDevice::readEof, this, &PtyProcess::readOutput);
    connect(m_process.get(), &ProcessController::transportClosed, this, [this] {
        m_readNotifier.reset();
        m_pendingInput.clear();
        m_inputOffset = 0;
    });

    connect(m_process.get(), &ProcessController::started, this, [this] {
        const QPointer<PtyProcess> guard{this};
        // QPointer construction cannot invalidate this context-bound callback's receiver.
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage)
        refreshProcessInfo();

        if (guard != nullptr) {
            Q_EMIT started();
        }
    });

    connect(m_process.get(), &ProcessController::finished, this, &PtyProcess::processFinished);
    connect(m_process.get(), &ProcessController::processInfoChanged, this, &PtyProcess::processInfoChanged);
    resize(columns, rows);

    return bool(guard);
}

bool PtyProcess::attach(int masterFd, int columns, int rows)
{
    return initialize(attachedProcess(), masterFd, columns, rows) && m_process->start({});
}

bool PtyProcess::start(const TerminalLaunch &launch, int columns, int rows, int masterFd)
{
    if (const QString error{launch.validationError()}; !error.isEmpty()) {
        Q_EMIT errorOccurred(error);

        return false;
    }

    std::unique_ptr<ProcessController> controller;

    if (KSandbox::isFlatpak()) {
        controller = std::make_unique<HostProcess>();
    } else {
        controller = std::make_unique<NativeProcess>();
    }

    if (!initialize(std::move(controller), masterFd, columns, rows, masterFd < 0)) {
        return false;
    }

    return m_process->start(launch);
}

bool PtyProcess::write(const QByteArray &data)
{
    if (!device() || device()->masterFd() < 0 || m_closing || !isRunning()) {
        return false;
    }

    const qint64 queued{m_pendingInput.size() - m_inputOffset + device()->bytesToWrite()};

    if (data.size() > maximumPendingInput - queued) {
        Q_EMIT errorOccurred(i18n("The terminal input queue is full. Wait for the running program before pasting more text."));

        return false;
    }

    if (m_inputOffset != 0) {
        m_pendingInput.remove(0, m_inputOffset);
        m_inputOffset = 0;
    }

    m_pendingInput.append(data);
    invalidateProcessInfo(true);
    flushInput();

    return true;
}

void PtyProcess::flushInput()
{
    if (!device() || m_closing || device()->masterFd() < 0) {
        return;
    }

    auto *ptyDevice = device();
    const qint64 capacity{deviceWriteBudget - ptyDevice->bytesToWrite()};
    const qint64 remaining{m_pendingInput.size() - m_inputOffset};

    if (capacity > 0 && remaining > 0) {
        const qint64 accepted{ptyDevice->write(m_pendingInput.constData() + m_inputOffset, std::min(capacity, remaining))};

        if (accepted < 0) {
            Q_EMIT errorOccurred(i18n("Could not write to the terminal: %1", ptyDevice->errorString()));

            return;
        }

        m_inputOffset += accepted;
    }

    if (m_inputOffset == m_pendingInput.size()) {
        m_pendingInput.clear();
        m_inputOffset = 0;
    }
}

void PtyProcess::readOutput()
{
    if (!device()) {
        return;
    }

    auto *ptyDevice = device();
    QByteArray output = ptyDevice->isOpen() ? ptyDevice->read(readBudget) : QByteArray();
    bool readAgain{false};
    bool readEnded{false};
    int readFailure{0};
#ifdef Q_OS_MACOS
    // KPty opens the master nonblocking. Only our notifier reads ordinary Mac
    // child sessions; attached descriptors retain KPty's original EOF handling.
    if (m_readNotifier && ptyDevice->isOpen() && ptyDevice->masterFd() >= 0) {
        m_readNotifier->setEnabled(false);
        QElapsedTimer intake;
        intake.start();
        while (ptyDevice->bytesAvailable() == 0 && output.size() < readBudget && intake.nsecsElapsed() < 2'000'000) {
            char packet[64 * 1024];
            const auto count = ::read(ptyDevice->masterFd(), packet, std::min<qint64>(sizeof(packet), readBudget - output.size()));
            if (count > 0)
                output.append(packet, count);
            else if (count < 0 && errno == EINTR)
                continue;
            else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && output.size() < 32 * 1024 && intake.nsecsElapsed() < 1'000'000) {
                // Darwin's small tty packets wake a writer between reads. A
                // bounded poll lets it supply the next packet without a Cocoa
                // round trip. Never wait more than 1 ms or exceed the 2 ms
                // intake budget; idle sessions have no polling timer. Once a
                // useful batch is ready, yield it instead of delaying delivery
                // for another packet. The 256 KiB and 2 ms bounds still apply.
                pollfd next{ptyDevice->masterFd(), POLLIN, 0};
                if (::poll(&next, 1, 1) > 0)
                    continue;
                break;
            } else {
                readEnded = count == 0 || (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK);
                if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EIO)
                    readFailure = errno;
                break;
            }
        }
        readAgain = !readEnded && (output.size() >= readBudget || intake.nsecsElapsed() >= 2'000'000);
    }
#endif
    const QPointer<PtyProcess> guard{this};
    const auto generation = m_runGeneration;

    if (!output.isEmpty()) {
        invalidateProcessInfo(false);

        Q_EMIT dataReceived(output);

        if (guard == nullptr || generation != m_runGeneration) {
            return;
        }
    }

    if (readFailure) {
        Q_EMIT errorOccurred(i18n("Could not read from the terminal: %1", systemError(readFailure)));
        if (guard != nullptr && generation == m_runGeneration)
            close();
        return;
    }

    if (ptyDevice->masterFd() >= 0 && (readAgain || ptyDevice->bytesAvailable() > 0)) {
        // Suspend intake while draining buffered output to give the GUI time to process input.
        if (!m_readNotifier)
            ptyDevice->setSuspended(true);

        if (!m_readScheduled) {
            m_readScheduled = true;
            QTimer::singleShot(0, this, [this, generation] {
                if (generation != m_runGeneration) {
                    return;
                }

                m_readScheduled = false;

                if (device() && device()->masterFd() >= 0) {
                    if (!m_readNotifier)
                        device()->setSuspended(false);
                    readOutput();
                }
            });
        }

        return;
    }

    if (m_readNotifier && !readEnded)
        m_readNotifier->setEnabled(true);

    if (m_exitPending) {
        // QProcess::finished can arrive before KPtyDevice's final readyRead.
        int available{0};

        if (ptyDevice->masterFd() >= 0 && ioctl(ptyDevice->masterFd(), FIONREAD, &available) == 0 && available > 0) {
            if (traceEnabled())
                trace(QStringLiteral("tail-pending"), {{QStringLiteral("bytes"), available}});
            if (m_readNotifier)
                m_readNotifier->setEnabled(true);
            else
                ptyDevice->setSuspended(false);

            return;
        }

        closeMaster();
        finishExit();
    }
}

void PtyProcess::resize(int columns, int rows, QSize cellPixels)
{
    rows = std::clamp(rows, 1, int(USHRT_MAX));
    columns = std::clamp(columns, 1, int(USHRT_MAX));
    const int height{int(std::clamp<qint64>(qint64(rows) * cellPixels.height(), 0, USHRT_MAX))};
    const int width{int(std::clamp<qint64>(qint64(columns) * cellPixels.width(), 0, USHRT_MAX))};

    if (device() && device()->masterFd() >= 0 && !device()->setWinSize(rows, columns, height, width)) {
        Q_EMIT errorOccurred(i18n("Could not resize the terminal: %1", systemError(errno)));
    }
}

void PtyProcess::close()
{
    if (m_closing) {
        return;
    }

    m_closing = true;
    m_metadataTimer.stop();

    if (m_process) {
        m_process->close();
    }
}

void PtyProcess::closeMaster()
{
    if (m_process) {
        m_process->closeTransport();
    }
}

void PtyProcess::processFinished(int exitCode)
{
    if (traceEnabled())
        trace(QStringLiteral("process-finished"), {{QStringLiteral("exit_code"), exitCode}});
    m_metadataTimer.stop();
    m_exitCode = exitCode;
    m_exitPending = true;
    readOutput();
}

void PtyProcess::finishExit()
{
    if (!m_exitPending || m_exitEmitted) {
        return;
    }

    m_exitPending = false;
    m_exitEmitted = true;
    m_metadataTimer.stop();

    if (traceEnabled())
        trace(QStringLiteral("process-drained"), {{QStringLiteral("exit_code"), m_exitCode}});
    Q_EMIT exited(m_exitCode);
}

bool PtyProcess::isRunning() const
{
    return m_process && m_process->isRunning();
}

bool PtyProcess::isBusy() const
{
    return isRunning() && m_process->processInfo().busy;
}

QString PtyProcess::workingDirectory() const
{
    return m_process ? m_process->processInfo().directory : QString();
}

qint64 PtyProcess::processId() const
{
    return m_process ? m_process->processId() : -1;
}

QString PtyProcess::program() const
{
    return m_process ? m_process->program() : QString();
}

qint64 PtyProcess::foregroundProcessId() const
{
    return isRunning() ? m_process->processInfo().foregroundPid : -1;
}

QString PtyProcess::foregroundProcessName() const
{
    return isRunning() ? m_process->processInfo().foregroundName : QString();
}

SshProcessInfo PtyProcess::sshProcessInfo() const
{
    return isRunning() ? m_process->processInfo().ssh : SshProcessInfo{};
}

void PtyProcess::invalidateProcessInfo(bool input)
{
    ++m_ioGeneration;
    m_process->invalidateProcessInfo(input);

    if (!m_closing && !m_metadataTimer.isActive() && !m_refreshPending) {
        m_metadataTimer.start();
    }
}

void PtyProcess::refreshProcessInfo(std::function<void()> callback)
{
    if (callback) {
        m_refreshCallbacks.append(std::move(callback));
    }

    if (m_refreshPending) {
        return;
    }

    m_metadataTimer.stop();
    m_refreshPending = true;
    const auto ioGeneration = m_ioGeneration;
    const auto runGeneration = m_runGeneration;
    const QPointer<PtyProcess> guard{this};
    auto completed = [this, guard, ioGeneration, runGeneration] {
        if (!guard || runGeneration != m_runGeneration) {
            return;
        }

        m_refreshPending = false;

        if (isRunning() && !m_closing && ioGeneration != m_ioGeneration) {
            m_metadataTimer.start();
        }

        const auto callbacks = std::exchange(m_refreshCallbacks, {});

        for (const auto &callback : callbacks) {
            callback();

            if (guard == nullptr) {
                return;
            }
        }
    };

    if (m_process) {
        m_process->refreshProcessInfo(std::move(completed));
    } else {
        completed();
    }
}
}
