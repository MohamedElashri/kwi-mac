// SPDX-FileCopyrightText: 2026 Kwi contributors
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "LocalActivation.h"
#include "MacLaunch_p.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

class LocalActivationTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void launchDirectoryPolicy()
    {
        QCOMPARE(Kwi::macLaunchDirectory(QStringLiteral("/"), true), QDir::homePath());
        QCOMPARE(Kwi::macLaunchDirectory(QStringLiteral("/"), false), QStringLiteral("/"));
        QCOMPARE(Kwi::macLaunchDirectory(QStringLiteral("/tmp/space é"), true), QStringLiteral("/tmp/space é"));
    }

    void forwardArgumentsAndStatus()
    {
        QTemporaryDir directory{QStringLiteral("/tmp/kwi-activation-XXXXXX")};
        QVERIFY(directory.isValid());
        const auto endpoint = directory.filePath(QStringLiteral("socket"));
        Kwi::LocalActivation primary{endpoint};
        QCOMPARE(primary.start({QStringLiteral("kwi")}, directory.path()), -1);
        const QStringList arguments{QStringLiteral("kwi"), QStringLiteral("--new-tab"), QStringLiteral("space argument"), QString{}, QString::fromUtf8("é")};
        QStringList received;
        QString cwd;
        primary.activate = [&](const QStringList &forwarded, const QString &path) {
            received = forwarded;
            cwd = path;
            return 2;
        };
        QProcess child;
        child.setWorkingDirectory(directory.path());
        // QProcess uses filesystem encoding for macOS argv (including Unicode
        // decomposition). Transport the fixture as ASCII to test IPC fidelity.
        const auto fixture = QJsonDocument{QJsonArray::fromStringList(arguments)}.toJson().toBase64();
        child.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--activation-client"), endpoint, QString::fromLatin1(fixture)});
        QTRY_COMPARE_WITH_TIMEOUT(child.state(), QProcess::NotRunning, 8000);
        QCOMPARE(child.exitStatus(), QProcess::NormalExit);
        QCOMPARE(child.exitCode(), 2);
        QCOMPARE(received, arguments);
        QCOMPARE(QFileInfo(cwd).canonicalFilePath(), QFileInfo(directory.path()).canonicalFilePath());
    }

    void invalidRequestIsNotLaunched()
    {
        QTemporaryDir directory{QStringLiteral("/tmp/kwi-activation-XXXXXX")};
        const auto endpoint = directory.filePath(QStringLiteral("socket"));
        Kwi::LocalActivation primary{endpoint};
        QCOMPARE(primary.start({QStringLiteral("kwi")}, directory.path()), -1);
        bool called{false};
        primary.activate = [&](const QStringList &, const QString &) {
            called = true;
            return 0;
        };
        QLocalSocket client;
        client.connectToServer(endpoint);
        QVERIFY(client.waitForConnected(1000));
        client.write("{\"arguments\":[4],\"directory\":\"/tmp\"}\n");
        QTRY_VERIFY_WITH_TIMEOUT(client.canReadLine(), 5000);
        QCOMPARE(client.readLine(), QByteArray("2\n"));
        QVERIFY(!called);
    }

    void simultaneousFirstLaunch()
    {
        QTemporaryDir directory;
        const auto endpoint = directory.filePath(QStringLiteral("socket"));
        const QStringList request{QStringLiteral("kwi"), QStringLiteral("--new-tab"), QString{}, QStringLiteral("literal ; $HOME é")};
        const auto encoded = QString::fromLatin1(QJsonDocument{QJsonArray::fromStringList(request)}.toJson().toBase64());
        QProcess first, second;
        first.start(QCoreApplication::applicationFilePath(),
                    {QStringLiteral("--activation-race"), endpoint, directory.filePath(QStringLiteral("first.json")), encoded});
        second.start(QCoreApplication::applicationFilePath(),
                     {QStringLiteral("--activation-race"), endpoint, directory.filePath(QStringLiteral("second.json")), encoded});
        QTRY_COMPARE_WITH_TIMEOUT(first.state(), QProcess::NotRunning, 8000);
        QTRY_COMPARE_WITH_TIMEOUT(second.state(), QProcess::NotRunning, 8000);
        QCOMPARE(first.exitCode(), 0);
        QCOMPARE(second.exitCode(), 0);
        int owners{0}, requests{0};
        for (const auto &name : {QStringLiteral("first.json"), QStringLiteral("second.json")}) {
            QFile file{directory.filePath(name)};
            QVERIFY(file.open(QIODevice::ReadOnly));
            const auto result = QJsonDocument::fromJson(file.readAll()).object();
            if (result.value(QStringLiteral("owner")).toBool()) {
                ++owners;
                requests += result.value(QStringLiteral("requests")).toInt();
                QCOMPARE(result.value(QStringLiteral("arguments")).toArray(), QJsonArray::fromStringList(request));
            }
        }
        QCOMPARE(owners, 1);
        QCOMPARE(requests, 1);
    }

    void recoverCrashedOwner()
    {
        QTemporaryDir directory;
        const auto endpoint = directory.filePath(QStringLiteral("socket"));
        const auto ready = directory.filePath(QStringLiteral("ready"));
        QProcess owner;
        owner.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--activation-owner"), endpoint, ready});
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(ready), 3000);
        owner.kill();
        QVERIFY(owner.waitForFinished(3000));
        QVERIFY(QFile::exists(endpoint + QStringLiteral(".lock")));
        Kwi::LocalActivation replacement{endpoint};
        QCOMPARE(replacement.start({QStringLiteral("kwi")}, directory.path()), -1);
    }
};

