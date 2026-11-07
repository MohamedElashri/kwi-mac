// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: GPL-3.0-or-later
#include "SessionModel.h"
#include "Session.h"
#include "TerminalSession.h"
#include "settings/LaunchSettings.h"
#include "settings/TerminalPresets.h"
#include "settings/TerminalSettings.h"

#include <KLocalizedString>
#include <KShell>
#include <QAbstractItemModelTester>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QLatin1String>
#include <QObject>
#include <QPointer>
#include <QSignalSpy>
#include <QStringList>
#include <QStringLiteral>
#include <QTemporaryDir>
#include <QTest>
#include <signal.h>

using namespace Kwi;

class SessionModelTest : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

private Q_SLOTS:
    void initTestCase();
    void closesAsynchronously();
    void holdsExitedSession();
    void keepsActiveTabWhenRemovingAnother();

    void confirmsForegroundWork();
    void confirmsDirectCommandAndClosesIdleShell();

    void preservesLaunchErrors();

    void removesNaturallyExitedSession();
    void inheritsWorkingDirectory();
    void moveAndLastUsedPreserveIdentity();

private:
    QString m_shellPreset;
};

void SessionModelTest::initTestCase()
{
    TerminalSettings settings;
    TerminalPresets presets{&settings};
    QVERIFY(presets.savePreset(
        QStringLiteral("Test shell"),
        {.command = {QStringLiteral("/bin/sh"), QStringLiteral("-i")}, .environment = {QStringLiteral("ENV="), QStringLiteral("BASH_ENV=")}}));
    const auto preset = presets.preset(QStringLiteral("Test shell"));
    QVERIFY(preset);
    m_shellPreset = preset->id;
}

// Closing a tab finishes asynchronously after terminating a child that ignores HUP and TERM.
void SessionModelTest::closesAsynchronously()
{
    TerminalSettings settings;
    settings.setDefaultPreset(m_shellPreset);
    settings.setConfirmCloseRunning(false);
    TerminalPresets presets{&settings};
    SessionModel model{&settings, &presets};

    QSignalSpy closed{&model, &SessionModel::allClosed};
    model.launchSession(
        {.command = QStringList{QStringLiteral("/bin/sh"), QStringLiteral("-c"), QStringLiteral("trap '' HUP TERM; printf 'READY\\n'; while :; do :; done")},
         .directory = QDir::tempPath()},
        {},
        false);
    QPointer<TerminalSession> session = model.currentSession();
    QSignalSpy exited{session, &TerminalSession::exited};
    QTRY_VERIFY_WITH_TIMEOUT(session->historyText().contains(QStringLiteral("READY")), 5000);
    model.closeSession(0);
    QCOMPARE(model.count(), 0);
    QVERIFY(session);
    QCOMPARE(closed.size(), 0);
    QTRY_COMPARE_WITH_TIMEOUT(closed.size(), 1, 5000);
    QCOMPARE(exited.size(), 1);
    QCOMPARE(exited.first().first().toInt(), 128 + SIGKILL);
    QTRY_VERIFY(session.isNull());
}

// A held tab retains its child's exit status and output until the user closes it.
void SessionModelTest::holdsExitedSession()
{
    TerminalSettings settings;
    settings.setDefaultPreset(m_shellPreset);
    TerminalPresets presets{&settings};
    SessionModel model{&settings, &presets};

    QSignalSpy closed{&model, &SessionModel::allClosed};
    model.launchSession({.command = QStringList{QStringLiteral("/bin/sh"), QStringLiteral("-c"), QStringLiteral("printf 'completed\\n'; read -r ack; exit 9")},
                         .directory = QDir::tempPath()},
                        {},
                        true);
    QSignalSpy exited{model.currentSession(), &TerminalSession::exited};
    QTRY_VERIFY(model.currentSession()->historyText().contains(QStringLiteral("completed")));
    QVERIFY(model.currentSession()->sendInput(QStringLiteral("\x04")));
    QTRY_COMPARE_WITH_TIMEOUT(exited.size(), 1, 5000);
    QCOMPARE(exited.first().first().toInt(), 9);
    QCOMPARE(model.count(), 1);
    QVERIFY(!model.currentSession()->isActive());
    QVERIFY(model.currentSession()->historyText().contains(QStringLiteral("completed")));
    QCOMPARE(closed.size(), 0);
    model.closeAll(true);
    QCOMPARE(closed.size(), 1);
}

