// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "process/PtyProcess.h"
#include "ProcessTestSupport.h"

#include <KLocalizedString>
#include <KPty>
#include <KSandbox>
#include <KUser>
#include <QByteArray>
#include <QChar>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QLatin1String>
#include <QObject>
#include <QSignalSpy>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QStringLiteral>
#include <QTemporaryDir>
#include <QTest>

#include <cerrno>
#include <fcntl.h>
#include <optional>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

using Kwi::PtyProcess;

class PtyProcessTest : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

private Q_SLOTS:
    void initTestCase()
    {
        KLocalizedString::setApplicationDomain("kwi");
    }

    void outputEnvironmentAndExit();
    void inputAndResize();
    void reportsStartupErrors();
    void rejectsInvalidLaunchBeforeStarting();
    void execFailureIsReapedBeforeExit();
    void tracksDirectoryAndForegroundJob();

    void directCommandsAreForeground_data();
    void directCommandsAreForeground();
    void drainsOutputBeforeExit();
    void closesStubbornChildAndReaps();
    void destructionReapsChild();
    void restartsAfterExit();
    void externalPtyAttachment();
    void startsOnExternalPty();
    void rejectsInvalidExternalPty();
    void launchEnvironmentOverrides();
    void terminalEnvironmentPolicy_data();
    void terminalEnvironmentPolicy();
    void defaultShellRespectsPlatformPolicy();

    void resolvesCommandInChildContext_data();
    void resolvesCommandInChildContext();
};

// A child receives terminal defaults, and exit reporting preserves its status and leaves no native child to reap.
void PtyProcessTest::outputEnvironmentAndExit()
{
    PtyProcess process;
    QByteArray output;
    connect(&process, &PtyProcess::dataReceived, this, [&output](const QByteArray &data) {
        output += data;
    });
    QSignalSpy exited{&process, &PtyProcess::exited};
    QSignalSpy errors{&process, &PtyProcess::errorOccurred};
    QSignalSpy started{&process, &PtyProcess::started};
    QVERIFY(process.start(testLaunch(QStringLiteral("/bin/sh"),
                                     {QStringLiteral("-c"),
                                      QStringLiteral("stty -echo; printf '%s|%s|%s\\n' \"$TERM\" "
                                                     "\"$COLORTERM\" \"$TERM_PROGRAM\"; read -r ack; exit 7")},
                                     QDir::tempPath()),
                          80,
                          24));
    // Wait for output before allowing the child to revoke its FreeBSD controlling TTY.
    QTRY_VERIFY(output.contains("xterm-256color|truecolor|zirq"));
    const pid_t pid = pid_t(process.processId());
    QVERIFY(pid > 0);
    QVERIFY(process.write(QByteArray(1, '\x04')));
    QTRY_COMPARE_WITH_TIMEOUT(exited.size(), 1, 5000);
    QCOMPARE(started.size(), 1);
    QVERIFY(output.contains("xterm-256color|truecolor|zirq"));
    QCOMPARE(exited.at(0).at(0).toInt(), 7);
    QVERIFY(errors.isEmpty());
    QVERIFY(!process.isRunning());

    if (!KSandbox::isFlatpak()) {
        int status;
        QCOMPARE(waitpid(pid, &status, WNOHANG), -1);
        QCOMPARE(errno, ECHILD);
    }
}

// A child receives Unicode input and observes the updated terminal size.
void PtyProcessTest::inputAndResize()
{
    PtyProcess process;
    QByteArray output;
    connect(&process, &PtyProcess::dataReceived, this, [&output](const QByteArray &data) {
        output += data;
    });
    QSignalSpy exited{&process, &PtyProcess::exited};
    QVERIFY(process.start(testLaunch(QStringLiteral("/bin/sh"),
                                     {QStringLiteral("-c"),
                                      QStringLiteral("stty -echo; printf 'READY\\n'; IFS= read -r text; "
                                                     "printf '<%s>\\n' \"$text\"; stty size; read -r ack; exit 0")},
                                     QDir::tempPath()),
                          80,
                          24));
    QTRY_VERIFY_WITH_TIMEOUT(output.contains("READY"), 5000);
    process.resize(117, 43);
    QVERIFY(process.write(QByteArray("hello ") + QByteArray::fromHex("e4b896e7958c") + '\n'));
    const QByteArray expected = QByteArray("<hello ") + QByteArray::fromHex("e4b896e7958c") + '>';
    QTRY_VERIFY(output.contains(expected));
    QTRY_VERIFY(output.contains("43 117"));
    QVERIFY(process.write(QByteArray(1, '\x04')));
    QTRY_COMPARE_WITH_TIMEOUT(exited.size(), 1, 5000);
}