int main(int argc, char **argv)
{
    QCoreApplication app{argc, argv};
    const auto arguments = app.arguments();
    if (arguments.value(1) == QLatin1String("--activation-race") || arguments.value(1) == QLatin1String("--activation-owner")) {
        Kwi::LocalActivation service{arguments.value(2)};
        QStringList request{QStringLiteral("kwi")};
        if (arguments.size() > 4) {
            request.clear();
            for (const auto &value : QJsonDocument::fromJson(QByteArray::fromBase64(arguments.value(4).toLatin1())).array())
                request.append(value.toString());
        }
        const int status = service.start(request, QDir::currentPath());
        const bool owner = status == -1;
        if (arguments.value(1) == QLatin1String("--activation-owner")) {
            QFile ready{arguments.value(3)};
            if (!owner || !ready.open(QIODevice::WriteOnly | QIODevice::NewOnly))
                return 1;
            ready.write("ready");
            ready.close();
            QTimer::singleShot(8000, &app, &QCoreApplication::quit);
            return app.exec();
        }
        int requests{0};
        QJsonArray received;
        if (owner) {
            service.activate = [&](const QStringList &values, const QString &) {
                ++requests;
                received = QJsonArray::fromStringList(values);
                QTimer::singleShot(100, &app, &QCoreApplication::quit);
                return 0;
            };
            QTimer::singleShot(5000, &app, &QCoreApplication::quit);
            app.exec();
        }
        QFile output{arguments.value(3)};
        if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly))
            return 1;
        output.write(
            QJsonDocument{QJsonObject{{QStringLiteral("owner"), owner}, {QStringLiteral("requests"), requests}, {QStringLiteral("arguments"), received}}}
                .toJson());
        return owner ? 0 : status;
    }
    if (arguments.value(1) == QLatin1String("--activation-client")) {
        Kwi::LocalActivation client{arguments.value(2)};
        QStringList request;
        for (const auto &value : QJsonDocument::fromJson(QByteArray::fromBase64(arguments.value(3).toLatin1())).array()) {
            request.append(value.toString());
        }
        return client.start(request, QDir::currentPath());
    }
    LocalActivationTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "LocalActivationTest.moc"
