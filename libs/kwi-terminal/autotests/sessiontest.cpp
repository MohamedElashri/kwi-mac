// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Session.h"
#include "ProcessTestSupport.h"

#include <QByteArray>
#include <QChar>
#include <QDir>
#include <QLatin1Char>
#include <QObject>
#include <QString>
#include <QStringLiteral>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

using namespace Kwi;

class SessionTest : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

private Q_SLOTS:
    void sanitizesApplicationTitles();
    void repeatedStartPreservesRunningSession();
    void restartClearsProcessMetadataAndRetainsHistory();
};

// Application titles remove control characters and remain bounded without splitting a surrogate pair.
void SessionTest::sanitizesApplicationTitles()
{
    Session session;
    const QString unsafe = QStringLiteral("unsafe") + QChar{0x85} + QChar{0xfdd0} + QChar{0x7f} + QString(300, QLatin1Char('a'));
    session.feed(QByteArray("\x1b]2;") + unsafe.toUtf8() + QByteArray("\x1b\\"));
    QVERIFY(session.applicationTitle().startsWith(QStringLiteral("unsafe")));
    QCOMPARE(session.applicationTitle().size(), 256);
    const QString title{session.applicationTitle()};

    for (const QChar character : title) {
        QVERIFY(character.category() != QChar::Other_Control);
        QVERIFY(!character.isNonCharacter());
    }

    const QString longTitle{QString(255, QLatin1Char('a')) + QStringLiteral("😀")};
    session.feed(QByteArray("\x1b]2;") + longTitle.toUtf8() + QByteArray("\x07"));
    QCOMPARE(session.applicationTitle(), QString(255, QLatin1Char('a')));
}

// A second start request leaves the running child and its input channel intact.
void SessionTest::repeatedStartPreservesRunningSession()
{
    Session session;
    QVERIFY(session.start(
        testLaunch(QStringLiteral("/bin/sh"),
                   {QStringLiteral("-c"), QStringLiteral("stty -echo; printf 'ready\\n'; read -r line; printf 'received:%s\\n' \"$line\"; read -r ack")},
                   {},
                   {QStringLiteral("ENV="), QStringLiteral("BASH_ENV=")})));
    QTRY_VERIFY(session.historyText().contains(QStringLiteral("ready")));
    const auto pid{session.processId()};
    QVERIFY(pid > 0);

    QVERIFY(!session.start(
        testLaunch(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), QStringLiteral("exit 99")}, {}, {QStringLiteral("ENV="), QStringLiteral("BASH_ENV=")})));
    QCOMPARE(session.processId(), pid);
    QVERIFY(session.isActive());
    QVERIFY(session.sendInput(QStringLiteral("still-running\n")));
    QTRY_VERIFY(session.historyText().contains(QStringLiteral("received:still-running")));
    QVERIFY(session.sendInput(QStringLiteral("finish\n")));
    QTRY_VERIFY(session.isFinished());
    QCOMPARE(session.exitCode(), 0);
}

// Restarting a session clears process metadata and preserves its terminal history.
void SessionTest::restartClearsProcessMetadataAndRetainsHistory()
{
    Session session;
    QTemporaryDir firstDirectory{QDir{QDir::tempPath()}.canonicalPath() + QStringLiteral("/sessiontest-XXXXXX")};
    QTemporaryDir secondDirectory{QDir{QDir::tempPath()}.canonicalPath() + QStringLiteral("/sessiontest-XXXXXX")};
    QVERIFY(firstDirectory.isValid());
    QVERIFY(secondDirectory.isValid());

    const QString firstUrl{QString::fromUtf8(QUrl::fromLocalFile(firstDirectory.path()).toEncoded())};
    const QString firstCommand{QStringLiteral("printf '\\033]7;%s\\007\\033]2;First process\\007\\033]9;4;1;42\\007old-process\\n' \"$1\"; IFS= read -r line")};
    QVERIFY(session.start(testLaunch(QStringLiteral("/bin/sh"),
                                     {QStringLiteral("-c"), firstCommand, QStringLiteral("test"), firstUrl},
                                     firstDirectory.path(),
                                     {QStringLiteral("ENV="), QStringLiteral("BASH_ENV=")})));
    QTRY_VERIFY(session.historyText().contains(QStringLiteral("old-process")));
    QCOMPARE(session.workingDirectory(), firstDirectory.path());
    QCOMPARE(session.applicationTitle(), QStringLiteral("First process"));
    QCOMPARE(session.progressPercent(), 42);
    QVERIFY(session.sendInput(QStringLiteral("\n")));
    QTRY_VERIFY(session.isFinished());
    QCOMPARE(session.exitCode(), 0);
    QCOMPARE(session.progressState(), Session::ProgressState::None);
    QCOMPARE(session.progressPercent(), -1);

    QVERIFY(session.start(testLaunch(QStringLiteral("cat"), {}, secondDirectory.path())));
    QCOMPARE(session.progressState(), Session::ProgressState::None);
    QCOMPARE(session.progressPercent(), -1);
    QVERIFY(!session.hasApplicationTitle());
    QVERIFY(session.applicationTitle().isEmpty());
    QTRY_COMPARE(session.state(), Session::State::Running);
    QCOMPARE(session.launchedProgramName(), QStringLiteral("cat"));
    QCOMPARE(session.workingDirectory(), secondDirectory.path());
    QVERIFY(session.sendInput(QStringLiteral("second-process\n")));
    QTRY_VERIFY(session.historyText().contains(QStringLiteral("second-process")));
    QVERIFY(session.historyText().contains(QStringLiteral("old-process")));
    QCOMPARE(session.workingDirectory(), secondDirectory.path());
    QCOMPARE(session.progressState(), Session::ProgressState::None);
    QCOMPARE(session.progressPercent(), -1);
    QVERIFY(!session.hasApplicationTitle());
    QVERIFY(session.applicationTitle().isEmpty());
    session.close();
    QTRY_VERIFY(session.isFinished());
}

QTEST_MAIN(SessionTest)

#include "sessiontest.moc"
