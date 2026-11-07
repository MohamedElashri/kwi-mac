// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: GPL-3.0-or-later
#include <KLocalizedString>
#include <KParts/ReadOnlyPart>
#include <KPluginFactory>
#include <KPluginMetaData>
#include <KPtyDevice>
#include <KPtyProcess>
#include <QAction>
#include <QApplication>
#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QIODevice>
#include <QKeyEvent>
#include <QKeySequence>
#include <QList>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QString>
#include <QStringList>
#include <QStringLiteral>
#include <QTemporaryDir>
#include <QTest>
#include <QVBoxLayout>
#include <QWidget>
#include <cerrno>
#include <kde_terminal_interface.h>
#include <signal.h>
#include <utility>

class KwiPartPluginTest : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

private Q_SLOTS:
    void cleanup();
    void multipleInstancesAndChildTeardown();
    void callerOwnedPty();
    void hostShortcutNegotiation();
    void overrideKey(QKeyEvent *, bool &override);

private:
    KParts::ReadOnlyPart *createPart();
    QList<QPointer<KParts::ReadOnlyPart>> m_parts;
    QList<int> m_children;
    bool m_allowTerminalShortcut{false};
};

static QString pluginPath()
{
    return qEnvironmentVariable("KWI_TEST_PART_PLUGIN", QStringLiteral(KWI_PART_PLUGIN_PATH));
}

KParts::ReadOnlyPart *KwiPartPluginTest::createPart()
{
    const auto result = KPluginFactory::instantiatePlugin<KParts::ReadOnlyPart>(KPluginMetaData(pluginPath()), this);

    if (!result) {
        qWarning() << result.errorString;

        return nullptr;
    }

    m_parts.append(result.plugin);
    return result.plugin;
}

void KwiPartPluginTest::cleanup()
{
    for (const auto &part : std::as_const(m_parts)) {
        delete part;
    }

    m_parts.clear();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    const auto children = std::exchange(m_children, {});

    for (const int pid : children) {
        QTRY_VERIFY_WITH_TIMEOUT(kill(pid_t(pid), 0) < 0 && errno == ESRCH, 3000);
    }
}

// Deleting one Part reaps its child and leaves another Part's terminal usable.
void KwiPartPluginTest::multipleInstancesAndChildTeardown()
{
    QTemporaryDir directory{QDir{QDir::tempPath()}.canonicalPath() + QStringLiteral("/kpartplugintest-XXXXXX")};
    QVERIFY(directory.isValid());
    QWidget host;
    QVBoxLayout layout{&host};
    auto *first = createPart();
    auto *second = createPart();
    QVERIFY(first && second);
    layout.addWidget(first->widget());
    layout.addWidget(second->widget());
    auto *firstTerminal = qobject_cast<TerminalInterface *>(first);
    auto *secondTerminal = qobject_cast<TerminalInterface *>(second);
    QVERIFY(firstTerminal && secondTerminal);
    firstTerminal->showShellInDir(directory.path());
    QTRY_VERIFY_WITH_TIMEOUT(firstTerminal->terminalProcessId() > 0, 3000);
    const int firstPid{firstTerminal->terminalProcessId()};
    m_children.append(firstPid);

    const QString output{directory.filePath(QStringLiteral("received-input"))};
    secondTerminal->startProgram(QStringLiteral("/bin/sh"),
                                 {QStringLiteral("kwi-plugin-probe"),
                                  QStringLiteral("-c"),
                                  QStringLiteral("while IFS= read -r value; do printf '%s\\n' \"$value\" >> \"$1\"; done"),
                                  QStringLiteral("probe"),
                                  output});
    host.resize(700, 600);
    host.show();
    QVERIFY(QTest::qWaitForWindowExposed(&host));
    QTRY_VERIFY_WITH_TIMEOUT(secondTerminal->terminalProcessId() > 0, 3000);
    const int secondPid{secondTerminal->terminalProcessId()};
    m_children.append(secondPid);
    secondTerminal->sendInput(QStringLiteral("plugin input 水\n"));
    const auto received = [&output] {
        QFile file{output};

        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
    };
    QTRY_COMPARE_WITH_TIMEOUT(received(), QByteArray("plugin input 水\n"), 3000);
    delete first;
    QTRY_VERIFY_WITH_TIMEOUT(kill(pid_t(firstPid), 0) < 0 && errno == ESRCH, 3000);
    m_children.removeAll(firstPid);
    secondTerminal->sendInput(QStringLiteral("surviving input\n"));
    QTRY_COMPARE_WITH_TIMEOUT(received(), QByteArray("plugin input 水\nsurviving input\n"), 3000);

    delete second;
    QTRY_VERIFY_WITH_TIMEOUT(kill(pid_t(secondPid), 0) < 0 && errno == ESRCH, 3000);
    m_children.removeAll(secondPid);
}