// Removing a tab preserves the active session through the resulting index changes.
void SessionModelTest::keepsActiveTabWhenRemovingAnother()
{
    TerminalSettings settings;
    settings.setDefaultPreset(m_shellPreset);
    TerminalPresets presets{&settings};
    SessionModel model{&settings, &presets};
    QAbstractItemModelTester tester{&model, QAbstractItemModelTester::FailureReportingMode::QtTest};
    model.newSession();
    model.newSession();
    QPointer<TerminalSession> middle = model.currentSession();
    model.newSession();
    QPointer<TerminalSession> last = model.currentSession();
    QCOMPARE(model.currentIndex(), 2);
    model.setCurrentIndex(1);
    model.closeSession(0);
    QCOMPARE(model.count(), 2);
    QCOMPARE(model.currentIndex(), 0);
    QCOMPARE(model.currentSession(), middle.data());
    model.closeSession(0);
    QCOMPARE(model.currentIndex(), 0);
    QCOMPARE(model.currentSession(), last.data());
    model.closeAll(true);
    QCOMPARE(model.count(), 0);
    QCOMPARE(model.currentIndex(), -1);
    QVERIFY(!model.currentSession());
}

// Closing a tab or window with foreground work requests confirmation before stopping its process.
void SessionModelTest::confirmsForegroundWork()
{
    TerminalSettings settings;
    settings.setDefaultPreset(m_shellPreset);
    TerminalPresets presets{&settings};
    settings.setConfirmCloseRunning(true);
    SessionModel model{&settings, &presets};

    QSignalSpy confirmation{&model, &SessionModel::closeConfirmationRequested};
    QSignalSpy closed{&model, &SessionModel::allClosed};
    model.launchSession({.command = QStringList{QStringLiteral("/bin/sh"), QStringLiteral("-i")}, .directory = QDir::tempPath()}, {}, false);
    auto *session = model.currentSession();
    QTRY_COMPARE_WITH_TIMEOUT(session->state(), Session::State::Running, 5000);
    QVERIFY(session->send("sleep 30\n"));
    QTRY_VERIFY_WITH_TIMEOUT(session->foregroundCommandName() == QLatin1String("sleep"), 5000);
    model.closeSession(0);
    QTRY_COMPARE(confirmation.size(), 1);
    QCOMPARE(confirmation.first().at(1).value<SessionModel::CloseScope>(), SessionModel::CloseScope::Session);
    QCOMPARE(model.count(), 1);
    model.closeAll();
    QTRY_COMPARE(confirmation.size(), 2);
    QCOMPARE(confirmation.last().at(1).value<SessionModel::CloseScope>(), SessionModel::CloseScope::Window);
    QCOMPARE(model.count(), 1);
    model.closeAll(true);
    QTRY_COMPARE_WITH_TIMEOUT(closed.size(), 1, 5000);
}

// Direct commands require close confirmation and idle shells close immediately.
void SessionModelTest::confirmsDirectCommandAndClosesIdleShell()
{
    TerminalSettings settings;
    settings.setDefaultPreset(m_shellPreset);
    TerminalPresets presets{&settings};
    settings.setConfirmCloseRunning(true);
    SessionModel model{&settings, &presets};

    QSignalSpy confirmation{&model, &SessionModel::closeConfirmationRequested};
    QSignalSpy closed{&model, &SessionModel::allClosed};
    model.launchSession({.command = QStringList{QStringLiteral("sleep"), QStringLiteral("30")}, .directory = QDir::tempPath()}, {}, false);
    QTRY_VERIFY_WITH_TIMEOUT(model.currentSession()->foregroundCommandName() == QLatin1String("sleep"), 5000);
    model.closeSession(0);
    QTRY_COMPARE(confirmation.size(), 1);
    QCOMPARE(model.count(), 1);
    model.confirmClose(confirmation.constLast().constFirst().toULongLong());
    QTRY_COMPARE_WITH_TIMEOUT(closed.size(), 1, 5000);

    model.newSession();
    auto *shell = model.currentSession();
    QTRY_COMPARE_WITH_TIMEOUT(shell->state(), Session::State::Running, 5000);
    QVERIFY(shell->send("printf 'READY%s\\n' '-idle'\n"));
    QTRY_VERIFY_WITH_TIMEOUT(shell->historyText().contains(QStringLiteral("READY-idle")), 5000);
    QTRY_VERIFY(!shell->isBusy());
    model.closeSession(0);
    QTRY_COMPARE_WITH_TIMEOUT(closed.size(), 2, 5000);
    QCOMPARE(confirmation.size(), 1);
}

