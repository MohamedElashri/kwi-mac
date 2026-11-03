// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "HostCommandQuery_p.h"
#include "HostProcessProtocol_p.h"
#include "ProcessQueryLimits_p.h"

#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QSocketNotifier>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

namespace Kwi
{
namespace
{
namespace Protocol = HostProcessProtocol;

bool openPipe(QFile &reader, QDBusUnixFileDescriptor &writer)
{
    int descriptors[2];

    if (pipe2(descriptors, O_CLOEXEC) < 0) {
        return false;
    }

    // Keep the host command's stdout/stderr blocking while the GUI reads asynchronously.
    if (fcntl(descriptors[0], F_SETFL, O_NONBLOCK) < 0 || !reader.open(descriptors[0], QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        ::close(descriptors[0]);
        ::close(descriptors[1]);

        return false;
    }

    writer.giveFileDescriptor(descriptors[1]);

    return true;
}
}

HostCommandQuery::HostCommandQuery(QDBusConnection bus, QObject *parent)
    : QObject{parent}
    , m_bus{std::move(bus)}
{
    m_deadline.setSingleShot(true);
    m_deadline.setInterval(ProcessQueryLimits::TimeoutMs);

    connect(&m_deadline, &QTimer::timeout, this, [this] {
        finish(Result::Failed);
    });
}

HostCommandQuery::~HostCommandQuery()
{
    cancelCommand();
}

void HostCommandQuery::start(const QStringList &arguments, const QString &directory, const QProcessEnvironment &environment)
{
    Q_ASSERT(m_phase == Phase::Ready);

    if (m_phase != Phase::Ready) {
        return;
    }

    m_phase = Phase::Running;
    const bool connected = m_bus.connect(QString::fromLatin1(Protocol::Service),
                                         QString::fromLatin1(Protocol::Object),
                                         QString::fromLatin1(Protocol::Interface),
                                         QStringLiteral("HostCommandExited"),
                                         this,
                                         SLOT(hostExited(uint, uint)));
    QDBusUnixFileDescriptor outputWriter;
    QDBusUnixFileDescriptor errorWriter;

    if (!connected || !openPipe(m_outputPipe, outputWriter) || !openPipe(m_errorPipe, errorWriter)) {
        finish(Result::FailedToStart);

        return;
    }

    m_outputNotifier = std::make_unique<QSocketNotifier>(m_outputPipe.handle(), QSocketNotifier::Read);
    m_errorNotifier = std::make_unique<QSocketNotifier>(m_errorPipe.handle(), QSocketNotifier::Read);

    connect(m_outputNotifier.get(), &QSocketNotifier::activated, this, [this] {
        readPipe(false);
    });

    connect(m_errorNotifier.get(), &QSocketNotifier::activated, this, [this] {
        readPipe(true);
    });

    const Protocol::FileDescriptors descriptors{{1, outputWriter}, {2, errorWriter}};
    m_spawn = new QDBusPendingCallWatcher(
        m_bus.asyncCall(Protocol::command(directory, Protocol::boundedQueryCommand(arguments), environment, descriptors, Protocol::CommandFlags::WatchBus),
                        Protocol::CallTimeoutMs),
        this);

    connect(m_spawn, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *watcher) {
        const QDBusPendingReply<uint> reply{*watcher};

        // Release the pending call's pipe writers so EOF can be observed.
        m_spawn = nullptr;
        watcher->deleteLater();

        if (reply.isError() || reply.value() == 0) {
            finish(Result::FailedToStart);

            return;
        }

        m_pid = reply.value();
        const auto exit = m_earlyExits.constFind(m_pid);

        if (exit != m_earlyExits.cend()) {
            m_exitStatus = exit.value();
        }

        m_earlyExits.clear();
        maybeFinish();
    });

    m_deadline.start();
}

void HostCommandQuery::readPipe(bool standardError)
{
    if (m_phase != Phase::Running) {
        return;
    }

    QFile &pipe = standardError ? m_errorPipe : m_outputPipe;
    auto &notifier = standardError ? m_errorNotifier : m_outputNotifier;
    char buffer[4096];

    while (true) {
        const ssize_t count{::read(pipe.handle(), buffer, sizeof(buffer))};

        if (count > 0) {
            const qsizetype previous{standardError ? m_errorBytes : m_output.size()};

            if (count > ProcessQueryLimits::MaximumOutputBytes - previous) {
                finish(Result::Failed);

                return;
            }

            if (standardError) {
                m_errorBytes += count;
            } else {
                m_output.append(buffer, count);
            }
        } else if (count == 0) {
            notifier->setEnabled(false);
            pipe.close();
            maybeFinish();

            return;
        } else if (errno == EINTR) {
            continue;
        } else if (errno == EAGAIN) {
            return;
        } else {
            finish(Result::Failed);

            return;
        }
    }
}

void HostCommandQuery::hostExited(uint pid, uint status)
{
    if (m_phase != Phase::Running) {
        return;
    }

    if (m_pid == 0) {
        // Bound queued exit signals from other sessions while awaiting the launch reply.
        if (m_earlyExits.size() >= 128) {
            finish(Result::Failed);
        } else {
            m_earlyExits.insert(pid, status);
        }
    } else if (pid == m_pid) {
        m_exitStatus = status;
        maybeFinish();
    }
}

void HostCommandQuery::maybeFinish()
{
    if (m_exitStatus && !m_outputPipe.isOpen() && !m_errorPipe.isOpen()) {
        finish(WIFEXITED(*m_exitStatus) && WEXITSTATUS(*m_exitStatus) == 0 ? Result::Success : Result::Failed);
    }
}

void HostCommandQuery::finish(Result result)
{
    if (m_phase == Phase::Finished) {
        return;
    }

    m_phase = Phase::Finished;
    m_deadline.stop();

    if (m_outputNotifier) {
        m_outputNotifier->setEnabled(false);
    }

    if (m_errorNotifier) {
        m_errorNotifier->setEnabled(false);
    }

    m_outputPipe.close();
    m_errorPipe.close();

    if (result != Result::Success) {
        cancelCommand();
    }

    const QByteArray output{result == Result::Success ? std::exchange(m_output, {}) : QByteArray()};

    Q_EMIT finished(result, output);
}

void HostCommandQuery::cancelCommand()
{
    if (m_cancelRequested || m_exitStatus) {
        return;
    }

    m_cancelRequested = true;

    if (m_pid) {
        (void)m_bus.asyncCall(Protocol::signal(m_pid, SIGKILL, true), Protocol::CallTimeoutMs);
    } else if (m_spawn) {
        // Keep the watcher alive to cancel a command launched after its owner is destroyed.
        auto *watcher = std::exchange(m_spawn, nullptr);
        watcher->disconnect(this); // clazy:exclude=unused-result-check
        watcher->setParent(nullptr);

        connect(watcher, &QDBusPendingCallWatcher::finished, watcher, [bus = m_bus](QDBusPendingCallWatcher *watcher) {
            const QDBusPendingReply<uint> reply{*watcher};
            watcher->deleteLater();

            if (!reply.isError() && reply.value()) {
                (void)bus.asyncCall(Protocol::signal(reply.value(), SIGKILL, true), Protocol::CallTimeoutMs);
            }
        });
    }
}
}
