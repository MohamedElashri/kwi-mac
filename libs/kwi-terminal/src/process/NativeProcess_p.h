// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "ProcessController_p.h"
#include <QString>
#include <QTimer>
#include <functional>
#include <memory>

class KPtyProcess;

namespace Kwi
{
class NativeProcess final : public ProcessController // clazy:exclude=ctor-missing-parent-argument,missing-qobject-macro
{
public:
    NativeProcess();
    ~NativeProcess() override;

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

private:
    void signalSession(int signal) const;
    void beginClose();

    std::unique_ptr<KPtyProcess> m_process;
    QTimer m_terminateTimer;
    QTimer m_killTimer;
    ProcessInfo m_info;
    QString m_initialExecutable;
    qint64 m_pid{-1};
    int m_closingForegroundGroup{-1};
    bool m_interactiveShell{false};
    bool m_closing{false};
};
}
