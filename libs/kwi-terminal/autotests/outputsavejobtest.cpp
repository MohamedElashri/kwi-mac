// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "OutputSaveJob.h"

#include <Terminal.h>

#include <KJob>
#include <QByteArray>
#include <QFile>
#include <QIODevice>
#include <QObject>
#include <QSignalSpy>
#include <QString>
#include <QStringLiteral>
#include <QTemporaryDir>
#include <QTest>

namespace
{
QByteArray readFile(const QString &path)
{
    QFile file{path};

    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}
}

class OutputSaveJobTest : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

private Q_SLOTS:
    void savesOutput_data();
    void savesOutput();
    void preservesDestinationOnFailure();
    void cancellation();
    void ioErrors();
};

void OutputSaveJobTest::savesOutput_data()
{
    QTest::addColumn<QByteArray>("input");
    QTest::addColumn<QByteArray>("expected");
    QTest::newRow("empty") << QByteArray{} << QByteArray{};
    QTest::newRow("unicode-and-color") << QStringLiteral("\x1b[31m界🙂한글é\x1b[0m\r\nlast").toUtf8() << QStringLiteral("界🙂한글é\nlast").toUtf8();
    QTest::newRow("wrapped") << QByteArray(200, 'x') << QByteArray(200, 'x');
    QTest::newRow("large") << (QByteArrayLiteral("retained output\r\n").repeated(50000) + "last")
                           << (QByteArrayLiteral("retained output\n").repeated(50000) + "last");
}

// Successful output export replaces existing file contents with the complete retained text.
void OutputSaveJobTest::savesOutput()
{
    QFETCH(QByteArray, input);
    QFETCH(QByteArray, expected);
    GhosttyQt::Terminal terminal{80, 24, GhosttyQt::Terminal::UnlimitedScrollback};
    QVERIFY(terminal.feed(input));
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath(QStringLiteral("output.txt"));
    QFile existing{path};
    QVERIFY(existing.open(QIODevice::WriteOnly));
    QCOMPARE(existing.write("previous contents"), 17);
    existing.close();

    Kwi::OutputSaveJob job{&terminal, path};
    job.setAutoDelete(false);
    QSignalSpy result{&job, &KJob::result};
    job.start();
    QCOMPARE(readFile(path), QByteArrayLiteral("previous contents"));
    QVERIFY(result.size() == 1 || result.wait());
    QCOMPARE(result.size(), 1);
    QCOMPARE(job.error(), 0);
    QVERIFY(existing.open(QIODevice::ReadOnly));
    QCOMPARE(existing.readAll(), expected);
}

// Invalidating the history snapshot makes the save fail and preserves the existing file.
void OutputSaveJobTest::preservesDestinationOnFailure()
{
    GhosttyQt::Terminal terminal;
    QVERIFY(terminal.feed("output"));
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("output.txt"));
    QFile existing{path};
    QVERIFY(existing.open(QIODevice::WriteOnly));
    QCOMPARE(existing.write("previous contents"), 17);
    existing.close();
    Kwi::OutputSaveJob job{&terminal, path};
    job.setAutoDelete(false);
    terminal.reset();
    QSignalSpy result{&job, &KJob::result};
    job.start();
    QTRY_COMPARE(result.size(), 1);
    QVERIFY(job.error() != 0);
    QVERIFY(!job.errorString().isEmpty());
    QCOMPARE(readFile(path), QByteArrayLiteral("previous contents"));
}

// Cancelling an output save destroys the job and leaves the existing file unchanged.
void OutputSaveJobTest::cancellation()
{
    GhosttyQt::Terminal terminal;
    QVERIFY(terminal.feed("output"));
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("output.txt"));
    QFile existing{path};
    QVERIFY(existing.open(QIODevice::WriteOnly));
    QCOMPARE(existing.write("previous contents"), 17);
    existing.close();
    auto *job = new Kwi::OutputSaveJob{&terminal, path};
    QSignalSpy result{job, &KJob::result};
    QSignalSpy destroyed{job, &QObject::destroyed};
    job->start();
    QVERIFY(job->kill());
    QTRY_COMPARE(destroyed.size(), 1);
    QCOMPARE(result.size(), 0);
    QCOMPARE(readFile(path), QByteArrayLiteral("previous contents"));
}

// Output saves report an error when the destination is a directory.
void OutputSaveJobTest::ioErrors()
{
    GhosttyQt::Terminal terminal;
    QVERIFY(terminal.feed("output"));
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Kwi::OutputSaveJob job{&terminal, directory.path()};
    job.setAutoDelete(false);
    QSignalSpy result{&job, &KJob::result};
    job.start();
    QVERIFY(result.size() == 1 || result.wait());
    QCOMPARE(result.size(), 1);
    QVERIFY(job.error() != 0);
    QVERIFY(!job.errorString().isEmpty());
}

QTEST_GUILESS_MAIN(OutputSaveJobTest)
#include "outputsavejobtest.moc"
