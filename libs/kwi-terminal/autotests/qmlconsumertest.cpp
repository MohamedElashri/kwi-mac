// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: GPL-3.0-or-later
#include <QByteArray>
#include <QChar>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QGuiApplication>
#include <QIODevice>
#include <QList>
#include <QMetaEnum>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlError>
#include <QQmlProperty>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QString>
#include <QStringList>
#include <QStringLiteral>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>
#include <QVariant>
#include <QVariantMap>
#include <algorithm>
#include <iterator>
#include <memory>

namespace
{
constexpr auto Module = "org.kde.kwi.terminal";

QByteArray readFile(const QString &path)
{
    QFile file{path};

    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

QObject *objectProperty(QObject *object, const char *name)
{
    return object->property(name).value<QObject *>();
}

QString history(QObject *session)
{
    QString text;

    if (!QMetaObject::invokeMethod(session, "historyText", Q_RETURN_ARG(QString, text))) {
        qFatal("The documented Session.historyText() method is unavailable");
    }

    return text;
}

bool invokeText(QObject *object, const char *method, const QString &text)
{
    return QMetaObject::invokeMethod(object, method, Q_ARG(QString, text));
}

int stateValue(QObject *session, const char *name)
{
    const auto *meta = session->metaObject();
    const int index{meta->indexOfEnumerator("State")};

    return index >= 0 ? meta->enumerator(index).keyToValue(name) : -1;
}

}

class ConsumerTest : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

private Q_SLOTS:
    void init();
    void cleanup();

    void declarativeLaunchAndBindings();

    void invalidLaunchConfiguration_data();
    void invalidLaunchConfiguration();
    void deferredPasteUsesOriginSession();

private:
    std::unique_ptr<QObject> session(const QVariantMap &properties = {});
    std::unique_ptr<QQuickItem> terminal(QObject *session, QQuickWindow &window);
    bool start(QObject *session);

    std::unique_ptr<QQmlEngine> m_engine;
    std::unique_ptr<QTemporaryDir> m_directory;
    QList<QQmlError> m_warnings;
};

void ConsumerTest::init()
{
    const QString temporaryRoot{QDir{QDir::tempPath()}.canonicalPath()};
    QVERIFY(!temporaryRoot.isEmpty());
    m_directory = std::make_unique<QTemporaryDir>(temporaryRoot + QStringLiteral("/qmlconsumer-XXXXXX"));
    QVERIFY(m_directory->isValid());
    m_engine = std::make_unique<QQmlEngine>();
    connect(m_engine.get(), &QQmlEngine::warnings, this, [this](const QList<QQmlError> &warnings) {
        m_warnings.append(warnings);
    });
}

void ConsumerTest::cleanup()
{
    m_engine.reset();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    QStringList warnings;
    warnings.reserve(m_warnings.size());
    std::ranges::transform(m_warnings, std::back_inserter(warnings), &QQmlError::toString);
    m_warnings.clear();
    QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join(u'\n')));
    m_directory.reset();
}

std::unique_ptr<QObject> ConsumerTest::session(const QVariantMap &properties)
{
    QQmlComponent component{m_engine.get()};
    component.loadFromModule(QString::fromLatin1(Module), QStringLiteral("Session"));

    if (!component.isReady()) {
        qWarning().noquote() << component.errorString();

        return {};
    }

    auto object = std::unique_ptr<QObject>{component.createWithInitialProperties(properties)};

    if (!object) {
        qWarning().noquote() << component.errorString();
    }

    return object;
}

std::unique_ptr<QQuickItem> ConsumerTest::terminal(QObject *sessionObject, QQuickWindow &window)
{
    QQmlComponent component{m_engine.get()};
    component.loadFromModule(QString::fromLatin1(Module), QStringLiteral("Terminal"));

    if (!component.isReady()) {
        qWarning().noquote() << component.errorString();

        return {};
    }

    std::unique_ptr<QObject> object{component.create()};
    auto *item = qobject_cast<QQuickItem *>(object.get());

    if (!item || !QQmlProperty::write(item, QStringLiteral("session"), QVariant::fromValue(sessionObject))) {
        qWarning().noquote() << component.errorString() << "Cannot configure Terminal.session";

        return {};
    }

    item->setParentItem(window.contentItem());
    item->setSize(window.size());

    return std::unique_ptr<QQuickItem>{static_cast<QQuickItem *>(object.release())};
}

bool ConsumerTest::start(QObject *sessionObject)
{
    bool accepted{false};

    if (!QMetaObject::invokeMethod(sessionObject, "start", Q_RETURN_ARG(bool, accepted)) || !accepted) {
        return false;
    }

    const bool running = QTest::qWaitFor(
        [sessionObject] {
            return sessionObject->property("state").toInt() == stateValue(sessionObject, "Running");
        },
        5000);

    if (!running) {
        qWarning() << sessionObject->property("errorString");

        return false;
    }

    return true;
}

