// SPDX-FileCopyrightText: 2026 Eike Hein <sho@eikehein.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "process/HostProcessProtocol_p.h"
#include "process/ProcessQueryLimits_p.h"

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QStringLiteral>
#include <QTest>

#include <initializer_list>

using namespace Kwi::HostProcessProtocol;

namespace
{
QByteArray fields(std::initializer_list<QByteArray> values)
{
    QByteArray reply;

    for (const auto &value : values) {
        reply += value;
        reply += '\0';
    }

    return reply;
}

QByteArray metadata(const QByteArray &start = "1234", const QByteArray &foreground = "23", const QByteArray &members = "23:1234 24:1235")
{
    return fields({start, foreground, "/tmp/work", "/bin/sh", members, {}, {}});
}
}

class HostProcessProtocolTest : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

private Q_SLOTS:
    void resolvedCommandBoundaries();
    void completeObservation();
    void incompleteArgumentsPreserveCleanupIdentity();
    void rejectInvalidIdentity_data();
    void rejectInvalidIdentity();
    void sshArgumentsAreBounded();
};

// Command replies decode executable and shell fields and reject missing fields, invalid flags, truncation and excessive output.
void HostProcessProtocolTest::resolvedCommandBoundaries()
{
    const auto resolved = decodeCommand(fields({"./user command", "/tmp/user command", "0"}));
    QVERIFY(resolved);
    QCOMPARE(resolved->program, QStringLiteral("./user command"));
    QCOMPARE(resolved->executable, QStringLiteral("/tmp/user command"));
    QVERIFY(!resolved->shell);
    const auto shell = decodeCommand(fields({"/bin/sh", "/usr/bin/dash", "1"}));
    QVERIFY(shell);
    QVERIFY(shell->shell);
    QVERIFY(!decodeCommand(fields({"", "/bin/sh", "1"})));
    QVERIFY(!decodeCommand(fields({"/bin/sh", "", "1"})));
    QVERIFY(!decodeCommand(fields({"/bin/sh", "/bin/sh", "true"})));
    QVERIFY(!decodeCommand(fields({"/bin/sh", "/bin/sh", "1", "extra"})));
    QByteArray missingTerminator = fields({"/bin/sh", "/bin/sh", "1"});
    missingTerminator.chop(1);
    QVERIFY(!decodeCommand(missingTerminator));
    QVERIFY(!decodeCommand(QByteArray(Kwi::ProcessQueryLimits::MaximumOutputBytes + 1, 'x')));
}

// A complete host observation preserves process identities, directory, executable, arguments and cleanup members.
void HostProcessProtocolTest::completeObservation()
{
    const auto observed = decodeObservation(metadata() + fields({"sh", "-i", ""}), 23, 1234);
    QVERIFY(observed);
    QCOMPARE(observed->leader.pid, uint(23));
    QCOMPARE(observed->leader.startTime, quint64(1234));
    QCOMPARE(observed->foreground, uint(23));
    QCOMPARE(observed->directory, QStringLiteral("/tmp/work"));
    QCOMPARE(observed->foregroundExecutable, QStringLiteral("/bin/sh"));
    QCOMPARE(observed->arguments, QStringList({QStringLiteral("sh"), QStringLiteral("-i"), QString()}));
    QCOMPARE(observed->members, (QList<ProcessIdentity>{{23, 1234}, {24, 1235}}));
    QCOMPARE(encodeMembers(observed->members), QStringLiteral("23:1234 24:1235"));
}

// Missing or oversized command lines preserve cleanup identities while valid arguments remain readable.
void HostProcessProtocolTest::incompleteArgumentsPreserveCleanupIdentity()
{
    const QList<QByteArray> commandLines = {
        {},
        QByteArray("sh\0-i", 5),
        QByteArray(Kwi::ProcessQueryLimits::MaximumCommandLineBytes, 'x') + '\0',
        QByteArray("sh\0", 3) + QByteArray(Kwi::ProcessQueryLimits::MaximumCommandLineBytes - 3, '\0') + '\0',
    };

    for (const auto &commandLine : commandLines) {
        const auto observed = decodeObservation(metadata() + commandLine, 23, 1234);
        QVERIFY(observed);
        QVERIFY(observed->arguments.isEmpty());
        QCOMPARE(observed->members, (QList<ProcessIdentity>{{23, 1234}, {24, 1235}}));
    }

    QByteArray exactBoundary(Kwi::ProcessQueryLimits::MaximumCommandLineBytes - 1, 'x');
    exactBoundary += '\0';
    const auto boundary = decodeObservation(metadata() + exactBoundary, 23, 1234);
    QVERIFY(boundary);
    QCOMPARE(boundary->arguments.size(), 1);
}

