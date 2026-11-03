// SPDX-FileCopyrightText: 2026 Kwi contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "process/SshMetadataResolver_p.h"
#include <QFile>
#include <QHostAddress>
#include <QProcess>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTest>
#include <libproc.h>

class SshMetadataTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void argumentBoundaries()
    {
        const auto query = Kwi::sshConfigurationQuery({QStringLiteral("ssh"),
                                                       QStringLiteral("-vp2202"),
                                                       QStringLiteral("-E"),
                                                       QStringLiteral("must-not-write.log"),
                                                       QStringLiteral("-l"),
                                                       QStringLiteral("alice"),
                                                       QStringLiteral("--"),
                                                       QStringLiteral("host"),
                                                       QStringLiteral("printf"),
                                                       QStringLiteral("literal ; $HOME")});
        QVERIFY(query);
        QVERIFY(query->arguments.contains(QStringLiteral("-G")));
        QVERIFY(query->arguments.contains(QStringLiteral("2202")));
        QVERIFY(!query->arguments.contains(QStringLiteral("-E")));
        QVERIFY(!query->arguments.contains(QStringLiteral("must-not-write.log")));
        QCOMPARE(query->remoteCommand, QStringLiteral("printf literal ; $HOME"));
        QVERIFY(!Kwi::sshConfigurationQuery({QStringLiteral("ssh"), QStringLiteral("-G"), QStringLiteral("host")}));
        QVERIFY(!Kwi::sshConfigurationQuery({QStringLiteral("ssh"), QStringLiteral("-p")}));
        QVERIFY(!Kwi::sshMetadata("hostname host\nuser alice\nport 0\n", {}));
    }

    void actualOpenSshConfigurationAndStaleIdentity()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QTcpServer endpoint;
        QVERIFY(endpoint.listen(QHostAddress::LocalHost));
        QFile config{directory.filePath(QStringLiteral("ssh config-é"))};
        QVERIFY(config.open(QIODevice::WriteOnly));
        config.write("Host test-host\n HostName 127.0.0.1\n User alice\n Port ");
        config.write(QByteArray::number(endpoint.serverPort()));
        config.write("\n CanonicalizeHostname no\n BatchMode yes\n ConnectTimeout 10\n ControlMaster no\n");
        config.close();
        const QStringList arguments{QStringLiteral("-F"),
                                    config.fileName(),
                                    QStringLiteral("test-host"),
                                    QStringLiteral("printf"),
                                    QStringLiteral("literal ; $HOME")};
        QProcess client;
        client.start(QStringLiteral("/usr/bin/ssh"), arguments);
        QVERIFY(client.waitForStarted(3000));
        const auto cleanup = qScopeGuard([&] {
            client.kill();
            client.waitForFinished(3000);
        });
        QTRY_VERIFY_WITH_TIMEOUT(endpoint.hasPendingConnections(), 3000);
        proc_bsdinfo process{};
        QCOMPARE(proc_pidinfo(int(client.processId()), PROC_PIDTBSDINFO, 0, &process, sizeof(process)), int(sizeof(process)));
        Kwi::SshProcessInfo identity{client.processId(),
                                     process.pbi_start_tvsec * 1000000 + process.pbi_start_tvusec,
                                     QStringLiteral("/usr/bin/ssh"),
                                     directory.path(),
                                     QStringList{QStringLiteral("/usr/bin/ssh")} + arguments};
        Kwi::SshMetadataResolver resolver;
        resolver.setProcess(identity);
        QTRY_VERIFY_WITH_TIMEOUT(resolver.available(), 5000);
        QCOMPARE(resolver.info().hostName, QStringLiteral("127.0.0.1"));
        QCOMPARE(resolver.info().userName, QStringLiteral("alice"));
        QCOMPARE(resolver.info().port, int(endpoint.serverPort()));
        QCOMPARE(resolver.info().remoteCommand, QStringLiteral("printf literal ; $HOME"));

        auto stale = identity;
        ++stale.startTime;
        resolver.setProcess(stale);
        QVERIFY(!resolver.available());
        QTest::qWait(50);
        QVERIFY(!resolver.available());

        client.kill();
        QVERIFY(client.waitForFinished(3000));
        resolver.setProcess(identity);
        QTest::qWait(50);
        QVERIFY(!resolver.available());
    }
};

QTEST_GUILESS_MAIN(SshMetadataTest)
#include "SshMetadataTest.moc"
