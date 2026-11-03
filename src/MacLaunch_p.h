// SPDX-FileCopyrightText: 2026 Kwi contributors
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QDir>

namespace Kwi
{
inline QString macLaunchDirectory(const QString &directory, bool launchedByLaunchd)
{
    // Launch Services hands a fresh Finder/Dock app to launchd with cwd "/".
    // A CLI caller's root directory is meaningful and must remain intact.
    return launchedByLaunchd && directory == QDir::rootPath() ? QDir::homePath() : directory;
}
}
