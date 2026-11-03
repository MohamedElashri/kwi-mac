// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "ProcessInfo_p.h"
#include <QDBusUnixFileDescriptor>
#include <QObject>
#include <QString>
#include <functional>

class KPtyDevice;

namespace Kwi
{
struct TerminalLaunch;

class ProcessController : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

    virtual bool open(int masterFd) = 0;
    [[nodiscard]] virtual KPtyDevice *device() const = 0;
    virtual bool start(const TerminalLaunch &launch) = 0;
    virtual void close() = 0;

    [[nodiscard]] virtual bool isRunning() const = 0;
    [[nodiscard]] virtual qint64 processId() const = 0;
    [[nodiscard]] virtual QString program() const = 0;
    [[nodiscard]] virtual ProcessInfo processInfo() const = 0;

    virtual void refreshProcessInfo(std::function<void()> completed = {}) = 0;
    virtual void invalidateProcessInfo(bool input) = 0;

    /** Close the transport after output has drained, or during child cleanup. */
    void closeTransport();

Q_SIGNALS:
    void started();
    void finished(int exitCode);
    void errorOccurred(const QString &message);
    void transportClosed();
    void processInfoChanged();

protected:
    /** Duplicate a master supplied by the caller. */
    bool prepareMaster(int masterFd);

    QDBusUnixFileDescriptor m_master;
};
}
