// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "kwiterminal_export.h"
#include <QMap>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>
#include <optional>

namespace Kwi
{
struct KWI_TERMINAL_EXPORT TerminalLaunch {
    // An empty program selects the system shell on the native or Flatpak host.
    QString program{};
    QStringList arguments{};
    QString directory{};

    // Capture the inherited native environment when the launch is requested.
    QProcessEnvironment inheritedEnvironment{QProcessEnvironment::systemEnvironment()};

    // Caller overrides of terminal defaults. Null removes a variable.
    QMap<QString, std::optional<QString>> environmentChanges{};

    /** Check syntax without resolving paths. An empty result means valid. */
    [[nodiscard]] QString validationError() const;
    [[nodiscard]] QMap<QString, std::optional<QString>> resolvedEnvironmentChanges() const;
    [[nodiscard]] QProcessEnvironment environment() const;
};

[[nodiscard]] KWI_TERMINAL_EXPORT QString systemShell(const QProcessEnvironment &environment = QProcessEnvironment::systemEnvironment());
[[nodiscard]] bool interactiveShellArguments(const QStringList &arguments);
}
