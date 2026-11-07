// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <KConfig>
#include <KConfigGroup>
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>
#include <QStringLiteral>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>
#include <memory>

namespace
{
QByteArray readFile(const QString &path)
{
    QFile file{path};

    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

QStringList captureCommand(const QString &path, const QStringList &arguments = {})
{
    return QStringList{QStringLiteral("-e"),
                       QStringLiteral("/bin/sh"),
                       QStringLiteral("-c"),
                       QStringLiteral("output=$1; shift; printf '%s\\0' \"$PWD\" \"$@\" > \"$output\""),
                       QStringLiteral("capture"),
                       path}
    + arguments;
}

QByteArray capturedFields(const QString &directory, const QStringList &arguments = {})
{
    QByteArray result{directory.toUtf8()};
    result += '\0';

    for (const auto &argument : arguments) {
        result += argument.toUtf8();
        result += '\0';
    }

    return result;
}
}

class CliTest : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

private Q_SLOTS:
    void init();
    void cleanup();
    void literalArgumentsAndWorkingDirectory();
    void forwardingAndCommandLineErrors();

private:
    void prepare(QProcess &process);
    std::unique_ptr<QTemporaryDir> m_directory;
    QProcessEnvironment m_environment;
    QProcess m_primary;
};

void CliTest::init()
{
    m_directory = std::make_unique<QTemporaryDir>(QDir{QDir::tempPath()}.canonicalPath() + QStringLiteral("/clitest-XXXXXX"));
    QVERIFY(m_directory->isValid());
    m_environment = QProcessEnvironment::systemEnvironment();

    for (const auto *kind : {"CONFIG", "DATA", "CACHE", "STATE"}) {
        const QString name{QString::fromLatin1(kind)};
        const QString path{m_directory->filePath(name)};
        QVERIFY(QDir{}.mkpath(path));
        m_environment.insert(QStringLiteral("XDG_%1_HOME").arg(name), path);
    }

    const QString presetDirectory{m_directory->filePath(QStringLiteral("DATA/kwi/presets"))};
    QVERIFY(QDir{}.mkpath(presetDirectory));
    const QString presetFile{presetDirectory + QStringLiteral("/shell.preset")};
    KConfig preset{presetFile, KConfig::SimpleConfig};
    KConfigGroup general{&preset, QStringLiteral("General")};
    general.writeEntry("Format", QStringLiteral("KwiLaunchPreset"));
    general.writeEntry("Version", 1);
    general.writeEntry("Name", QStringLiteral("Test shell"));
    KConfigGroup launch{&preset, QStringLiteral("Launch")};
    launch.writeEntry("Command", QStringList{QStringLiteral("/bin/sh"), QStringLiteral("-i")});
    launch.writeEntry("Environment", QStringList{QStringLiteral("ENV="), QStringLiteral("BASH_ENV=")});
    QVERIFY(preset.sync());
    KConfig settings{m_directory->filePath(QStringLiteral("CONFIG/kwiterminalrc")), KConfig::SimpleConfig};
    KConfigGroup{&settings, QStringLiteral("Presets")}.writeEntry("DefaultPreset", QUrl::fromLocalFile(presetFile).toString());
    QVERIFY(settings.sync());

    m_environment.insert(QStringLiteral("SHELL"), QStringLiteral("/bin/sh"));
    m_environment.insert(QStringLiteral("ENV"), QString{});
    m_environment.insert(QStringLiteral("BASH_ENV"), QString{});
    prepare(m_primary);
}

void CliTest::cleanup()
{
    if (m_primary.state() != QProcess::NotRunning) {
        m_primary.terminate();

        if (!m_primary.waitForFinished(5000)) {
            m_primary.kill();
            QVERIFY(m_primary.waitForFinished(5000));
            QFAIL("Kwi did not terminate.");
        }
    }
}

void CliTest::prepare(QProcess &process)
{
    process.setProgram(QStringLiteral(KWI_EXECUTABLE_PATH));
    process.setProcessEnvironment(m_environment);
    process.setWorkingDirectory(m_directory->path());
}

