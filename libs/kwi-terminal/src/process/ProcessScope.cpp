// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "ProcessScope_p.h"

#include <KPtyProcess>
#include <QByteArray>
#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDBusUnixFileDescriptor>
#include <QDBusVariant>
#include <QGuiApplication>
#include <QList>
#include <QLoggingCategory>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QTimer>
#include <QUuid>
#include <QVariant>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <utility>
#ifdef Q_OS_LINUX
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace
{
QString unitName()
{
    const QString desktopFile{QGuiApplication::desktopFileName()};
    const QByteArray identity{(desktopFile.isEmpty() ? QCoreApplication::applicationName() : desktopFile).toUtf8()};
    QString escaped;
    escaped.reserve(identity.size());

    for (const unsigned char byte : identity) {
        if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') || byte == '.' || byte == ':' || byte == '_') {
            escaped += QLatin1Char(byte);
        } else {
            escaped += QStringLiteral("\\x%1").arg(byte, 2, 16, QLatin1Char('0'));
        }
    }

    return QStringLiteral("app-%1-%2.scope").arg(escaped, QUuid::createUuid().toString(QUuid::Id128));
}

#ifdef Q_OS_LINUX
Q_STATIC_LOGGING_CATEGORY(scopeLog, "org.kde.kwi.terminal.scope", QtWarningMsg)

constexpr int scopeTimeoutMs = 5000;

using Properties = QList<std::pair<QString, QDBusVariant>>;
using AuxiliaryUnits = QList<std::pair<QString, Properties>>;

QDBusMessage managerCall(const QString &method)
{
    return QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.systemd1"),
                                          QStringLiteral("/org/freedesktop/systemd1"),
                                          QStringLiteral("org.freedesktop.systemd1.Manager"),
                                          method);
}

class ScopeLaunch final : public QObject
{
    Q_OBJECT

public:
    explicit ScopeLaunch(KPtyProcess *process)
        : QObject{process}
    {
        int descriptors[2];

        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, descriptors) == 0) {
            m_reader.giveFileDescriptor(descriptors[0]);
            m_writer.giveFileDescriptor(descriptors[1]);
        }

        connect(process, &QProcess::stateChanged, this, [this](QProcess::ProcessState state) {
            if (state == QProcess::NotRunning) {
                complete();
            }
        });
        m_deadline.setSingleShot(true);
        connect(&m_deadline, &QTimer::timeout, this, [this] {
            qCDebug(scopeLog) << "Scope registration timed out:" << m_unit;
            complete();
        });
    }

    ~ScopeLaunch() override
    {
        release();
    }

    [[nodiscard]] int reader() const
    {
        return m_reader.fileDescriptor();
    }

    [[nodiscard]] int writer() const
    {
        return m_writer.fileDescriptor();
    }

    void registerProcess(qint64 pid)
    {
        m_reader = {};
        QDBusUnixFileDescriptor identity;
        identity.giveFileDescriptor(int(syscall(SYS_pidfd_open, pid_t(pid), 0)));

        if (!identity.isValid()) {
            qCDebug(scopeLog) << "Could not open a pidfd for scope registration:" << QString::fromLocal8Bit(strerror(errno));
            complete();

            return;
        }

        qDBusRegisterMetaType<std::pair<QString, QDBusVariant>>();
        qDBusRegisterMetaType<Properties>();
        qDBusRegisterMetaType<std::pair<QString, Properties>>();
        qDBusRegisterMetaType<AuxiliaryUnits>();
        qDBusRegisterMetaType<QList<QDBusUnixFileDescriptor>>();

        if (!m_bus.connect(QStringLiteral("org.freedesktop.systemd1"),
                           QStringLiteral("/org/freedesktop/systemd1"),
                           QStringLiteral("org.freedesktop.systemd1.Manager"),
                           QStringLiteral("JobRemoved"),
                           this,
                           SLOT(jobRemoved(uint, QDBusObjectPath, QString, QString)))) {
            qCDebug(scopeLog) << "Could not connect to systemd job notifications:" << m_bus.lastError();
            complete();

            return;
        }

        auto *service = new QDBusServiceWatcher(QStringLiteral("org.freedesktop.systemd1"), m_bus, QDBusServiceWatcher::WatchForUnregistration, this);
        connect(service, &QDBusServiceWatcher::serviceUnregistered, this, [this] {
            qCDebug(scopeLog) << "The systemd manager disappeared during scope registration:" << m_unit;
            complete();
        });
        m_deadline.start(std::chrono::milliseconds{scopeTimeoutMs});
        auto *watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(managerCall(QStringLiteral("Subscribe")), scopeTimeoutMs), this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, identity] {
            const QDBusPendingReply<> reply{*watcher};
            watcher->deleteLater();

            if (!m_deadline.isActive()) {
                return;
            }

            // The shared connection may already be subscribed by another session or its host.
            if (reply.isError() && reply.error().name() != QLatin1String("org.freedesktop.systemd1.AlreadySubscribed")) {
                qCDebug(scopeLog) << "Could not subscribe to systemd job notifications:" << reply.error();
                complete();

                return;
            }

            startScope(identity);
        });
    }

