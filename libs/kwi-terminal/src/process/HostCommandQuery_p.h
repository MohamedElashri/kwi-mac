// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "kwiterminal_export.h"
#include <QByteArray>
#include <QDBusConnection>
#include <QFile>
#include <QHash>
#include <QObject>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <memory>
#include <optional>

class QSocketNotifier;
class QDBusPendingCallWatcher;

namespace Kwi
{
class KWI_TERMINAL_EXPORT HostCommandQuery final : public QObject
{
    Q_OBJECT

public:
    enum class Result {
        Success,
        FailedToStart,
        Failed
    };
    Q_ENUM(Result)

    explicit HostCommandQuery(QDBusConnection bus, QObject *parent = nullptr);
    ~HostCommandQuery() override;

    void start(const QStringList &arguments, const QString &directory = QStringLiteral("/"), const QProcessEnvironment &environment = {});

Q_SIGNALS:
    void finished(Kwi::HostCommandQuery::Result result, const QByteArray &output);

private Q_SLOTS:
    void hostExited(uint pid, uint status);

private:
    enum class Phase {
        Ready,
        Running,
        Finished
    };

    void readPipe(bool standardError);
    void maybeFinish();
    void finish(Result result);
    void cancelCommand();

    QDBusConnection m_bus;
    QDBusPendingCallWatcher *m_spawn{nullptr};
    QHash<uint, uint> m_earlyExits;
    QFile m_outputPipe;
    QFile m_errorPipe;
    std::unique_ptr<QSocketNotifier> m_outputNotifier;
    std::unique_ptr<QSocketNotifier> m_errorNotifier;
    QByteArray m_output;
    QTimer m_deadline;
    std::optional<uint> m_exitStatus;
    qsizetype m_errorBytes{0};
    uint m_pid{0};
    Phase m_phase{Phase::Ready};
    bool m_cancelRequested{false};
};
}
