// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QString>
#include <QStringList>

class KPtyProcess;

namespace Kwi::ProcessScope
{
void startNative(KPtyProcess &process);
[[nodiscard]] QStringList hostCommand(const QString &launcher, const QStringList &arguments);
}