// Missing executables and directories report startup errors without announcing a successful start.
void PtyProcessTest::reportsStartupErrors()
{
    QTemporaryDir directory{QDir{QDir::tempPath()}.canonicalPath() + QStringLiteral("/ptyprocesstest-XXXXXX")};
    QVERIFY(directory.isValid());
    PtyProcess process;
    QSignalSpy errors{&process, &PtyProcess::errorOccurred};
    QSignalSpy exited{&process, &PtyProcess::exited};
    QSignalSpy started{&process, &PtyProcess::started};
    connect(&process, &PtyProcess::exited, this, [&process] {
        QVERIFY(!process.isRunning());
    });
    const bool pending = process.start(testLaunch(directory.filePath(QStringLiteral("missing-program")), {}, directory.path()), 80, 24);
    QTRY_COMPARE(errors.size(), 1);

    if (pending) {
        QTRY_COMPARE(exited.size(), 1);
        QCOMPARE(exited.first().first().toInt(), 127);
        exited.clear();
    } else {
        QVERIFY(exited.isEmpty());
    }

    QVERIFY(started.isEmpty());
    QVERIFY(!process.isRunning());
    QVERIFY(process.start(
        testLaunch(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), QStringLiteral("exit 0")}, directory.filePath(QStringLiteral("missing-directory"))),
        80,
        24));
    QTRY_COMPARE_WITH_TIMEOUT(exited.size(), 1, 5000);
    QCOMPARE(errors.size(), 2);
    QCOMPARE(exited.at(0).at(0).toInt(), 127);
    QCOMPARE(started.size(), 0);
}

// Invalid launch fields are rejected before starting a child and leave the process object usable.
void PtyProcessTest::rejectsInvalidLaunchBeforeStarting()
{
    const auto launch =
        testLaunch(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), QStringLiteral("stty -echo; printf VALID_LAUNCH; read -r ack; exit 0")}, QDir::tempPath());
    PtyProcess process;
    QSignalSpy errors{&process, &PtyProcess::errorOccurred};
    QSignalSpy started{&process, &PtyProcess::started};
    QSignalSpy exited{&process, &PtyProcess::exited};
    QByteArray output;
    connect(&process, &PtyProcess::dataReceived, this, [&output](const QByteArray &data) {
        output += data;
    });

    auto invalid = launch;
    invalid.directory = QStringLiteral("relative");
    QVERIFY(!process.start(invalid, 80, 24));
    invalid = launch;
    invalid.inheritedEnvironment.insert(QStringLiteral("BAD_VALUE"), QString(QChar::Null));
    QVERIFY(!process.start(invalid, 80, 24));
    invalid = launch;
    invalid.environmentChanges.insert(QStringLiteral("BAD=NAME"), std::nullopt);
    QVERIFY(!process.start(invalid, 80, 24));
    QCOMPARE(errors.size(), 3);
    QVERIFY(!errors.first().first().toString().isEmpty());
    QVERIFY(started.isEmpty());
    QVERIFY(exited.isEmpty());
    QVERIFY(!process.isRunning());
    QCOMPARE(process.processId(), qint64(-1));

    QVERIFY(process.start(launch, 80, 24));
    QTRY_COMPARE(output, QByteArrayLiteral("VALID_LAUNCH"));
    QVERIFY(process.write(QByteArray(1, '\x04')));
    QTRY_COMPARE_WITH_TIMEOUT(exited.size(), 1, 5000);
    QCOMPARE(started.size(), 1);
    QCOMPARE(exited.first().first().toInt(), 0);
    QCOMPARE(errors.size(), 3);
    QCOMPARE(output, QByteArrayLiteral("VALID_LAUNCH"));
}

