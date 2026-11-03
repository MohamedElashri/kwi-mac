// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "ProcessController_p.h"

#include <KLocalizedString>
#include <KPtyDevice>
#include <cerrno>
#include <cstring>

namespace Kwi
{
bool ProcessController::prepareMaster(int masterFd)
{
    m_master = {};

    if (masterFd >= 0) {
        m_master = QDBusUnixFileDescriptor(masterFd);

        if (!m_master.isValid()) {
            Q_EMIT errorOccurred(i18n("Could not attach to the terminal: %1", QString::fromLocal8Bit(strerror(errno))));

            return false;
        }
    }

    return true;
}

void ProcessController::closeTransport()
{
    if (device()) {
        device()->close();
    }

    m_master = {};

    Q_EMIT transportClosed();
}

}