// A failed launch leaves its tab available for inspection until explicitly closed.
void SessionModelTest::preservesLaunchErrors()
{
    QTemporaryDir directory{QDir{QDir::tempPath()}.canonicalPath() + QStringLiteral("/sessionmodeltest-XXXXXX")};
    QVERIFY(directory.isValid());
    TerminalSettings settings;
    settings.setDefaultPreset(m_shellPreset);
    TerminalPresets presets{&settings};
    SessionModel model{&settings, &presets};

    QSignalSpy closed{&model, &SessionModel::allClosed};
    model.launchSession({.command = QStringList{directory.filePath(QStringLiteral("missing-program"))}, .directory = QDir::tempPath()}, {}, false);
    QTRY_VERIFY_WITH_TIMEOUT(!model.currentSession()->errorString().isEmpty(), 5000);
    QCOMPARE(model.count(), 1);
    QVERIFY(!model.currentSession()->isActive());
    QCOMPARE(closed.size(), 0);
    model.closeAll(true);
    QCOMPARE(closed.size(), 1);
}

// An exited child closes its tab and leaves the model empty.
void SessionModelTest::removesNaturallyExitedSession()
{
    TerminalSettings settings;
    settings.setDefaultPreset(m_shellPreset);
    TerminalPresets presets{&settings};
    SessionModel model{&settings, &presets};

    QSignalSpy closed{&model, &SessionModel::allClosed};
    model.launchSession({.command = QStringList{QStringLiteral("/bin/sh"), QStringLiteral("-c"), QStringLiteral("exit 0")}, .directory = QDir::tempPath()},
                        {},
                        false);
    QTRY_COMPARE_WITH_TIMEOUT(closed.size(), 1, 5000);
    QCOMPARE(model.count(), 0);
    QCOMPARE(model.currentIndex(), -1);
    QVERIFY(!model.currentSession());
}

// A new session starts in the directory reported by the active shell.
void SessionModelTest::inheritsWorkingDirectory()
{
    QTemporaryDir directory{QDir{QDir::tempPath()}.canonicalPath() + QStringLiteral("/sessionmodeltest-XXXXXX")};
    QVERIFY(directory.isValid());
    TerminalSettings settings;
    settings.setDefaultPreset(m_shellPreset);
    TerminalPresets presets{&settings};
    SessionModel model{&settings, &presets};

    model.launchSession({.command = QStringList{QStringLiteral("/bin/sh"), QStringLiteral("-i")}, .directory = QDir::tempPath()}, {}, false);
    auto *first = model.currentSession();
    QTRY_COMPARE_WITH_TIMEOUT(first->state(), Session::State::Running, 5000);
    QVERIFY(first->send(QStringLiteral("cd %1\n").arg(KShell::quoteArg(directory.path())).toUtf8()));
    QTRY_COMPARE_WITH_TIMEOUT(first->workingDirectory(), directory.path(), 5000);
    model.newSession();
    auto *second = model.currentSession();
    QVERIFY(second != first);
    QTRY_COMPARE_WITH_TIMEOUT(second->state(), Session::State::Running, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(second->workingDirectory(), directory.path(), 5000);
    QSignalSpy closed{&model, &SessionModel::allClosed};
    model.closeAll(true);
    QTRY_COMPARE_WITH_TIMEOUT(closed.size(), 1, 5000);
}

// Reordering tabs preserves the active session and the identity of the last-used tab.
void SessionModelTest::moveAndLastUsedPreserveIdentity()
{
    TerminalSettings settings;
    settings.setDefaultPreset(m_shellPreset);
    TerminalPresets presets{&settings};
    SessionModel model{&settings, &presets};
    QAbstractItemModelTester tester{&model, QAbstractItemModelTester::FailureReportingMode::QtTest};
    model.newSession();
    auto *first = model.currentSession();
    model.newSession();
    auto *second = model.currentSession();
    model.newSession();
    auto *third = model.currentSession();
    model.setCurrentIndex(0);
    model.moveSession(0, 2);
    QCOMPARE(model.currentSession(), first);
    QCOMPARE(model.currentIndex(), 2);
    QCOMPARE(model.indexOf(second), 0);
    QCOMPARE(model.indexOf(third), 1);
    model.activateLastUsed();
    QCOMPARE(model.currentSession(), third);
    model.activateLastUsed();
    QCOMPARE(model.currentSession(), first);
    model.moveSession(2, 0);
    QCOMPARE(model.currentSession(), first);
    QCOMPARE(model.currentIndex(), 0);
    model.closeAll(true);
}

int main(int argc, char **argv)
{
    QTemporaryDir configuration{QDir{QDir::tempPath()}.canonicalPath() + QStringLiteral("/sessionmodeltest-XXXXXX")};

    if (!configuration.isValid()) {
        return 1;
    }

    qputenv("XDG_CONFIG_HOME", QFile::encodeName(configuration.path()));
    qputenv("XDG_DATA_HOME", QFile::encodeName(configuration.filePath(QStringLiteral("data"))));
    qputenv("SHELL", "/bin/sh");
    QGuiApplication application{argc, argv};
    KLocalizedString::setApplicationDomain("kwi");
    SessionModelTest test;

    return QTest::qExec(&test, argc, argv);
}

#include "sessionmodeltest.moc"