void HostProcessProtocolTest::rejectInvalidIdentity_data()
{
    QTest::addColumn<QByteArray>("reply");
    QTest::addColumn<uint>("pid");
    QTest::addColumn<quint64>("expectedStart");
    const QByteArray commandLine = fields({"sh", "-i"});
    QTest::newRow("missing-pid") << metadata() + commandLine << uint(0) << quint64(0);
    QTest::newRow("overflow-pid") << metadata() + commandLine << uint(2147483648U) << quint64(0);
    QTest::newRow("pid-reused") << metadata() + commandLine << uint(23) << quint64(9999);
    QTest::newRow("missing-start") << metadata("") + commandLine << uint(23) << quint64(0);
    QTest::newRow("zero-start") << metadata("0") + commandLine << uint(23) << quint64(0);
    QTest::newRow("signed-start") << metadata("+1234") + commandLine << uint(23) << quint64(0);
    QTest::newRow("overflow-start") << metadata("18446744073709551616") + commandLine << uint(23) << quint64(0);
    QTest::newRow("negative-group") << metadata("1234", "-1") + commandLine << uint(23) << quint64(0);
    QTest::newRow("overflow-group") << metadata("1234", "2147483648") + commandLine << uint(23) << quint64(0);
    QTest::newRow("missing-member-time") << metadata("1234", "23", "24:") + commandLine << uint(23) << quint64(0);
    QTest::newRow("negative-member") << metadata("1234", "23", "-24:1235") + commandLine << uint(23) << quint64(0);
    QTest::newRow("overflow-member") << metadata("1234", "23", "2147483648:1235") + commandLine << uint(23) << quint64(0);
    QTest::newRow("extra-member-field") << metadata("1234", "23", "24:1235:1") + commandLine << uint(23) << quint64(0);
    QTest::newRow("truncated-header") << fields({"1234", "23", "/tmp"}) << uint(23) << quint64(0);
    QTest::newRow("oversized-reply") << QByteArray(Kwi::ProcessQueryLimits::MaximumOutputBytes + 1, '\0') << uint(23) << quint64(0);
}

// Host observations with invalid or reused process identities, malformed headers or excessive output are rejected.
void HostProcessProtocolTest::rejectInvalidIdentity()
{
    QFETCH(QByteArray, reply);
    QFETCH(uint, pid);
    QFETCH(quint64, expectedStart);
    QVERIFY(!decodeObservation(reply, pid, expectedStart));
}

// SSH observations preserve literal argument boundaries and discard malformed or oversized command lines.
void HostProcessProtocolTest::sshArgumentsAreBounded()
{
    const auto reply = [](const QByteArray &arguments) -> QByteArray {
        return fields({"1234", "24", "/tmp", "/usr/bin/ssh", {}, "24", "1235"}) + arguments;
    };
    const auto observed = decodeObservation(reply(fields({"ssh", "-l", "user", "host", "printf", "a b", "one\ntwo", "$() `literal` %1", ""})), 23, 1234);
    QVERIFY(observed);
    QCOMPARE(observed->ssh, (ProcessIdentity{24, 1235}));
    QCOMPARE(observed->arguments,
             QStringList({QStringLiteral("ssh"),
                          QStringLiteral("-l"),
                          QStringLiteral("user"),
                          QStringLiteral("host"),
                          QStringLiteral("printf"),
                          QStringLiteral("a b"),
                          QStringLiteral("one\ntwo"),
                          QStringLiteral("$() `literal` %1"),
                          QString()}));
    const QList<QByteArray> invalidArguments{QByteArray("ssh"),
                                             QByteArray(Kwi::ProcessQueryLimits::MaximumCommandLineBytes, 'x') + '\0',
                                             QByteArray("\0ssh\0", 5)};

    for (const auto &arguments : invalidArguments) {
        const auto incomplete = decodeObservation(reply(arguments), 23, 1234);
        QVERIFY(incomplete);
        QVERIFY(incomplete->arguments.isEmpty());
    }
}

QTEST_GUILESS_MAIN(HostProcessProtocolTest)
#include "hostprocessprotocoltest.moc"