// A child whose interpreter is missing is reaped before its failed exit is reported.
void PtyProcessTest::execFailureIsReapedBeforeExit()
{
    if (KSandbox::isFlatpak()) {
        QSKIP("Flatpak owns host children outside this PID namespace.");
    }

    QTemporaryDir directory{QDir{QDir::tempPath()}.canonicalPath() + QStringLiteral("/ptyprocesstest-XXXXXX")};
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("broken-program"));
    QFile program{path};
    QVERIFY(program.open(QIODevice::WriteOnly));
    QVERIFY(program.write(QByteArrayLiteral("#!") + QFile::encodeName(directory.filePath(QStringLiteral("missing-interpreter"))) + '\n') > 0);
    program.close();
    QVERIFY(program.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    PtyProcess process;
    QSignalSpy exited{&process, &PtyProcess::exited};
    QSignalSpy started{&process, &PtyProcess::started};
    QVERIFY(process.start(testLaunch(path, {}, directory.path()), 80, 24));
    const pid_t pid = pid_t(process.processId());
    QVERIFY(pid > 0);
    connect(&process, &PtyProcess::exited, this, [&process, pid](int code) {
        QCOMPARE(code, 127);
        QVERIFY(!process.isRunning());
        int status = 0;
        QCOMPARE(waitpid(pid, &status, WNOHANG), -1);
        QCOMPARE(errno, ECHILD);
    });
    QTRY_COMPARE_WITH_TIMEOUT(exited.size(), 1, 5000);
    QCOMPARE(started.size(), 0);
}

// Process observations track shell directory changes and foreground jobs, clearing job metadata after interruption and exit.
void PtyProcessTest::tracksDirectoryAndForegroundJob()
{
    QTemporaryDir directory{QDir{QDir::tempPath()}.canonicalPath() + QStringLiteral("/ptyprocesstest-XXXXXX")};
    QVERIFY(directory.isValid());
    PtyProcess process;
    QSignalSpy exited{&process, &PtyProcess::exited};
    QByteArray output;
    connect(&process, &PtyProcess::dataReceived, this, [&output](const QByteArray &data) {
        output += data;
    });
    QVERIFY(process.start(testLaunch(QStringLiteral("/bin/sh"),
                                     {QStringLiteral("-i")},
                                     directory.path(),
                                     {QStringLiteral("HOME=") + directory.path(), QStringLiteral("ENV="), QStringLiteral("BASH_ENV=")}),
                          80,
                          24));
    QTRY_COMPARE_WITH_TIMEOUT(process.workingDirectory(), directory.path(), 5000);
    QVERIFY(process.write("cd /; printf 'CHANGED\\n'\n"));
    QTRY_VERIFY_WITH_TIMEOUT(output.contains("CHANGED\r\n"), 5000);
    QTRY_COMPARE_WITH_TIMEOUT(process.workingDirectory(), QStringLiteral("/"), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!process.isBusy(), 5000);
    const auto shellId = process.processId();
    QVERIFY(shellId > 0);
    QCOMPARE(process.foregroundProcessId(), -1);
    QVERIFY(process.foregroundProcessName().isEmpty());
    QVERIFY(process.write("sleep 30\n"));
    QTRY_COMPARE_WITH_TIMEOUT(process.foregroundProcessName(), QStringLiteral("sleep"), 5000);
    QVERIFY(process.foregroundProcessId() > 0);
    QVERIFY(process.foregroundProcessId() != shellId);
    QCOMPARE(process.foregroundProcessName(), QStringLiteral("sleep"));
    QVERIFY(process.write(QByteArray(1, '\x03')));
    QTRY_VERIFY_WITH_TIMEOUT(!process.isBusy(), 5000);
    QCOMPARE(process.foregroundProcessId(), -1);
    QVERIFY(process.foregroundProcessName().isEmpty());
    QVERIFY(process.write("exit\n"));
    QTRY_COMPARE_WITH_TIMEOUT(exited.size(), 1, 5000);
    QVERIFY(!process.isBusy());
    QCOMPARE(process.foregroundProcessId(), -1);
    QVERIFY(process.foregroundProcessName().isEmpty());
    process.refreshProcessInfo();
    QCOMPARE(process.foregroundProcessId(), -1);
    QVERIFY(process.foregroundProcessName().isEmpty());
}

