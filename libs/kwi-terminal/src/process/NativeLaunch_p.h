// SPDX-FileCopyrightText: 2026 Kwi contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "ProcessLaunch.h"

namespace Kwi
{
[[nodiscard]] QStringList nativeLaunchArguments(const TerminalLaunch &launch, const QString &executable);
}
