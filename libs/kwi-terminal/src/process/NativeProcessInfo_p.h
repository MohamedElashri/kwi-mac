// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "ProcessInfo_p.h"
#include <QByteArray>
#include <QString>
#include <QtTypes>
#include <sys/types.h>

namespace Kwi::NativeProcessInfo
{
[[nodiscard]] ProcessInfo observe(int masterFd, qint64 leader, const QString &initialExecutable, bool interactiveShell, const ProcessInfo &previous);
[[nodiscard]] pid_t processInGroup(pid_t group, pid_t session);
[[nodiscard]] QString directory(pid_t pid);
[[nodiscard]] QString executable(pid_t pid);
/** Native NUL-separated argv, bounded by ProcessQueryLimits. */
[[nodiscard]] QByteArray commandLine(pid_t pid);
/** Opaque process-lifetime token; zero means unavailable. */
[[nodiscard]] quint64 startTime(pid_t pid);
/** Find a surviving member when the process-group leader has exited. */
[[nodiscard]] pid_t groupMember(pid_t group, pid_t session);
}
