// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "settings/LaunchSettings.h"
#include "ProcessLaunch.h"

#include <QByteArray>
#include <QChar>
#include <QDir>
#include <QLatin1Char>
#include <QMetaType>
#include <QObject>
#include <QScopeGuard>
#include <QString>
#include <QStringList>
#include <QStringLiteral>
#include <QTest>

#include <optional>

Q_DECLARE_METATYPE(Kwi::LaunchSettings)
Q_DECLARE_METATYPE(Kwi::TerminalLaunch)

using namespace Kwi;

class LaunchSettingsTest final : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

private Q_SLOTS:
    void directoryPrecedence();
    void commandIsOneUnit();
    void environmentIsLiteralAndCaptured();
    void terminalDefaultsKeepTheRequestedRecipe();
    void validatesRecipes_data();
    void validatesRecipes();
    void validatesEnvironment_data();
    void validatesEnvironment();
    void validatesLaunchSnapshots_data();
    void validatesLaunchSnapshots();
};

// An explicit launch directory overrides the preset's directory policy.
void LaunchSettingsTest::directoryPrecedence()
{
    LaunchSettings settings;
    LaunchRequest request{.contextDirectory = QStringLiteral("/context")};
    QCOMPARE(resolveLaunch(settings, request).directory, QStringLiteral("/context"));
    QCOMPARE(resolveLaunch(settings).directory, QDir::homePath());
    settings.directoryMode = LaunchSettings::DirectoryMode::Home;
    QCOMPARE(resolveLaunch(settings, request).directory, QDir::homePath());
    settings.directoryMode = LaunchSettings::DirectoryMode::Custom;
    settings.directory = QStringLiteral("/project");
    QCOMPARE(resolveLaunch(settings, request).directory, QStringLiteral("/project"));
    request.directory = QStringLiteral("/explicit");

    for (auto mode : {LaunchSettings::DirectoryMode::Current, LaunchSettings::DirectoryMode::Home, LaunchSettings::DirectoryMode::Custom}) {
        settings.directoryMode = mode;
        QCOMPARE(resolveLaunch(settings, request).directory, QStringLiteral("/explicit"));
    }

    request.directory = QStringLiteral("relative");
    QCOMPARE(resolveLaunch(settings, request).directory, QDir::current().absoluteFilePath(QStringLiteral("relative")));
}

// An explicit command replaces both the preset's program and its arguments.
void LaunchSettingsTest::commandIsOneUnit()
{
    LaunchSettings settings{.command = {QStringLiteral("/bin/sh"), QStringLiteral("-l"), QStringLiteral("literal $HOME")}};
    auto launch = resolveLaunch(settings);
    QCOMPARE(launch.program, QStringLiteral("/bin/sh"));
    QCOMPARE(launch.arguments, QStringList({QStringLiteral("-l"), QStringLiteral("literal $HOME")}));
    launch = resolveLaunch(settings, {.command = QStringList{QStringLiteral("/bin/echo"), QStringLiteral("a b")}});
    QCOMPARE(launch.program, QStringLiteral("/bin/echo"));
    QCOMPARE(launch.arguments, QStringList{QStringLiteral("a b")});
    launch = resolveLaunch(settings, {.command = QStringList{QStringLiteral("/bin/echo")}});
    QVERIFY(launch.arguments.isEmpty());
    launch = resolveLaunch({});
    QVERIFY(launch.program.isEmpty());
    QVERIFY(launch.arguments.isEmpty());
}