void PtyProcessTest::directCommandsAreForeground_data()
{
    QTest::addColumn<QString>("program");
    QTest::addColumn<QStringList>("arguments");
    QTest::newRow("sleep") << QStringLiteral("sleep") << QStringList{QStringLiteral("30")};
    QTest::newRow("python") << QStringLiteral(KWI_TEST_PYTHON) << QStringList{QStringLiteral("-c"), QStringLiteral("import time; time.sleep(30)")};
    QTest::newRow("shell-command") << QStringLiteral("/bin/sh") << QStringList{QStringLiteral("-c"), QStringLiteral("sleep 30")};
}

// Directly launched commands are busy until they close.
void PtyProcessTest::directCommandsAreForeground()
{
    QFETCH(QString, program);
    QFETCH(QStringList, arguments);
    PtyProcess process;
    QSignalSpy started{&process, &PtyProcess::started};
    QSignalSpy exited{&process, &PtyProcess::exited};
    QVERIFY(process.start(testLaunch(program, arguments, QDir::tempPath()), 80, 24));
    QTRY_COMPARE_WITH_TIMEOUT(started.size(), 1, 5000);
    QVERIFY(process.isBusy());
    process.close();
    QTRY_COMPARE_WITH_TIMEOUT(exited.size(), 1, 5000);
    QVERIFY(!process.isBusy());
}

// All bytes from a large child write arrive before exit is reported.
void PtyProcessTest::drainsOutputBeforeExit()
{
    PtyProcess process;
    qsizetype received = 0;
    connect(&process, &PtyProcess::dataReceived, this, [&received](const QByteArray &data) {
        received += data.size();
    });
    QSignalSpy exited{&process, &PtyProcess::exited};
    QVERIFY(process.start(testLaunch(QStringLiteral(KWI_TEST_PYTHON),
                                     {QStringLiteral("-c"),
                                      QStringLiteral("import sys,termios; sys.stdout.buffer.write(b'x'*2097152); "
                                                     "sys.stdout.buffer.flush(); termios.tcdrain(1)")},
                                     QDir::tempPath()),
                          80,
                          24));
    QTRY_COMPARE_WITH_TIMEOUT(exited.size(), 1, 10000);
    QCOMPARE(received, 2097152);
}

// Closing a child that ignores hangup and termination escalates to SIGKILL and reaps the native child.
void PtyProcessTest::closesStubbornChildAndReaps()
{
    PtyProcess process;
    QByteArray output;
    connect(&process, &PtyProcess::dataReceived, this, [&output](const QByteArray &data) {
        output += data;
    });
    QSignalSpy exited{&process, &PtyProcess::exited};
    QVERIFY(process.start(testLaunch(QStringLiteral("/bin/sh"),
                                     {QStringLiteral("-c"), QStringLiteral("trap '' HUP TERM; printf 'READY\\n'; while :; do :; done")},
                                     QDir::tempPath()),
                          80,
                          24));
    QTRY_VERIFY_WITH_TIMEOUT(output.contains("READY"), 5000);
    const pid_t pid = pid_t(process.processId());
    QVERIFY(pid > 0);
    process.close();
    QTRY_COMPARE_WITH_TIMEOUT(exited.size(), 1, 5000);
    QCOMPARE(exited.at(0).at(0).toInt(), 128 + SIGKILL);

    if (!KSandbox::isFlatpak()) {
        int status;
        QCOMPARE(waitpid(pid, &status, WNOHANG), -1);
        QCOMPARE(errno, ECHILD);
    }
}

// Destroying a process object terminates and reaps its native child.
void PtyProcessTest::destructionReapsChild()
{
    if (KSandbox::isFlatpak()) {
        QSKIP("Flatpak owns host children outside this PID namespace.");
    }

    pid_t pid;
    {
        PtyProcess process;
        QVERIFY(process.start(testLaunch(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), QStringLiteral("sleep 30")}, QDir::tempPath()), 80, 24));
        QTRY_VERIFY(process.processId() > 0);
        pid = pid_t(process.processId());
    }

    QTRY_VERIFY_WITH_TIMEOUT(kill(pid, 0) < 0 && errno == ESRCH, 5000);
    int status;
    QCOMPARE(waitpid(pid, &status, WNOHANG), -1);
    QCOMPARE(errno, ECHILD);
}