private Q_SLOTS:
    void jobRemoved(uint, const QDBusObjectPath &, const QString &unit, const QString &result)
    {
        if (unit == m_unit) {
            if (result != QLatin1String("done")) {
                qCDebug(scopeLog) << "Scope registration failed:" << unit << result;
            }

            complete();
        }
    }

private:
    void startScope(const QDBusUnixFileDescriptor &identity)
    {
        const Properties properties{
            {QStringLiteral("Description"), QDBusVariant{QGuiApplication::applicationDisplayName()}},
            {QStringLiteral("Slice"), QDBusVariant{QStringLiteral("app.slice")}},
            {QStringLiteral("CollectMode"), QDBusVariant{QStringLiteral("inactive-or-failed")}},
            {QStringLiteral("ManagedOOMMemoryPressure"), QDBusVariant{QStringLiteral("kill")}},
            {QStringLiteral("JobTimeoutUSec"), QDBusVariant{quint64{scopeTimeoutMs} * 1000}},
            // Cancellation may reap the child while this request is pending.
            {QStringLiteral("PIDFDs"), QDBusVariant{QVariant::fromValue(QList<QDBusUnixFileDescriptor>{identity})}},
        };
        auto call = managerCall(QStringLiteral("StartTransientUnit"));
        call.setArguments({m_unit, QStringLiteral("fail"), QVariant::fromValue(properties), QVariant::fromValue(AuxiliaryUnits{})});
        auto *watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(call, scopeTimeoutMs), this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher] {
            const QDBusPendingReply<QDBusObjectPath> reply{*watcher};
            watcher->deleteLater();

            if (reply.isError()) {
                qCDebug(scopeLog) << "Could not register the terminal scope:" << reply.error();
                complete();
            }
        });
    }

    void release()
    {
        if (m_writer.isValid()) {
            ssize_t result;

            do {
                result = send(m_writer.fileDescriptor(), "x", 1, MSG_NOSIGNAL);
            } while (result < 0 && errno == EINTR);

            m_writer = {};
        }
    }

    void complete()
    {
        m_deadline.stop();
        release();
        deleteLater();
    }

    QDBusConnection m_bus{QDBusConnection::sessionBus()};
    QDBusUnixFileDescriptor m_reader;
    QDBusUnixFileDescriptor m_writer;
    QTimer m_deadline{this};
    const QString m_unit{unitName()};
};
#endif
}

namespace Kwi::ProcessScope
{
void startNative(KPtyProcess &process)
{
#ifdef Q_OS_LINUX
    auto *scope = new ScopeLaunch(&process);
    const int reader{scope->reader()};
    const int writer{scope->writer()};

    if (reader >= 0) {
        const auto modifier = process.childProcessModifier();
        process.setChildProcessModifier([reader, writer, modifier, &process] {
            if (modifier) {
                modifier();
            }

            // Wait before exec. EOF aborts startup if the parent disappears.
            close(writer);
            char value;
            ssize_t result;

            do {
                result = read(reader, &value, 1);
            } while (result < 0 && errno == EINTR);

            if (result != 1) {
                process.failChildProcessModifier("Could not wait for the terminal scope", result < 0 ? errno : ECANCELED);
            }

            close(reader);
        });
        const QPointer<ScopeLaunch> guard{scope};
        process.start();

        if (guard) {
            process.setChildProcessModifier(modifier);

            if (process.processId() > 0) {
                scope->registerProcess(process.processId());
            } else {
                delete scope;
            }
        }

        return;
    }

    delete scope;
#endif
    process.start();
}

QStringList hostCommand(const QString &launcher, const QStringList &arguments)
{
    // The launcher executes in place. Its failures are ordinary process exits,
    // so retrying here could run a user's command twice.
    return QStringList{launcher,
                       QStringLiteral("--user"),
                       QStringLiteral("--scope"),
                       QStringLiteral("--quiet"),
                       QStringLiteral("--collect"),
                       QStringLiteral("--property=ManagedOOMMemoryPressure=kill"),
                       QStringLiteral("--expand-environment=no"),
                       QStringLiteral("--slice=app.slice"),
                       QStringLiteral("--description=%1").arg(QGuiApplication::applicationDisplayName()),
                       QStringLiteral("--unit=%1").arg(unitName()),
                       QStringLiteral("--")}
    + arguments;
}
}

#ifdef Q_OS_LINUX
#include "ProcessScope.moc"
#endif
