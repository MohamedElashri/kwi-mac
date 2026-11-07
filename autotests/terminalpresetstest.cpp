// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "settings/TerminalPresets.h"
#include "settings/TerminalSettings.h"

#include <KConfig>
#include <KConfigGroup>
#include <KSharedConfig>
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QIODevice>
#include <QObject>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QStringLiteral>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

using namespace Kwi;

class TerminalPresetsTest : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

private Q_SLOTS:
    void init();
    void presetPersistenceAndOwnership();
    void rejectsOversizedPresetFiles();
};

void TerminalPresetsTest::init()
{
    const auto config = KSharedConfig::openConfig(QStringLiteral("kwiterminalrc"), KConfig::NoGlobals);
    const auto groups = config->groupList();

    for (const QString &group : groups) {
        config->deleteGroup(group);
    }

    QVERIFY(config->sync());
    QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/kwi/presets")).removeRecursively();
    QDir(qEnvironmentVariable("XDG_DATA_DIRS") + QStringLiteral("/kwi/presets")).removeRecursively();
}

// Preset files preserve literal launch recipes and installed presets remain protected from edits and removal.
void TerminalPresetsTest::presetPersistenceAndOwnership()
{
    TerminalSettings settings;
    TerminalPresets presets{&settings};
    const LaunchSettings recipe{.command = {QStringLiteral("/bin/sh"), QStringLiteral("-l"), QStringLiteral("literal $HOME"), QString{}},
                                .directoryMode = LaunchSettings::DirectoryMode::Custom,
                                .directory = QDir::tempPath(),
                                .environment = {QStringLiteral("ONE=value"),
                                                QStringLiteral("EMPTY="),
                                                QStringLiteral("MANY=a=b"),
                                                QStringLiteral("LITERAL=$HOME"),
                                                QStringLiteral("ONE=last")}};
    QVERIFY(presets.savePreset(QStringLiteral("Shell"), recipe));
    const auto saved = presets.preset(QStringLiteral("Shell"));
    QVERIFY(saved);
    TerminalSettings otherSettings;
    TerminalPresets reopened{&otherSettings};
    const auto loaded = reopened.preset(QStringLiteral("Shell"));
    QVERIFY(loaded);
    QCOMPARE(loaded->launch, recipe);

#ifdef Q_OS_MACOS
    // macOS has a fixed system data directory; XDG_DATA_DIRS cannot inject an
    // installed preset fixture. Exercise read-only protection with the builtin.
    const auto builtin = presets.preset(TerminalPresets::builtinPresetName());
    QVERIFY(builtin);
    QVERIFY(!builtin->editable);
    QVERIFY(!presets.savePreset(builtin->name, {}));
    QVERIFY(!presets.removePreset(builtin->name));
#else
    const QString installedDirectory{qEnvironmentVariable("XDG_DATA_DIRS") + QStringLiteral("/kwi/presets")};
    QVERIFY(QDir{}.mkpath(installedDirectory));
    const QString installedPath{installedDirectory + QStringLiteral("/installed.preset")};
    QVERIFY(QFile::copy(saved->path, installedPath));
    KConfig installed{installedPath, KConfig::SimpleConfig};
    installed.group(QStringLiteral("General")).writeEntry("Name", QStringLiteral("Installed"));
    QVERIFY(installed.sync());
    QFile file{installedPath};
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray original{file.readAll()};
    file.close();
    presets.refresh();
    QVERIFY(presets.preset(QStringLiteral("Installed")));
    QVERIFY(!presets.savePreset(QStringLiteral("Installed"), {}));
    QVERIFY(!presets.removePreset(QStringLiteral("Installed")));
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), original);
#endif
}

// Oversized preset files are excluded from discovery and rejected on import.
void TerminalPresetsTest::rejectsOversizedPresetFiles()
{
    TerminalSettings settings;
    TerminalPresets presets{&settings};
    QVERIFY(presets.savePreset(QStringLiteral("Oversized"), {}));
    const auto saved = presets.preset(QStringLiteral("Oversized"));
    QVERIFY(saved);
    QFile file{saved->path};
    QVERIFY(file.open(QIODevice::Append));
    const QByteArray extra{"\n#" + QByteArray(1024 * 1024, 'x')};
    QCOMPARE(file.write(extra), extra.size());
    file.close();
    presets.refresh();
    QVERIFY(!presets.preset(QStringLiteral("Oversized")));
    QVERIFY(!presets.importPreset(QUrl::fromLocalFile(saved->path)));
}

int main(int argc, char **argv)
{
    QTemporaryDir directory;

    if (!directory.isValid()) {
        return 1;
    }

    qputenv("XDG_CONFIG_HOME", QFile::encodeName(directory.filePath(QStringLiteral("config"))));
    qputenv("XDG_CONFIG_DIRS", QFile::encodeName(directory.filePath(QStringLiteral("system-config"))));
    qputenv("XDG_DATA_HOME", QFile::encodeName(directory.filePath(QStringLiteral("data"))));
    qputenv("XDG_DATA_DIRS", QFile::encodeName(directory.filePath(QStringLiteral("system-data"))));
    QGuiApplication app{argc, argv};
    TerminalPresetsTest test;

    return QTest::qExec(&test, argc, argv);
}

#include "terminalpresetstest.moc"