// A resolved launch preserves literal environment values and its snapshot of the inherited environment.
void LaunchSettingsTest::environmentIsLiteralAndCaptured()
{
    const QByteArray name{"KWI_LAUNCH_SNAPSHOT_TEST"};
    const bool existed = qEnvironmentVariableIsSet(name.constData());
    const QByteArray previous{qgetenv(name.constData())};
    const auto restore = qScopeGuard([&] {
        if (existed)
            qputenv(name.constData(), previous);
        else
            qunsetenv(name.constData());
    });
    qputenv(name.constData(), "inherited");
    LaunchSettings settings{.environment = {QStringLiteral("KWI_LAUNCH_SNAPSHOT_TEST=first"),
                                            QStringLiteral("KWI_LAUNCH_SNAPSHOT_TEST=last=$HOME literal"),
                                            QStringLiteral("KWI_EMPTY=")}};
    const auto inherited = resolveLaunch({});
    const auto launch = resolveLaunch(settings);
    QCOMPARE(qgetenv(name.constData()), QByteArray("inherited"));
    QCOMPARE(launch.environment().value(QString::fromLatin1(name)), QStringLiteral("last=$HOME literal"));
    QVERIFY(launch.environment().contains(QStringLiteral("KWI_EMPTY")));
    QVERIFY(launch.environment().value(QStringLiteral("KWI_EMPTY")).isEmpty());
    qputenv(name.constData(), "later");
    settings.environment.clear();
    QCOMPARE(inherited.environment().value(QString::fromLatin1(name)), QStringLiteral("inherited"));
    QCOMPARE(launch.environment().value(QString::fromLatin1(name)), QStringLiteral("last=$HOME literal"));
}

// Terminal environment defaults preserve explicit overrides and removals.
void LaunchSettingsTest::terminalDefaultsKeepTheRequestedRecipe()
{
    TerminalLaunch launch{};
    launch.inheritedEnvironment.clear();

    for (const auto *name : {"TERM", "COLORTERM", "TERM_PROGRAM", "COLUMNS", "LINES", "TERMCAP"}) {
        launch.inheritedEnvironment.insert(QString::fromLatin1(name), QStringLiteral("inherited"));
    }

    launch.inheritedEnvironment.insert(QStringLiteral("UNCHANGED"), QStringLiteral("captured"));
    const auto defaults = launch.environment();
    QCOMPARE(defaults.value(QStringLiteral("TERM")), QStringLiteral("xterm-256color"));
    QCOMPARE(defaults.value(QStringLiteral("COLORTERM")), QStringLiteral("truecolor"));
    QCOMPARE(defaults.value(QStringLiteral("TERM_PROGRAM")), QStringLiteral("zirq"));

    for (const auto *name : {"COLUMNS", "LINES", "TERMCAP"}) {
        QVERIFY(!defaults.contains(QString::fromLatin1(name)));
    }

    launch.environmentChanges = {{QStringLiteral("TERM"), QStringLiteral("vt100")},
                                 {QStringLiteral("COLORTERM"), std::nullopt},
                                 {QStringLiteral("COLUMNS"), QStringLiteral("42")},
                                 {QStringLiteral("EXPLICIT"), QStringLiteral("literal=$HOME")}};
    const auto changes = launch.resolvedEnvironmentChanges();
    QCOMPARE(changes.value(QStringLiteral("TERM")).value_or(QString{}), QStringLiteral("vt100"));
    QVERIFY(changes.contains(QStringLiteral("COLORTERM")));
    QVERIFY(!changes.value(QStringLiteral("COLORTERM")));
    const auto environment = launch.environment();
    QCOMPARE(environment.value(QStringLiteral("TERM")), QStringLiteral("vt100"));
    QVERIFY(!environment.contains(QStringLiteral("COLORTERM")));
    QCOMPARE(environment.value(QStringLiteral("COLUMNS")), QStringLiteral("42"));
    QVERIFY(!environment.contains(QStringLiteral("LINES")));
    QVERIFY(!environment.contains(QStringLiteral("TERMCAP")));
    QCOMPARE(environment.value(QStringLiteral("TERM_PROGRAM")), QStringLiteral("zirq"));
    QCOMPARE(environment.value(QStringLiteral("EXPLICIT")), QStringLiteral("literal=$HOME"));
    QCOMPARE(environment.value(QStringLiteral("UNCHANGED")), QStringLiteral("captured"));
}