// An attached PTY exchanges bytes and applies cell and pixel sizes without taking ownership of the caller's descriptors.
void PtyProcessTest::externalPtyAttachment()
{
    KPty owner;
    QVERIFY(owner.open());
    termios attributes{};
    QCOMPARE(tcgetattr(owner.slaveFd(), &attributes), 0);
    cfmakeraw(&attributes);
    QCOMPARE(tcsetattr(owner.slaveFd(), TCSANOW, &attributes), 0);
    PtyProcess attached;
    QByteArray received;
    connect(&attached, &PtyProcess::dataReceived, this, [&received](const QByteArray &bytes) {
        received += bytes;
    });
    QSignalSpy exited{&attached, &PtyProcess::exited};
    QVERIFY(attached.attach(owner.masterFd(), 91, 37));
    QVERIFY(attached.isRunning());
    QCOMPARE(attached.processId(), -1);
    QCOMPARE(::write(owner.slaveFd(), "external output", 15), 15);
    QTRY_COMPARE_WITH_TIMEOUT(received, QByteArray("external output"), 3000);
    QVERIFY(attached.write("input"));
    int available = 0;
    QTRY_VERIFY_WITH_TIMEOUT(ioctl(owner.slaveFd(), FIONREAD, &available) == 0 && available == 5, 3000);
    char bytes[5];
    QCOMPARE(::read(owner.slaveFd(), bytes, sizeof(bytes)), 5);
    QCOMPARE(QByteArray(bytes, sizeof(bytes)), QByteArray("input"));
    winsize size{};
    QCOMPARE(ioctl(owner.slaveFd(), TIOCGWINSZ, &size), 0);
    QCOMPARE(size.ws_col, 91);
    QCOMPARE(size.ws_row, 37);
    attached.resize(100, 40, QSize(12, 25));
    QCOMPARE(ioctl(owner.slaveFd(), TIOCGWINSZ, &size), 0);
    QCOMPARE(size.ws_col, 100);
    QCOMPARE(size.ws_row, 40);
    QCOMPARE(size.ws_xpixel, 1200);
    QCOMPARE(size.ws_ypixel, 1000);
    attached.close();
    QTRY_COMPARE(exited.size(), 1);
    QVERIFY(fcntl(owner.masterFd(), F_GETFD) >= 0);
    QVERIFY(fcntl(owner.slaveFd(), F_GETFD) >= 0);
}

// A child launched on a supplied PTY reports output and exit status while leaving the caller's master descriptor open.
void PtyProcessTest::startsOnExternalPty()
{
    KPty owner;
    QVERIFY(owner.open());
    PtyProcess process;
    QByteArray received;
    connect(&process, &PtyProcess::dataReceived, this, [&received](const QByteArray &bytes) {
        received += bytes;
    });
    QSignalSpy exited{&process, &PtyProcess::exited};
    QVERIFY(process.start(testLaunch(QStringLiteral("/bin/sh"),
                                     {QStringLiteral("-c"), QStringLiteral("stty -echo; printf 'borrowed-master'; read -r ack; exit 9")},
                                     QDir::tempPath()),
                          80,
                          24,
                          owner.masterFd()));
    QTRY_COMPARE(received, QByteArray("borrowed-master"));
    QVERIFY(process.write(QByteArray(1, '\x04')));
    QTRY_COMPARE_WITH_TIMEOUT(exited.size(), 1, 5000);
    QCOMPARE(exited.first().first().toInt(), 9);
    QCOMPARE(received, QByteArray("borrowed-master"));
    QVERIFY(fcntl(owner.masterFd(), F_GETFD) >= 0);
}

// Invalid PTY descriptors are rejected without starting a process or closing the caller's descriptor.
void PtyProcessTest::rejectsInvalidExternalPty()
{
    QFile regular{QStringLiteral("/dev/null")};
    QVERIFY(regular.open(QIODevice::ReadWrite));
    PtyProcess process;
    QSignalSpy errors{&process, &PtyProcess::errorOccurred};
    QVERIFY(!process.attach(regular.handle(), 80, 24));
    QVERIFY(!process.attach(-1, 80, 24));
    QCOMPARE(errors.size(), 2);
    QVERIFY(fcntl(regular.handle(), F_GETFD) >= 0);
    QVERIFY(!process.isRunning());
}