// A session declared in QML launches with its configured environment and directory and exits after receiving input.
void ConsumerTest::declarativeLaunchAndBindings()
{
    QQmlComponent component{m_engine.get()};
    component.setData(R"QML(
import QtQuick
import org.kde.kwi.terminal
Item {
    id: host
    width: 640
    height: 400
    required property string testDirectory
    required property string testPython
    property Session process: Session {
        executable: host.testPython
        arguments: ["-u", "-c", "import os; print(os.getenv('CONSUMER_VALUE')); print('PROGRAM='+str(os.getenv('TERM_PROGRAM'))); input()"]
        initialDirectory: host.testDirectory
        environmentChanges: ({CONSUMER_VALUE: "declarative 水", TERM_PROGRAM: null})
    }
    Terminal { anchors.fill: parent; session: host.process }
    Component.onCompleted: process.start()
    function finishLaunch(): void { process.sendInput("finish\n"); }
}
)QML",
                      QUrl{});
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    std::unique_ptr<QObject> host{component.createWithInitialProperties(
        {{QStringLiteral("testDirectory"), m_directory->path()}, {QStringLiteral("testPython"), QStringLiteral(KWI_TEST_PYTHON)}})};
    QVERIFY2(host, qPrintable(component.errorString()));
    auto *process = objectProperty(host.get(), "process");
    QVERIFY(process);
    QTRY_COMPARE(process->property("state").toInt(), stateValue(process, "Running"));
    QVERIFY(process->property("active").toBool());
    QVERIFY(process->property("processId").toLongLong() > 0);
    QTRY_VERIFY(history(process).contains(QStringLiteral("declarative 水")));
    QTRY_VERIFY(history(process).contains(QStringLiteral("PROGRAM=None")));
    QTRY_COMPARE(process->property("workingDirectory").toString(), m_directory->path());
    QVERIFY(QMetaObject::invokeMethod(host.get(), "finishLaunch"));
    QTRY_COMPARE(process->property("state").toInt(), stateValue(process, "Exited"));
    QVERIFY(!process->property("active").toBool());
    QCOMPARE(process->property("exitCode").toInt(), 0);
}

void ConsumerTest::invalidLaunchConfiguration_data()
{
    QTest::addColumn<QVariantMap>("properties");
    QTest::newRow("non-string-environment-value") << QVariantMap{{QStringLiteral("environmentChanges"), QVariantMap{{QStringLiteral("VALUE"), 42}}}};
    QTest::newRow("argument-with-null") << QVariantMap{
        {QStringLiteral("arguments"), QStringList{QStringLiteral("before") + QChar::Null + QStringLiteral("after")}}};
}

// The public QML API rejects malformed launch values before starting a process.
void ConsumerTest::invalidLaunchConfiguration()
{
    QFETCH(QVariantMap, properties);
    auto process = session(properties);
    QVERIFY(process);
    bool accepted{true};
    QVERIFY(QMetaObject::invokeMethod(process.get(), "start", Q_RETURN_ARG(bool, accepted)));
    QVERIFY(!accepted);
    QCOMPARE(process->property("state").toInt(), stateValue(process.get(), "NotStarted"));
    QCOMPARE(process->property("processId").toLongLong(), 0);
    QVERIFY(!process->property("errorString").toString().isEmpty());
}

// Deferred paste approval delivers sanitized, bracketed input to the original session after the view switches sessions.
void ConsumerTest::deferredPasteUsesOriginSession()
{
    const QString capturePath = m_directory->filePath(QStringLiteral("input"));
    const QString script = QStringLiteral(
        "import os,sys,tty\n"
        "tty.setraw(0)\n"
        "capture=open(sys.argv[1],'wb',buffering=0)\n"
        "os.write(1,b'\\x1b[?2004hREADY\\r\\n')\n"
        "while True:\n"
        " data=os.read(0,4096)\n"
        " if not data: break\n"
        " capture.write(data)\n");
    auto process = session({{QStringLiteral("executable"), QStringLiteral(KWI_TEST_PYTHON)},
                            {QStringLiteral("arguments"), QStringList{QStringLiteral("-u"), QStringLiteral("-c"), script, capturePath}}});
    QVERIFY(process);
    QQuickWindow window;
    window.resize(640, 400);
    auto control = terminal(process.get(), window);
    QVERIFY(control);
    auto *view = qobject_cast<QQuickItem *>(objectProperty(control.get(), "view"));
    QVERIFY(view);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(start(process.get()));
    QTRY_VERIFY(history(process.get()).contains(QStringLiteral("READY")));

    const QString paste = QStringLiteral("one\r\ntwo\x1b[201~\x03\x7f\t") + QChar(0x85);
    QSignalSpy requested{view, SIGNAL(multilinePasteRequested(Kwi::Session *, QString))};
    QVERIFY(requested.isValid());
    QVERIFY(invokeText(view, "requestPaste", paste));
    QCOMPARE(requested.count(), 1);
    QPointer<QObject> origin = requested.first().at(0).value<QObject *>();
    QCOMPARE(origin.data(), process.get());
    QCOMPARE(requested.first().at(1).toString(), paste);
    QVERIFY(invokeText(process.get(), "sendInput", QStringLiteral("before-approval")));
    QTRY_COMPARE(readFile(capturePath), QByteArrayLiteral("before-approval"));

    auto other = session();
    QVERIFY(other);
    QVERIFY(QQmlProperty::write(control.get(), QStringLiteral("session"), QVariant::fromValue(other.get())));
    QVERIFY(invokeText(origin.data(), "pasteApprovedText", requested.first().at(1).toString()));
    QTRY_COMPARE(readFile(capturePath), QByteArrayLiteral("before-approval\x1b[200~one\rtwo[201~\t\x1b[201~"));
    QVERIFY(history(other.get()).isEmpty());
    process.reset();
    QVERIFY(origin.isNull());
}

int main(int argc, char **argv)
{
    QTemporaryDir config;
    QTemporaryDir data;

    if (!config.isValid() || !data.isValid()) {
        return 2;
    }

    qputenv("XDG_CONFIG_HOME", config.path().toUtf8());
    qputenv("XDG_DATA_HOME", data.path().toUtf8());
    qputenv("SHELL", "/bin/sh");

    QGuiApplication application{argc, argv};
    application.setApplicationName(QStringLiteral("consumer-host"));
    application.setOrganizationDomain(QStringLiteral("example.org"));
    application.setDesktopFileName(QStringLiteral("org.example.consumer-host"));
    ConsumerTest test;

    return QTest::qExec(&test, argc, argv);
}

#include "qmlconsumertest.moc"