void LaunchSettingsTest::validatesRecipes_data()
{
    QTest::addColumn<LaunchSettings>("settings");
    QTest::addColumn<bool>("valid");
    LaunchSettings settings;
    QTest::newRow("default-shell") << settings << true;
    settings.command = {QString{}};
    QTest::newRow("empty-program") << settings << false;
    settings.command = {QStringLiteral("./local-program"), QStringLiteral("a b"), QString{}, QStringLiteral("$HOME")};
    QTest::newRow("literal-arguments") << settings << true;
    settings.command.append(QString(QChar::Null));
    QTest::newRow("argument-null") << settings << false;
    settings = {};
    settings.directoryMode = LaunchSettings::DirectoryMode::Custom;
    QTest::newRow("missing-custom-directory") << settings << false;
    settings.directory = QStringLiteral("relative");
    QTest::newRow("relative-custom-directory") << settings << false;
    settings.directory = QStringLiteral("/not-required-to-exist-until-launch");
    QTest::newRow("future-custom-directory") << settings << true;
    settings.directoryMode = LaunchSettings::DirectoryMode::Current;
    QTest::newRow("directory-with-current-mode") << settings << false;

    settings = {};
    settings.command = QStringList(4096, QStringLiteral("a"));
    QTest::newRow("command-count-limit") << settings << true;
    settings.command.append(QStringLiteral("a"));
    QTest::newRow("command-count-over-limit") << settings << false;
    settings.command = {QStringLiteral("program"), QString(65529, QLatin1Char('a'))};
    QTest::newRow("command-text-limit") << settings << true;
    settings.command = {QStringLiteral("program"), QString(65530, QLatin1Char('a'))};
    QTest::newRow("command-text-over-limit") << settings << false;
    settings = {};
    settings.directoryMode = LaunchSettings::DirectoryMode::Custom;
    settings.directory = QLatin1Char('/') + QString(65535, QLatin1Char('d'));
    QTest::newRow("directory-text-limit") << settings << true;
    settings.directory.append(QLatin1Char('d'));
    QTest::newRow("directory-text-over-limit") << settings << false;
}

// Preset validation checks command and directory syntax and enforces their size limits.
void LaunchSettingsTest::validatesRecipes()
{
    QFETCH(LaunchSettings, settings);
    QFETCH(bool, valid);
    QString error{QStringLiteral("previous error")};
    QCOMPARE(validateLaunchSettings(settings, &error), valid);
    QCOMPARE(error.isEmpty(), valid);
}

void LaunchSettingsTest::validatesEnvironment_data()
{
    QTest::addColumn<QStringList>("entries");
    QTest::addColumn<bool>("valid");
    QTest::newRow("literal-values") << QStringList{QStringLiteral("A=one=two"), QStringLiteral("_A2= spaces $HOME "), QStringLiteral("EMPTY=")} << true;
    QTest::newRow("duplicates") << QStringList{QStringLiteral("A=first"), QStringLiteral("A=last")} << true;
    QTest::newRow("missing-equals") << QStringList{QStringLiteral("NAME")} << false;
    QTest::newRow("empty-name") << QStringList{QStringLiteral("=value")} << false;
    QTest::newRow("leading-digit") << QStringList{QStringLiteral("1NAME=value")} << false;
    QTest::newRow("space-in-name") << QStringList{QStringLiteral("BAD NAME=value")} << false;
    QTest::newRow("newline") << QStringList{QStringLiteral("NAME=one\ntwo")} << false;
    QTest::newRow("carriage-return") << QStringList{QStringLiteral("NAME=one\rtwo")} << false;
    QTest::newRow("null") << QStringList{QStringLiteral("NAME=") + QChar::Null} << false;
    QTest::newRow("entry-count-limit") << QStringList(4096, QStringLiteral("A=")) << true;
    QTest::newRow("entry-count-over-limit") << QStringList(4097, QStringLiteral("A=")) << false;
    QTest::newRow("text-limit") << QStringList{QStringLiteral("A=") + QString(65534, QLatin1Char('x'))} << true;
    QTest::newRow("text-over-limit") << QStringList{QStringLiteral("A=") + QString(65535, QLatin1Char('x'))} << false;
}

