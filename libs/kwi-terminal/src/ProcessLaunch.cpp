// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "ProcessLaunch.h"
#include "process/NativeLaunch_p.h"

#include <KLocalizedString>
#include <KUser>
#include <QDir>
#include <QFileInfo>
#ifdef Q_OS_MACOS
#include <QLocale>
#include <xlocale.h>
#endif
#include <algorithm>

namespace Kwi
{
QString systemShell(const QProcessEnvironment &environment)
{
#ifdef Q_OS_MACOS
    for (const auto &shell : {KUser{}.shell(), environment.value(QStringLiteral("SHELL")), QStringLiteral("/bin/sh")}) {
        const QFileInfo file{shell};

        if (file.isAbsolute() && file.isFile() && file.isExecutable()) {
            return shell;
        }
    }

    return QStringLiteral("/bin/sh");
#else
    QString shell{environment.value(QStringLiteral("SHELL"))};

    if (shell.isEmpty()) {
        shell = KUser{}.shell();
    }

    return shell.isEmpty() ? QStringLiteral("/bin/sh") : shell;
#endif
}

QStringList nativeLaunchArguments(const TerminalLaunch &launch, const QString &executable)
{
#ifdef Q_OS_MACOS
    // An explicit command is never rewritten. Known automatically selected shells
    // run their normal login setup; unknown custom shells retain their own argv.
    const QString name{QFileInfo(executable).fileName()};
    const QStringList loginShells{QStringLiteral("sh"),
                                  QStringLiteral("bash"),
                                  QStringLiteral("zsh"),
                                  QStringLiteral("ksh"),
                                  QStringLiteral("fish"),
                                  QStringLiteral("csh"),
                                  QStringLiteral("tcsh")};

    if (launch.program.isEmpty() && launch.arguments.isEmpty() && loginShells.contains(name)) {
        return {QStringLiteral("-l")};
    }
#else
    Q_UNUSED(executable)
#endif

    return launch.arguments;
}

QMap<QString, std::optional<QString>> TerminalLaunch::resolvedEnvironmentChanges() const
{
    QMap<QString, std::optional<QString>> result{{QStringLiteral("TERM"), QStringLiteral("xterm-256color")},
                                                 {QStringLiteral("COLORTERM"), QStringLiteral("truecolor")},
                                                 {QStringLiteral("TERM_PROGRAM"), QStringLiteral("kwi")},
                                                 {QStringLiteral("COLUMNS"), std::nullopt},
                                                 {QStringLiteral("LINES"), std::nullopt},
                                                 {QStringLiteral("TERMCAP"), std::nullopt}};
    result.insert(environmentChanges);
#ifdef Q_OS_MACOS
    // Finder/Launch Services need not supply a locale. Give native children a
    // valid UTF-8 default without running a shell to discover environment data.
    // Explicit caller values (including empty values/removals) take precedence.
    const auto specified = [&](const QString &name) {
        return inheritedEnvironment.contains(name) || environmentChanges.contains(name);
    };
    if (!specified(QStringLiteral("LANG")) && !specified(QStringLiteral("LC_ALL")) && !specified(QStringLiteral("LC_CTYPE"))) {
        QString locale{QLocale::system().name() + QStringLiteral(".UTF-8")};
        if (const auto native = newlocale(LC_CTYPE_MASK, locale.toLocal8Bit().constData(), nullptr)) {
            freelocale(native);
        } else {
            locale = QStringLiteral("en_US.UTF-8");
        }
        result.insert(QStringLiteral("LANG"), locale);
    }
#endif

    return result;
}

QString TerminalLaunch::validationError() const
{
    if ((!directory.isEmpty() && !QDir::isAbsolutePath(directory)) || directory.contains(QChar::Null)) {
        return i18nd("kwi-terminal", "A terminal's initial directory must be an absolute path.");
    }

    if (program.contains(QChar::Null) || std::ranges::any_of(arguments, [](const QString &argument) {
            return argument.contains(QChar::Null);
        })) {
        return i18nd("kwi-terminal", "The command contains a null character.");
    }

    const auto validAssignment = [](const QString &name, const QString &value) {
        return !name.isEmpty() && !name.contains(QChar::Null) && !name.contains(QLatin1Char('=')) && !value.contains(QChar::Null);
    };

    const auto inheritedNames = inheritedEnvironment.keys();

    if (std::ranges::any_of(inheritedNames, [&](const QString &name) {
            return !validAssignment(name, inheritedEnvironment.value(name));
        })) {
        return i18nd("kwi-terminal", "The terminal environment contains an invalid assignment.");
    }

    for (auto it = environmentChanges.cbegin(); it != environmentChanges.cend(); ++it) {
        if (!validAssignment(it.key(), it.value().value_or(QString{}))) {
            return i18nd("kwi-terminal", "The terminal environment contains an invalid assignment.");
        }
    }

    return {};
}

QProcessEnvironment TerminalLaunch::environment() const
{
    auto result = inheritedEnvironment;
    const auto changes{resolvedEnvironmentChanges()};

    for (auto it = changes.cbegin(); it != changes.cend(); ++it) {
        if (it.value()) {
            result.insert(it.key(), *it.value());
        } else {
            result.remove(it.key());
        }
    }

    return result;
}
}