// The launched child receives literal arguments and the directory resolved from the caller's path.
void CliTest::literalArgumentsAndWorkingDirectory()
{
    const QString directory{m_directory->filePath(QStringLiteral("project with spaces"))};
    QVERIFY(QDir{}.mkpath(directory));
    const QString output{m_directory->filePath(QStringLiteral("literal-arguments"))};
    const QStringList arguments{QStringLiteral("--version"),
                                QStringLiteral("--hold"),
                                QStringLiteral("--new-tab"),
                                QStringLiteral("--tab-title"),
                                QStringLiteral("child title"),
                                QStringLiteral("--"),
                                QStringLiteral("-e"),
                                QString{},
                                QStringLiteral("日本語"),
                                QStringLiteral("argument with spaces"),
                                QStringLiteral("$(touch should-not-exist)"),
                                QStringLiteral("semi;colon")};
    m_primary.setArguments(QStringList{QStringLiteral("--workdir"), QStringLiteral("project with spaces")} + captureCommand(output, arguments));
    m_primary.start();
    QVERIFY2(m_primary.waitForFinished(10000), qPrintable(m_primary.errorString()));
    QCOMPARE(m_primary.exitStatus(), QProcess::NormalExit);
    QCOMPARE(m_primary.exitCode(), 0);
    QCOMPARE(readFile(output), capturedFields(directory, arguments));
    QVERIFY(!QFile::exists(QDir{directory}.filePath(QStringLiteral("should-not-exist"))));
}

// A second invocation forwards its command and directory while handling help and invalid arguments locally.
void CliTest::forwardingAndCommandLineErrors()
{
    const QString firstOutput{m_directory->filePath(QStringLiteral("first-command"))};
    m_primary.setArguments(QStringList{QStringLiteral("--hold")} + captureCommand(firstOutput, {QStringLiteral("first")}));
    m_primary.start();
    QTRY_COMPARE(readFile(firstOutput), capturedFields(m_directory->path(), {QStringLiteral("first")}));
    QCOMPARE(m_primary.state(), QProcess::Running);

    const QString callerDirectory{m_directory->filePath(QStringLiteral("second caller"))};
    QVERIFY(QDir{}.mkpath(callerDirectory));
    const QString nextOutput{m_directory->filePath(QStringLiteral("forwarded-command"))};
    QProcess forwarded;
    prepare(forwarded);
    forwarded.setWorkingDirectory(callerDirectory);
    forwarded.setArguments(QStringList{QStringLiteral("--new-tab"), QStringLiteral("--hold")} + captureCommand(nextOutput, {QStringLiteral("forwarded")}));
    forwarded.start();
    QVERIFY2(forwarded.waitForFinished(10000), qPrintable(forwarded.errorString()));
    QCOMPARE(forwarded.exitStatus(), QProcess::NormalExit);
    QCOMPARE(forwarded.exitCode(), 0);
    QTRY_COMPARE(readFile(nextOutput), capturedFields(callerDirectory, {QStringLiteral("forwarded")}));
    QCOMPARE(m_primary.state(), QProcess::Running);

    for (const auto &arguments : {QStringList{QStringLiteral("--help")},
                                  QStringList{QStringLiteral("--version")},
                                  QStringList{QStringLiteral("-e")},
                                  QStringList{QStringLiteral("--preset"), QString{}}}) {
        QProcess command;
        prepare(command);
        auto environment = m_environment;
        environment.remove(QStringLiteral("QT_LOGGING_TO_CONSOLE"));
        environment.remove(QStringLiteral("QT_FORCE_STDERR_LOGGING"));
        environment.remove(QStringLiteral("QT_ASSUME_STDERR_HAS_CONSOLE"));
        command.setProcessEnvironment(environment);
        command.setArguments(arguments);
        command.start();
        QVERIFY2(command.waitForFinished(10000), qPrintable(command.errorString()));
        QCOMPARE(command.exitStatus(), QProcess::NormalExit);
        const bool informational{arguments.first() == QStringLiteral("--help") || arguments.first() == QStringLiteral("--version")};
        QCOMPARE(command.exitCode(), informational ? 0 : 2);

        if (informational) {
            QVERIFY(!command.readAllStandardOutput().isEmpty());
        } else {
            const QByteArray errorOutput{command.readAllStandardError()};
            const QByteArray option{arguments.first().toUtf8() + ' '};
            QVERIFY2(errorOutput.contains(option), errorOutput.constData());
        }

        QCOMPARE(m_primary.state(), QProcess::Running);
    }
}

QTEST_GUILESS_MAIN(CliTest)

#include "clitest.moc"
