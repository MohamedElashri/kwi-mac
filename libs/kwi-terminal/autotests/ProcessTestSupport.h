// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ProcessLaunch.h"

#include <QChar>
#include <QString>
#include <QStringList>

[[nodiscard]] inline Kwi::TerminalLaunch
testLaunch(const QString &program, const QStringList &arguments = {}, const QString &directory = {}, const QStringList &environment = {})
{
    Kwi::TerminalLaunch launch{.program = program, .arguments = arguments, .directory = directory};

    for (const auto &entry : environment) {
        const auto separator = entry.indexOf(QLatin1Char('='));
        launch.environmentChanges.insert(entry.left(separator), entry.mid(separator + 1));
    }

    return launch;
}
