// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "ProcessInfo_p.h"
#include "kwiterminal_export.h"
#include <QByteArray>
#include <QList>
#include <QObject>
#include <QSize>
#include <QString>
#include <QTimer>
#include <functional>
#include <memory>

class KPtyDevice;
class QSocketNotifier;

namespace Kwi
{
class ProcessController;
struct TerminalLaunch;

class KWI_TERMINAL_EXPORT PtyProcess final : public QObject
{
    Q_OBJECT

public:
    explicit PtyProcess(QObject *parent = nullptr);
    ~PtyProcess() override;

    bool start(const TerminalLaunch &launch, int columns, int rows, int masterFd = -1);

    /** Attach a duplicate of a caller-owned master. Never signal or reap its child. */
    bool attach(int masterFd, int columns, int rows);

    /** Rejects the entire input and emits errorOccurred if the bounded queue is full. */
    bool write(const QByteArray &data);
    void resize(int columns, int rows, QSize cellPixels = {});

    /** Hang up owned sessions, escalating to SIGTERM and SIGKILL if necessary.
     * Attached terminals are detached without signalling their child.
     */
    void close();

    [[nodiscard]] bool isRunning() const;
    [[nodiscard]] bool isBusy() const;
    [[nodiscard]] QString workingDirectory() const;
    [[nodiscard]] qint64 processId() const;
    [[nodiscard]] QString program() const;
    [[nodiscard]] qint64 foregroundProcessId() const;
    [[nodiscard]] QString foregroundProcessName() const;
    [[nodiscard]] SshProcessInfo sshProcessInfo() const;

    void refreshProcessInfo(std::function<void()> callback = {});

Q_SIGNALS:
    void started();
    void dataReceived(const QByteArray &data);
    void exited(int exitCode);
    void errorOccurred(const QString &message);
    void processInfoChanged();

private:
    [[nodiscard]] KPtyDevice *device() const;
    bool initialize(std::unique_ptr<ProcessController> controller, int masterFd, int columns, int rows, bool directRead = false);
    void invalidateProcessInfo(bool input);
    void readOutput();
    void flushInput();
    void processFinished(int exitCode);
    void closeMaster();
    void finishExit();

    std::unique_ptr<ProcessController> m_process;
    QTimer m_metadataTimer;
    std::unique_ptr<QSocketNotifier> m_readNotifier;
    QList<std::function<void()>> m_refreshCallbacks;
    QByteArray m_pendingInput;
    qsizetype m_inputOffset{0};
    quint64 m_ioGeneration{0};
    quint64 m_runGeneration{0};
    int m_exitCode{0};
    bool m_closing{false};
    bool m_exitPending{false};
    bool m_readScheduled{false};
    bool m_exitEmitted{false};
    bool m_refreshPending{false};
};

}