// Literal environment overrides reach the child without changing the parent environment.
void PtyProcessTest::launchEnvironmentOverrides()
{
    const QByteArray previous = qgetenv("KWI_PART_TEST");
    PtyProcess process;
    QByteArray received;
    connect(&process, &PtyProcess::dataReceived, this, [&received](const QByteArray &bytes) {
        received += bytes;
    });
    QSignalSpy exited{&process, &PtyProcess::exited};
    QVERIFY(
        process.start(testLaunch(QStringLiteral("/bin/sh"),
                                 {QStringLiteral("-c"), QStringLiteral("stty -echo; printf '%s|%s' \"$KWI_PART_TEST\" \"$TERM_PROGRAM\"; read -r ack; exit 0")},
                                 QDir::tempPath(),
                                 {QStringLiteral("KWI_PART_TEST=value=with=equals")}),
                      80,
                      24));
    QTRY_COMPARE(received, QByteArray("value=with=equals|zirq"));
    QVERIFY(process.write(QByteArray(1, '\x04')));
    QTRY_COMPARE_WITH_TIMEOUT(exited.size(), 1, 5000);
    QCOMPARE(received, QByteArray("value=with=equals|zirq"));
    QCOMPARE(qgetenv("KWI_PART_TEST"), previous);
}

void PtyProcessTest::terminalEnvironmentPolicy_data()
{
    QTest::addColumn<QStringList>("overrides");
    QTest::addColumn<QByteArray>("expected");
    QTest::newRow("inherited-values-removed") << QStringList{} << QByteArray("unset|unset|unset|captured");
    QTest::newRow("explicit-values-survive") << QStringList{QStringLiteral("COLUMNS=123"), QStringLiteral("LINES=45"), QStringLiteral("TERMCAP=literal=$HOME")}
                                             << QByteArray("123|45|literal=$HOME|captured");
}

// Launches discard inherited terminal size and capability variables, preserve explicit overrides and retain other captured values.
void PtyProcessTest::terminalEnvironmentPolicy()
{
    if (KSandbox::isFlatpak()) {
        QSKIP("Flatpak inherits its host service's environment.");
    }

    QFETCH(QStringList, overrides);
    QFETCH(QByteArray, expected);
    auto launch = testLaunch(QStringLiteral(KWI_TEST_PYTHON),
                             {QStringLiteral("-c"),
                              QStringLiteral("import os,sys,termios; "
                                             "sys.stdout.write('|'.join(os.environ.get(name,'unset') "
                                             "for name in ('COLUMNS','LINES','TERMCAP','KWI_SNAPSHOT'))); "
                                             "sys.stdout.flush(); termios.tcdrain(1)")},
                             QDir::tempPath(),
                             overrides);

    for (const auto *name : {"COLUMNS", "LINES", "TERMCAP"}) {
        launch.inheritedEnvironment.insert(QString::fromLatin1(name), QStringLiteral("inherited"));
    }

    launch.inheritedEnvironment.insert(QStringLiteral("KWI_SNAPSHOT"), QStringLiteral("captured"));
    PtyProcess process;
    QByteArray output;
    connect(&process, &PtyProcess::dataReceived, this, [&](const QByteArray &bytes) {
        output += bytes;
    });
    QSignalSpy exited{&process, &PtyProcess::exited};
    QVERIFY(process.start(launch, 80, 24));
    QTRY_COMPARE_WITH_TIMEOUT(exited.size(), 1, 5000);
    QCOMPARE(output, expected);
}

// Platform shell policy selects the executable; an explicit SHELL override remains visible in the child environment.
void PtyProcessTest::defaultShellRespectsPlatformPolicy()
{
    if (KSandbox::isFlatpak()) {
        QSKIP("Flatpak inherits its host service's environment.");
    }

    auto launch = testLaunch({}, {}, {}, {QStringLiteral("SHELL=/not/a/shell"), QStringLiteral("ENV="), QStringLiteral("BASH_ENV=")});
    launch.inheritedEnvironment.insert(QStringLiteral("SHELL"), QStringLiteral("/bin/sh"));
    PtyProcess process;
    QByteArray output;
    connect(&process, &PtyProcess::dataReceived, this, [&](const QByteArray &bytes) {
        output += bytes;
    });
    QSignalSpy exited{&process, &PtyProcess::exited};
    QSignalSpy started{&process, &PtyProcess::started};
    QVERIFY(process.start(launch, 80, 24));
    QTRY_COMPARE_WITH_TIMEOUT(started.size(), 1, 5000);
#ifdef Q_OS_MACOS
    // GUI launches use the account's login shell on macOS; an inherited SHELL
    // can name the terminal that launched us rather than the user's shell.
    QCOMPARE(process.program(), KUser{}.shell());
#else
    QCOMPARE(process.program(), QStringLiteral("/bin/sh"));
#endif
    QVERIFY(process.write("printf 'SHELL_VALUE:%s\\n' \"$SHELL\"\n"));
    QTRY_VERIFY(output.contains("SHELL_VALUE:/not/a/shell"));
    QVERIFY(process.write("exit\n"));
    QTRY_COMPARE_WITH_TIMEOUT(exited.size(), 1, 5000);
    QVERIFY(output.contains("SHELL_VALUE:/not/a/shell"));
}

