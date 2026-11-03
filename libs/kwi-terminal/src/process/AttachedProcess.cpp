// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "AttachedProcess_p.h"
#include "NativeProcessInfo_p.h"
#include "ProcessController_p.h"

#include <KLocalizedString>
#include <KPtyDevice>
#include <QPointer>
#include <QTimer>
#include <utility>

namespace Kwi
{
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

    // The base method takes the callback by value so asynchronous backends can retain it. clazy:exclude-next-line=function-args-by-ref
    void refreshProcessInfo(std::function<void()> completed = {}) override
    {
        const auto info = m_active ? NativeProcessInfo::observe(device()->masterFd(), -1, {}, false, m_info) : ProcessInfo{};
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
