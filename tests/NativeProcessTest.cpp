// SPDX-FileCopyrightText: 2026 Kwi contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "ProcessLaunch.h"
#include "process/DarwinProcessArguments_p.h"
#include "process/NativeLaunch_p.h"
#include "process/NativeProcessInfo_p.h"
#include "process/NativeProcess_p.h"
#include <KPtyDevice>
#include <KUser>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThreadPool>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <util.h>

class NativeProcessTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void destroyDuringStart()
    {
        auto process = std::make_unique<Kwi::NativeProcess>();
        QVERIFY(process->open(-1));
        Kwi::TerminalLaunch launch;
        launch.program = QStringLiteral("/bin/sh");
        launch.arguments = {QStringLiteral("-c"), QStringLiteral("sleep 60")};
        QVERIFY(process->start(launch));
        process.reset();
        QVERIFY2(QThreadPool::globalInstance()->waitForDone(8000), "Starting-process cleanup must not leave an unbounded worker wait");
    }

    void argumentRecord()
    {
        int argc{4};
        QByteArray record{reinterpret_cast<const char *>(&argc), sizeof(argc)};
        record.append("/bin/zsh", 9);
        while ((record.size() - sizeof(argc)) % 8)
            record.append('\0');
        const char payload[]{"-zsh\0space argument\0\0Unicode-é\0PRIVATE_ENV=must-not-escape\0"};
        record.append(payload, sizeof(payload) - 1);
        const char expected[]{"-zsh\0space argument\0\0Unicode-é\0"};
        QCOMPARE(Kwi::NativeProcessInfo::darwinArguments(record), QByteArray(expected, sizeof(expected) - 1));
        QVERIFY(Kwi::NativeProcessInfo::darwinArguments(record.first(10)).isEmpty());
        argc = 999999;
        std::memcpy(record.data(), &argc, sizeof(argc));
        QVERIFY(Kwi::NativeProcessInfo::darwinArguments(record).isEmpty());
    }

    void nativeArguments_data()
    {
        QTest::addColumn<QByteArray>("first");
        QTest::newRow("empty-argv0") << QByteArray{};
        QTest::newRow("unusual-argv0") << QByteArray{"unusual program name"};
        QTest::newRow("login-argv0") << QByteArray{"-sleep"};
    }

    void nativeArguments()
    {
        QFETCH(QByteArray, first);
        char *arguments[]{first.data(), const_cast<char *>("20"), nullptr};
        char *environment[]{const_cast<char *>("KWI_PRIVATE=MUST_NOT_APPEAR"), nullptr};
        const pid_t child{fork()};
        QVERIFY(child >= 0);
        if (child == 0) {
            execve("/bin/sleep", arguments, environment);
            _exit(127);
        }
        const auto cleanup = qScopeGuard([child] {
            kill(child, SIGKILL);
            waitpid(child, nullptr, 0);
        });
        QTRY_COMPARE_WITH_TIMEOUT(Kwi::NativeProcessInfo::executable(child), QStringLiteral("/bin/sleep"), 3000);
        const auto bytes = Kwi::NativeProcessInfo::commandLine(child);
        QCOMPARE(bytes, first + '\0' + QByteArray("20\0", 3));
        QVERIFY(!bytes.contains("KWI_PRIVATE"));
        QVERIFY(Kwi::NativeProcessInfo::startTime(child) > 0);
    }

    void environmentAndLocale()
    {
        Kwi::NativeProcess process;
        QVERIFY(process.open(-1));
        QByteArray output;
        connect(process.device(), &QIODevice::readyRead, this, [&] {
            output += process.device()->readAll();
        });
        Kwi::TerminalLaunch launch;
        launch.inheritedEnvironment = {};
        launch.program = QStringLiteral("/bin/sh");
        launch.arguments = {QStringLiteral("-c"), QStringLiteral("/usr/bin/locale charmap; /usr/bin/env")};
        launch.inheritedEnvironment.insert(QStringLiteral("CUSTOM"), QStringLiteral("inherited"));
        launch.inheritedEnvironment.insert(QStringLiteral("LINES"), QStringLiteral("7"));
        launch.environmentChanges.insert(QStringLiteral("CUSTOM"), QStringLiteral("override literal ; $HOME"));
        launch.environmentChanges.insert(QStringLiteral("COLORTERM"), std::nullopt);
        QVERIFY(process.start(launch));
        QTRY_VERIFY_WITH_TIMEOUT(!process.isRunning(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(output.contains("TERM_PROGRAM=kwi"), 3000);
        QVERIFY(output.startsWith("UTF-8\r\n"));
        QVERIFY(output.contains("CUSTOM=override literal ; $HOME\r\n"));
        QVERIFY(output.contains("TERM=xterm-256color\r\n"));
        QVERIFY(!output.contains("COLORTERM="));
        QVERIFY(!output.contains("LINES="));
        launch.environmentChanges.insert(QStringLiteral("LANG"), QStringLiteral("C"));
        QCOMPARE(launch.environment().value(QStringLiteral("LANG")), QStringLiteral("C"));
        launch.environmentChanges.insert(QStringLiteral("LANG"), std::nullopt);
        QVERIFY(!launch.environment().contains(QStringLiteral("LANG")));
    }

    void failedLaunchCleanup()
    {
        for (int n{0}; n < 5; ++n) {
            Kwi::NativeProcess process;
            QVERIFY(process.open(-1));
            QSignalSpy errors{&process, &Kwi::ProcessController::errorOccurred};
            QSignalSpy finished{&process, &Kwi::ProcessController::finished};
            Kwi::TerminalLaunch launch;
            launch.program = QStringLiteral("/bin/sh");
            launch.directory = QStringLiteral("/nonexistent/kwi-test-directory");
            QVERIFY(process.start(launch));
            QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
            QCOMPARE(finished[0][0].toInt(), 127);
            QVERIFY(!errors.isEmpty());
            QVERIFY(!process.isRunning());
        }
    }

    void resistantCloseDoesNotKillUnrelatedProcess()
    {
        QProcess unrelated;
        unrelated.start(QStringLiteral("/bin/sleep"), {QStringLiteral("20")});
        QVERIFY(unrelated.waitForStarted(3000));
        const auto cleanup = qScopeGuard([&] {
            unrelated.kill();
            unrelated.waitForFinished(3000);
        });
        Kwi::NativeProcess process;
        QVERIFY(process.open(-1));
        QByteArray output;
        connect(process.device(), &QIODevice::readyRead, this, [&] {
            output += process.device()->readAll();
        });
        Kwi::TerminalLaunch launch;
        launch.program = QStringLiteral("/bin/sh");
        launch.arguments = {QStringLiteral("-c"), QStringLiteral("trap '' HUP TERM; printf KWI_RESISTANT_READY; exec /bin/sleep 20")};
        QVERIFY(process.start(launch));
        QTRY_VERIFY_WITH_TIMEOUT(output.contains("KWI_RESISTANT_READY"), 3000);
        const auto pid = pid_t(process.processId());
        QElapsedTimer elapsed;
        elapsed.start();
        process.close();
        QVERIFY(elapsed.elapsed() < 100);
        QTRY_VERIFY_WITH_TIMEOUT(!process.isRunning(), 4000);
        QVERIFY(elapsed.elapsed() < 4000);
        QTRY_VERIFY_WITH_TIMEOUT(kill(pid, 0) < 0 && errno == ESRCH, 3000);
        QCOMPARE(unrelated.state(), QProcess::Running);
    }

    void attachedDescriptorDoesNotOwnChild()
    {
        int master{-1}, slave{-1};
        QVERIFY(openpty(&master, &slave, nullptr, nullptr, nullptr) == 0);
        const auto cleanup = qScopeGuard([&] {
            ::close(master);
            ::close(slave);
        });
        const pid_t child{fork()};
        QVERIFY(child >= 0);
        if (child == 0) {
            ::close(master);
            if (setsid() < 0 || ioctl(slave, TIOCSCTTY, 0) < 0)
                _exit(1);
            sleep(20);
            _exit(0);
        }
        const auto childCleanup = qScopeGuard([child] {
            kill(child, SIGKILL);
            waitpid(child, nullptr, 0);
        });
        QTRY_COMPARE_WITH_TIMEOUT(tcgetsid(master), child, 3000);
        auto attached = Kwi::attachedProcess();
        QVERIFY(attached->open(master));
        QVERIFY(attached->start({}));
        QByteArray output;
        connect(attached->device(), &QIODevice::readyRead, this, [&] {
            output += attached->device()->readAll();
        });
        QVERIFY(::write(slave, "KWI_ATTACHED\n", 13) == 13);
        QTRY_VERIFY_WITH_TIMEOUT(output.contains("KWI_ATTACHED"), 3000);
        attached->close();
        QTRY_VERIFY_WITH_TIMEOUT(!attached->isRunning(), 3000);
        QVERIFY(kill(child, 0) == 0);
        QVERIFY(fcntl(master, F_GETFD) >= 0);
        char bytes[64];
        QVERIFY(::write(slave, "AFTER_DETACH\n", 13) == 13);
        pollfd ready{master, POLLIN, 0};
        QVERIFY(poll(&ready, 1, 1000) > 0);
        QVERIFY(::read(master, bytes, sizeof(bytes)) > 0);
    }

    void attachedUnknownForegroundIsBusy()
    {
        int master{-1}, slave{-1};
        QVERIFY(openpty(&master, &slave, nullptr, nullptr, nullptr) == 0);
        const auto cleanup = qScopeGuard([&] {
            ::close(master);
            ::close(slave);
        });
        auto attached = Kwi::attachedProcess();
        QVERIFY(attached->open(master));
        QVERIFY(attached->start({}));
        attached->refreshProcessInfo();
        QVERIFY(attached->processInfo().busy);
        attached->close();
        QTRY_VERIFY_WITH_TIMEOUT(!attached->isRunning(), 3000);
    }

    void selfAndMissingProcess()
    {
        using namespace Kwi::NativeProcessInfo;
        QCOMPARE(QFileInfo(directory(getpid())).canonicalFilePath(), QDir::current().canonicalPath());
        QCOMPARE(QFileInfo(executable(getpid())).canonicalFilePath(), QFileInfo(QCoreApplication::applicationFilePath()).canonicalFilePath());
        QVERIFY(startTime(getpid()) > 0);
        const auto args = commandLine(getpid());
        QVERIFY(!args.isEmpty());
        QVERIFY(args.endsWith('\0'));
        QVERIFY(directory(-1).isEmpty());
        QVERIFY(executable(-1).isEmpty());
        QVERIFY(commandLine(-1).isEmpty());
        QCOMPARE(startTime(-1), quint64(0));
        QCOMPARE(groupMember(-1, -1), pid_t(-1));
    }

    void rewrittenNativeArguments()
    {
        QTemporaryDir directory;
        const auto ready = directory.filePath(QStringLiteral("ready"));
        QProcess child;
        QProcessEnvironment environment;
        environment.insert(QStringLiteral("KWI_PRIVATE"), QStringLiteral("MUST_NOT_APPEAR"));
        child.setProcessEnvironment(environment);
        child.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--rewrite-argv"), ready});
        const auto cleanup = qScopeGuard([&] {
            child.kill();
            child.waitForFinished(3000);
        });
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(ready), 3000);
        const auto arguments = Kwi::NativeProcessInfo::commandLine(pid_t(child.processId()));
        QVERIFY(arguments.startsWith("rewritten"));
        QVERIFY(arguments.contains("--rewrite-argv"));
        QVERIFY(!arguments.contains("KWI_PRIVATE"));
    }

    void defaultAndExplicitShell()
    {
        Kwi::TerminalLaunch launch;
#ifdef Q_OS_MACOS
        QCOMPARE(Kwi::nativeLaunchArguments(launch, QStringLiteral("/bin/zsh")), QStringList{QStringLiteral("-l")});
        auto env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("SHELL"), QStringLiteral("/missing/test-shell"));
        QCOMPARE(Kwi::systemShell(env), KUser{}.shell());
#endif
        launch.program = QStringLiteral("/bin/sh");
        launch.arguments = {QStringLiteral("-c"), QStringLiteral("printf '%s' 'space argument'")};
        QCOMPARE(Kwi::nativeLaunchArguments(launch, launch.program), launch.arguments);
        launch.program.clear();
        launch.arguments.clear();
        QVERIFY(Kwi::nativeLaunchArguments(launch, QStringLiteral("/some/custom-shell")).isEmpty());
    }

    void loginShellStartupFiles()
    {
        if (QFileInfo(KUser{}.shell()).fileName() != QLatin1String("zsh"))
            QSKIP("This fixture records the reference Mac's zsh account-shell behavior");
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        for (const auto &entry : {std::pair{".zshenv", "export KWI_ZSHENV=seen\n"},
                                  std::pair{".zprofile", "export KWI_PROFILE=seen\n"},
                                  std::pair{".zshrc", "export KWI_RC=seen\nPS1='KWI_DEFAULT_READY> '\n"}}) {
            QFile file{directory.filePath(QString::fromLatin1(entry.first))};
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(entry.second);
        }
        Kwi::NativeProcess process;
        QVERIFY(process.open(-1));
        QByteArray output;
        connect(process.device(), &QIODevice::readyRead, this, [&] {
            output += process.device()->readAll();
        });
        Kwi::TerminalLaunch launch;
        launch.inheritedEnvironment = {};
        launch.inheritedEnvironment.insert(QStringLiteral("HOME"), directory.path());
        launch.inheritedEnvironment.insert(QStringLiteral("PATH"), QStringLiteral("/usr/bin:/bin:/usr/sbin:/sbin"));
        launch.inheritedEnvironment.insert(QStringLiteral("SHELL"), QStringLiteral("/nonexistent/inherited-shell"));
        QVERIFY(process.start(launch));
        QTRY_VERIFY_WITH_TIMEOUT(output.contains("KWI_DEFAULT_READY>"), 5000);
        process.device()->write("printf 'KWI_DEFAULT:%s:%s:%s:%s\\n' \"$options[login]\" \"$options[interactive]\" \"$KWI_PROFILE\" \"$KWI_RC\"\n");
        QTRY_VERIFY_WITH_TIMEOUT(output.contains("KWI_DEFAULT:on:on:seen:seen"), 5000);
        process.device()->write("exit\n");
        QTRY_VERIFY_WITH_TIMEOUT(!process.isRunning(), 3000);

        Kwi::NativeProcess explicitCommand;
        QVERIFY(explicitCommand.open(-1));
        QByteArray explicitOutput;
        connect(explicitCommand.device(), &QIODevice::readyRead, this, [&] {
            explicitOutput += explicitCommand.device()->readAll();
        });
        launch.program = QStringLiteral("/bin/zsh");
        launch.arguments = {QStringLiteral("-c"),
                            QStringLiteral("printf 'KWI_EXPLICIT:%s:%s:%s' \"$KWI_ZSHENV\" \"${KWI_PROFILE:-absent}\" \"${KWI_RC:-absent}\"; read -r reply")};
        QVERIFY(explicitCommand.start(launch));
        // This checks startup policy, not immediate-exit tail delivery (covered
        // separately by pty-output). Keep the child alive until the observation.
        QTRY_VERIFY2_WITH_TIMEOUT(explicitOutput.contains("KWI_EXPLICIT:seen:absent:absent"), qPrintable(QString::fromUtf8(explicitOutput)), 3000);
        explicitCommand.device()->write("release\n");
        QTRY_VERIFY_WITH_TIMEOUT(!explicitCommand.isRunning(), 3000);
    }

    void localTerminfoAndDeniedProcess()
    {
        QTemporaryDir directory;
        QProcess query;
        QProcessEnvironment environment;
        environment.insert(QStringLiteral("HOME"), directory.path());
        environment.insert(QStringLiteral("PATH"), QStringLiteral("/usr/bin:/bin:/usr/sbin:/sbin"));
        query.setProcessEnvironment(environment);
        query.start(QStringLiteral("/usr/bin/infocmp"), {QStringLiteral("-1"), QStringLiteral("xterm-256color")});
        QVERIFY(query.waitForFinished(3000));
        QCOMPARE(query.exitCode(), 0);
        const auto capabilities = query.readAllStandardOutput();
        QVERIFY(capabilities.contains("colors#256") || capabilities.contains("colors#0x100"));
        if (geteuid() != 0)
            QVERIFY(Kwi::NativeProcessInfo::commandLine(1).isEmpty());
    }

    void ptyLifecycle()
    {
        Kwi::NativeProcess process;
        QVERIFY(process.open(-1));
        QByteArray output;
        connect(process.device(), &QIODevice::readyRead, this, [&] {
            output += process.device()->readAll();
        });
        QSignalSpy started{&process, &Kwi::ProcessController::started};
        QSignalSpy finished{&process, &Kwi::ProcessController::finished};
        QTemporaryDir directory{QDir::tempPath() + QString::fromUtf8("/kwi cwd-é-XXXXXX")};
        QVERIFY(directory.isValid());
        Kwi::TerminalLaunch launch;
        launch.program = QStringLiteral("/bin/sh");
        launch.arguments = {QStringLiteral("-i")};
        launch.directory = directory.path();
        launch.inheritedEnvironment.insert(QStringLiteral("PS1"), QStringLiteral("KWI_READY> "));
        QVERIFY(process.start(launch));
        QTRY_COMPARE_WITH_TIMEOUT(started.count(), 1, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(output.contains("KWI_READY>"), 5000);
        const pid_t pid{pid_t(process.processId())};
        const auto busy = [&] {
            process.refreshProcessInfo();
            return process.processInfo().busy;
        };
        QTRY_VERIFY_WITH_TIMEOUT(!busy(), 5000);
        QCOMPARE(QFileInfo(process.processInfo().directory).canonicalFilePath().normalized(QString::NormalizationForm_C),
                 QFileInfo(directory.path()).canonicalFilePath().normalized(QString::NormalizationForm_C));
        QVERIFY(process.device()->setWinSize(40, 120));
        process.device()->write("stty size; printf 'SIZE_DONE\\n'\n");
        QTRY_VERIFY_WITH_TIMEOUT(output.contains("40 120"), 5000);
        output.clear();
        process.device()->write("sleep 60\n");
        QTRY_VERIFY_WITH_TIMEOUT(busy() && process.processInfo().foregroundName == QLatin1String("sleep"), 5000);
        process.device()->write(QByteArray(1, '\x1a'));
        QTRY_VERIFY_WITH_TIMEOUT(!busy(), 5000);
        process.device()->write("fg\n");
        QTRY_VERIFY_WITH_TIMEOUT(busy(), 5000);
        process.device()->write(QByteArray(1, '\x03'));
        QTRY_VERIFY_WITH_TIMEOUT(!busy(), 5000);
        process.device()->write("cd /; printf 'CWD_DONE\\n'\n");
        QTRY_VERIFY_WITH_TIMEOUT(output.contains("CWD_DONE"), 5000);
        QTRY_VERIFY_WITH_TIMEOUT((process.refreshProcessInfo(), process.processInfo().directory == QLatin1String("/")), 5000);
        process.close();
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(kill(pid, 0) < 0 && errno == ESRCH, 5000);
    }

    void groupAfterLeaderExit()
    {
        int pipeFds[2];
        QVERIFY(pipe(pipeFds) == 0);
        const pid_t leader{fork()};
        QVERIFY(leader >= 0);
        if (leader == 0) {
            close(pipeFds[0]);
            if (setsid() < 0)
                _exit(1);
            const pid_t child{fork()};
            if (child == 0) {
                close(pipeFds[1]);
                sleep(15); // Bounded even if the test's parent fails.
                _exit(0);
            }
            (void)write(pipeFds[1], &child, sizeof(child));
            _exit(child < 0 ? 1 : 0);
        }
        close(pipeFds[1]);
        pollfd fd{pipeFds[0], POLLIN, 0};
        const int ready{poll(&fd, 1, 3000)};
        pid_t child{-1};
        const auto count = ready > 0 ? read(pipeFds[0], &child, sizeof(child)) : -1;
        close(pipeFds[0]);
        int status{};
        waitpid(leader, &status, 0);
        QVERIFY(count == sizeof(child) && child > 0);
        const auto member = Kwi::NativeProcessInfo::groupMember(leader, leader);
        const auto wrongSession = Kwi::NativeProcessInfo::groupMember(leader, getpid());
        kill(child, SIGKILL);
        QCOMPARE(member, child);
        QCOMPARE(wrongSession, pid_t(-1));
    }
};

int main(int argc, char **argv)
{
    if (argc == 3 && !std::strcmp(argv[1], "--rewrite-argv")) {
        const auto length = std::strlen(argv[0]);
        if (length < 9)
            return 1;
        std::memset(argv[0], ' ', length);
        std::memcpy(argv[0], "rewritten", 9);
        const int fd = ::open(argv[2], O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0)
            return 1;
        (void)::write(fd, "ready", 5);
        ::close(fd);
        sleep(10);
        return 0;
    }
    QCoreApplication app{argc, argv};
    NativeProcessTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "NativeProcessTest.moc"