void PtyProcessTest::resolvesCommandInChildContext_data()
{
    QTest::addColumn<QString>("program");
    QTest::addColumn<QString>("path");
    QTest::newRow("absolute-path-entry") << QStringLiteral("probe") << QStringLiteral("@tools");
    QTest::newRow("relative-path-entry") << QStringLiteral("probe") << QStringLiteral("missing:tools");
    QTest::newRow("empty-path-entry") << QStringLiteral("probe") << QStringLiteral("missing:");
    QTest::newRow("empty-path") << QStringLiteral("probe") << QString();
    QTest::newRow("relative-program") << QStringLiteral("./probe") << QStringLiteral("/not/a/search/path");
    QTest::newRow("relative-program-subdirectory") << QStringLiteral("tools/probe") << QStringLiteral("/not/a/search/path");
}

// Command lookup uses the child's directory and PATH, preserving literal arguments and environment overrides.
void PtyProcessTest::resolvesCommandInChildContext()
{
    const QByteArray previous = qgetenv("KWI_CHILD_VALUE");
    QFETCH(QString, program);
    QFETCH(QString, path);
    QTemporaryDir temporary{QDir{QDir::tempPath()}.canonicalPath() + QStringLiteral("/ptyprocesstest-XXXXXX")};
    QVERIFY(temporary.isValid());
    const QString directory = temporary.filePath(QStringLiteral("project with spaces"));
    QVERIFY(QDir().mkpath(directory + QStringLiteral("/tools")));

    for (const QString &relative : {QStringLiteral("probe"), QStringLiteral("tools/probe")}) {
        QFile script{QDir(directory).filePath(relative)};
        QVERIFY(script.open(QIODevice::WriteOnly));
        QVERIFY(script.write(QByteArrayLiteral("#!") + QFile::encodeName(QStringLiteral(KWI_TEST_PYTHON))
                             + "\nimport os,sys,termios\n"
                               "sys.stdout.write('|'.join((os.getcwd(),sys.argv[1],os.environ['KWI_CHILD_VALUE'])))\n"
                               "sys.stdout.flush()\ntermios.tcdrain(1)\n")
                > 0);
        script.close();
        QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    }

    if (path == QLatin1String("@tools")) {
        path = directory + QStringLiteral("/tools");
    }

    auto launch = testLaunch(program, {QStringLiteral("literal $HOME")}, directory, {QStringLiteral("PATH=") + path, QStringLiteral("KWI_CHILD_VALUE=a=b")});
    PtyProcess process;
    QByteArray output;
    connect(&process, &PtyProcess::dataReceived, this, [&output](const QByteArray &bytes) {
        output += bytes;
    });
    QSignalSpy exited{&process, &PtyProcess::exited};
    QVERIFY(process.start(launch, 80, 24));
    QTRY_COMPARE_WITH_TIMEOUT(exited.size(), 1, 5000);
    QCOMPARE(exited.first().first().toInt(), 0);
    QCOMPARE(output, directory.toUtf8() + "|literal $HOME|a=b");
    QCOMPARE(qgetenv("KWI_CHILD_VALUE"), previous);
}

// A process object can launch another child after each completed exit.
void PtyProcessTest::restartsAfterExit()
{
    PtyProcess process;
    QSignalSpy exited{&process, &PtyProcess::exited};

    for (int iteration = 0; iteration < 3; ++iteration) {
        QVERIFY(process.start(testLaunch(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), QStringLiteral("exit 0")}, QDir::tempPath()), 80, 24));
        QTRY_COMPARE_WITH_TIMEOUT(exited.size(), iteration + 1, 5000);
    }
}

QTEST_GUILESS_MAIN(PtyProcessTest)

#include "ptyprocesstest.moc"