// Preset environment entries allow literal values within the name, syntax and size constraints.
void LaunchSettingsTest::validatesEnvironment()
{
    QFETCH(QStringList, entries);
    QFETCH(bool, valid);
    QString error;
    QCOMPARE(validateEnvironment(entries, &error), valid);
    QCOMPARE(error.isEmpty(), valid);
}

void LaunchSettingsTest::validatesLaunchSnapshots_data()
{
    QTest::addColumn<TerminalLaunch>("launch");
    QTest::addColumn<bool>("valid");
    TerminalLaunch launch;
    launch.inheritedEnvironment.clear();
    QTest::newRow("default-shell-and-directory") << launch << true;

    launch.program = QStringLiteral("./literal $program");
    launch.arguments = {QString{}, QStringLiteral("literal $HOME 水\nnext line")};
    launch.directory = QStringLiteral("/not-required-to-exist-until-launch");
    launch.inheritedEnvironment.insert(QStringLiteral("1 literal key"), QStringLiteral("first\nsecond=literal"));
    launch.environmentChanges = {{QStringLiteral("EMPTY"), QString{}},
                                 {QStringLiteral("VALUE"), QStringLiteral("literal $HOME\nnext line")},
                                 {QStringLiteral("REMOVE"), std::nullopt}};
    QTest::newRow("literal-values-and-removal") << launch << true;

    const QString withNull{QStringLiteral("before") + QChar::Null + QStringLiteral("after")};
    auto invalid = launch;
    invalid.directory = QStringLiteral("relative/path");
    QTest::newRow("relative-directory") << invalid << false;
    invalid = launch;
    invalid.directory = QLatin1Char('/') + withNull;
    QTest::newRow("directory-null") << invalid << false;
    invalid = launch;
    invalid.program = withNull;
    QTest::newRow("executable-null") << invalid << false;
    invalid = launch;
    invalid.arguments.append(withNull);
    QTest::newRow("argument-null") << invalid << false;
    invalid = launch;
    invalid.inheritedEnvironment.insert(QString{}, QStringLiteral("value"));
    QTest::newRow("inherited-empty-name") << invalid << false;
    invalid = launch;
    invalid.inheritedEnvironment.insert(QStringLiteral("BAD=NAME"), QStringLiteral("value"));
    QTest::newRow("inherited-equals-name") << invalid << false;
    invalid = launch;
    invalid.inheritedEnvironment.insert(withNull, QStringLiteral("value"));
    QTest::newRow("inherited-null-name") << invalid << false;
    invalid = launch;
    invalid.inheritedEnvironment.insert(QStringLiteral("BAD_VALUE"), withNull);
    QTest::newRow("inherited-null-value") << invalid << false;
    invalid = launch;
    invalid.environmentChanges.insert(QString{}, std::nullopt);
    QTest::newRow("removed-empty-name") << invalid << false;
    invalid = launch;
    invalid.environmentChanges.insert(QStringLiteral("BAD=NAME"), std::nullopt);
    QTest::newRow("removed-equals-name") << invalid << false;
    invalid = launch;
    invalid.environmentChanges.insert(withNull, QStringLiteral("value"));
    QTest::newRow("delta-null-name") << invalid << false;
    invalid = launch;
    invalid.environmentChanges.insert(QStringLiteral("BAD_VALUE"), withNull);
    QTest::newRow("delta-null-value") << invalid << false;
}

// Process launch validation rejects relative directories and malformed program, argument or environment values.
void LaunchSettingsTest::validatesLaunchSnapshots()
{
    QFETCH(TerminalLaunch, launch);
    QFETCH(bool, valid);
    const auto error = launch.validationError();
    QCOMPARE(error.isEmpty(), valid);
}

QTEST_GUILESS_MAIN(LaunchSettingsTest)
#include "launchsettingstest.moc"