// Deleting a Part leaves the caller's PTY and child available for further input.
void KwiPartPluginTest::callerOwnedPty()
{
    QTemporaryDir directory{QDir{QDir::tempPath()}.canonicalPath() + QStringLiteral("/kpartplugintest-XXXXXX")};
    QVERIFY(directory.isValid());
    const QString output{directory.filePath(QStringLiteral("received-input"))};
    KPtyProcess process;
    process.setProgram(QStringLiteral("/bin/sh"),
                       {QStringLiteral("-c"),
                        QStringLiteral("IFS= read -r line; printf '%s\\n' \"$line\" > \"$1\"; "
                                       "IFS= read -r line; printf '%s\\n' \"$line\" >> \"$1\"; exec sleep 30"),
                        QStringLiteral("external-pty"),
                        output});
    process.setPtyChannels(KPtyProcess::AllChannels);
    process.pty()->setSuspended(true);
    process.start();
    const auto stop = qScopeGuard([&process] {
        if (process.state() != QProcess::NotRunning) {
            process.kill();
            process.waitForFinished(5000);
        }
    });
    QVERIFY(process.waitForStarted());
    const int master{process.pty()->masterFd()};
    auto *part = createPart();
    QVERIFY(part);
    QVERIFY(QMetaObject::invokeMethod(part, "openTeletype", Q_ARG(int, master), Q_ARG(bool, false)));
    auto *terminal = qobject_cast<TerminalInterface *>(part);
    QVERIFY(terminal);
    terminal->sendInput(QStringLiteral("caller-owned 水\n"));
    const auto received = [&output] {
        QFile file{output};

        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
    };
    QTRY_COMPARE(received(), QByteArray("caller-owned 水\n"));
    delete part;
    const QByteArray callerInput{"after detach\n"};
    QCOMPARE(process.pty()->write(callerInput), callerInput.size());
    QTRY_COMPARE(received(), QByteArray("caller-owned 水\nafter detach\n"));
    process.kill();
    QVERIFY(process.waitForFinished(5000));
}

void KwiPartPluginTest::overrideKey(QKeyEvent *, bool &override)
{
    override = m_allowTerminalShortcut;
}

// The host can reserve a shortcut or let the terminal receive it as input.
void KwiPartPluginTest::hostShortcutNegotiation()
{
    QTemporaryDir directory{QDir{QDir::tempPath()}.canonicalPath() + QStringLiteral("/kpartplugintest-XXXXXX")};
    QVERIFY(directory.isValid());
    const QString output{directory.filePath(QStringLiteral("received-key"))};
    const QString ready{directory.filePath(QStringLiteral("ready"))};
    QWidget host;
    QVBoxLayout layout{&host};
    auto *part = createPart();
    QVERIFY(part);
    layout.addWidget(part->widget());
    auto *terminal = qobject_cast<TerminalInterface *>(part);
    QVERIFY(terminal);
    terminal->startProgram(QStringLiteral("/bin/sh"),
                           {QStringLiteral("sh"),
                            QStringLiteral("-c"),
                            QStringLiteral("stty raw -echo; : > \"$2\"; dd bs=1 count=1 of=\"$1\" 2>/dev/null; exec sleep 30"),
                            QStringLiteral("keyboard-host"),
                            output,
                            ready});
    QTRY_VERIFY(terminal->terminalProcessId() > 0);
    m_children.append(terminal->terminalProcessId());
    QTRY_VERIFY(QFile::exists(ready));
    QVERIFY(connect(part, SIGNAL(overrideShortcut(QKeyEvent *, bool &)), this, SLOT(overrideKey(QKeyEvent *, bool &)), Qt::DirectConnection));
    QAction action{&host};
    action.setShortcuts({QKeySequence{Qt::CTRL | Qt::Key_K}, QKeySequence{Qt::CTRL | Qt::Key_L}});
    host.addAction(&action);
    QSignalSpy triggered{&action, &QAction::triggered};
    host.resize(700, 400);
    host.show();
    QVERIFY(QTest::qWaitForWindowExposed(&host));
    host.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&host));
    auto *focus = part->widget()->focusWidget();
    QVERIFY(focus);
    focus->setFocus();
    m_allowTerminalShortcut = false;
    QTest::keyClick(focus, Qt::Key_K, Qt::ControlModifier);
    QTRY_COMPARE(triggered.size(), 1);

    m_allowTerminalShortcut = true;
    QTest::keyClick(focus, Qt::Key_L, Qt::ControlModifier);
    const auto received = [&output] {
        QFile file{output};

        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
    };
    QTRY_COMPARE(received(), QByteArray(1, '\x0c'));
    QCOMPARE(triggered.size(), 1);
}

int main(int argc, char **argv)
{
    QTemporaryDir config{QDir{QDir::tempPath()}.canonicalPath() + QStringLiteral("/kpartplugintest-XXXXXX")};
    QTemporaryDir data{QDir{QDir::tempPath()}.canonicalPath() + QStringLiteral("/kpartplugintest-XXXXXX")};

    if (!config.isValid() || !data.isValid()) {
        return 1;
    }

    qputenv("XDG_CONFIG_HOME", config.path().toUtf8());
    qputenv("XDG_DATA_HOME", data.path().toUtf8());
    qputenv("SHELL", "/bin/sh");
    qputenv("ENV", "");
    qputenv("BASH_ENV", "");
    QApplication application{argc, argv};
    application.setApplicationName(QStringLiteral("kwi-plugin-test-host"));
    application.setDesktopFileName(QStringLiteral("org.kde.kwi-plugin-test-host"));
    KLocalizedString::setApplicationDomain("kwi-plugin-test-host");
    KwiPartPluginTest test;

    return QTest::qExec(&test, argc, argv);
}

#include "kpartplugintest.moc"
