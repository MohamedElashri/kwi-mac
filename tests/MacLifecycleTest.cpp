// SPDX-FileCopyrightText: 2026 Kwi contributors
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "MacGuiTestSupport.h"
#include "OutputSaveJob.h"
#include "SessionModel.h"
#include "TerminalSession.h"
#include "TerminalView.h"
#include "WindowManager.h"
#include "settings/TerminalPaletteModel.h"
#include "settings/TerminalPresets.h"
#include "settings/TerminalSettings.h"
#include "ui/AppShortcuts.h"
#include <BreezeIcons>
#include <KIconTheme>
#include <KirigamiAppDefaults>
#include <QApplication>
#include <QClipboard>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QFile>
#include <QFontDatabase>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QScopeGuard>
#include <QSet>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <algorithm>
#include <mach/mach.h>
#include <memory>
#include <qpa/qwindowsysteminterface.h>

namespace
{
template<typename Predicate>
bool awaitGui(Predicate ready, int timeout)
{
    if (ready())
        return true;
    // Run a real Cocoa event loop. Repeated QTest::qWait/processEvents polling
    // changes socket/render scheduling and badly distorts sustained PTY tests.
    QEventLoop loop;
    QTimer observer;
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&observer, &QTimer::timeout, &loop, [&] {
        if (ready())
            loop.quit();
    });
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    observer.start(1);
    deadline.start(timeout);
    loop.exec();
    return ready();
}

qint64 footprint()
{
    task_vm_info_data_t info{};
    mach_msg_type_number_t count{TASK_VM_INFO_COUNT};
    return task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS ? qint64(info.phys_footprint) : -1;
}
}

class MacLifecycleTest : public QObject
{
    Q_OBJECT
    std::unique_ptr<Kwi::TerminalSettings> m_settings;
    std::unique_ptr<Kwi::TerminalPresets> m_presets;
    std::unique_ptr<Kwi::AppShortcuts> m_shortcuts;
    std::unique_ptr<Kwi::TerminalPaletteModel> m_palettes;

    bool inputEcho(QQuickWindow *window, Kwi::TerminalView *view, Kwi::TerminalSession *session, const QString &condition)
    {
        if (view->session() != session)
            return false;
        if (!activateMacTestWindow(window, 5000))
            return false;
        view->forceActiveFocus();
        if (!awaitGui(
                [&] {
                    return view->hasActiveFocus();
                },
                2000))
            return false;
        int frameCount{0};
        QObject frameObserver;
        // frameSwapped is emitted by the render thread. Observe it on the GUI
        // thread so reads and destruction cannot race a direct signal delivery.
        QObject::connect(
            window,
            &QQuickWindow::frameSwapped,
            &frameObserver,
            [&frameCount] {
                ++frameCount;
            },
            Qt::QueuedConnection);
        QJsonArray samples;
        int focusReacquisitions{0};
        for (int n{0}; n < 30; ++n) {
            if (!window->isActive() || !view->hasActiveFocus()) {
                ++focusReacquisitions;
                if (!activateMacTestWindow(window, 5000))
                    return false;
                view->forceActiveFocus();
                if (!awaitGui(
                        [&] {
                            return view->hasActiveFocus();
                        },
                        2000))
                    return false;
            }
            const auto before = session->historyText().size();
            const auto count = frameCount;
            QElapsedTimer timer;
            timer.start();
            QTest::keyClick(window, Qt::Key_A);
            if (!awaitGui(
                    [&] {
                        return session->historyText().size() > before && frameCount > count;
                    },
                    2000)) {
                qWarning() << "Input proxy failed" << condition << "sample" << n << "active window" << window->isActive() << "active terminal"
                           << view->hasActiveFocus() << "history before/after" << before << session->historyText().size() << "frames before/after" << count
                           << frameCount;
                return false;
            }
            samples.append(timer.nsecsElapsed() / 1e6);
        }
        const auto beforeClear = session->historyText().size();
        QTest::keyClick(window, Qt::Key_U, Qt::MetaModifier);
        if (!awaitGui(
                [&] {
                    return session->historyText().size() < beforeClear;
                },
                2000)) {
            qWarning() << "Input proxy Ctrl-U failed" << condition << "active window" << window->isActive() << "active terminal" << view->hasActiveFocus()
                       << "history before/after" << beforeClear << session->historyText().size();
            return false;
        }
        qInfo().noquote() << "INPUT_ECHO_TIMING"
                          << QJsonDocument{QJsonObject{
                                               {QStringLiteral("condition"), condition},
                                               {QStringLiteral("focus_reacquisitions"), focusReacquisitions},
                                               {QStringLiteral("scope"),
                                                QStringLiteral("Qt event to observed PTY echo plus frame callback opportunity; not pixel presentation")},
                                               {QStringLiteral("samples_ms"), samples}}}
                                 .toJson(QJsonDocument::Compact);
        return true;
    }

private Q_SLOTS:
    void init()
    {
        activateMacTestApplication();
    }

    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setApplicationName(QStringLiteral("zirq-macos-lifecycle-test"));
        QApplication::setQuitOnLastWindowClosed(false);
        BreezeIcons::initIcons();
        KirigamiAppDefaults::apply(qApp);
        m_settings = std::make_unique<Kwi::TerminalSettings>();
        m_settings->setScrollbackLines(100000);
        m_presets = std::make_unique<Kwi::TerminalPresets>(m_settings.get());
        m_shortcuts = std::make_unique<Kwi::AppShortcuts>();
        m_palettes = std::make_unique<Kwi::TerminalPaletteModel>(m_settings.get());
        QVERIFY(!QIcon::fromTheme(QStringLiteral("application-menu")).isNull());
    }

    void configuredFont()
    {
        Kwi::TerminalSettings settings;
        settings.resetAppearanceDefaults();
        QVERIFY(!settings.useSystemFont());
        QCOMPARE(settings.fontPointSize(), 14.0);
        QString preferred;
        const auto system = QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
        for (const auto &family : QFontDatabase::families()) {
            if (family != system && QFontDatabase::isFixedPitch(family)) {
                preferred = family;
                break;
            }
        }
        QVERIFY(!preferred.isEmpty());
        settings.setFontFamily(preferred);
        settings.setFontPointSize(10);
        QCOMPARE(settings.fontFamily(), preferred);
        QVERIFY(settings.fixedWidthFonts().contains(preferred));
        const auto fonts = settings.fixedWidthFonts();
        QVERIFY(fonts.contains(system));
        QVERIFY(!fonts.contains(QStringLiteral("Helvetica")));
        const QSet<QString> uniqueFonts{fonts.cbegin(), fonts.cend()};
        QCOMPARE(uniqueFonts.size(), fonts.size());
        for (const auto &family : fonts)
            QVERIFY2(QFontDatabase::isFixedPitch(family), qPrintable(family));
        settings.setFontFamily(QStringLiteral("Kwi nonexistent fixed face"));
        QCOMPARE(settings.fontFamily(), preferred);
        settings.setUseSystemFont(true);
        QCOMPARE(settings.fontFamily(), system);
        settings.setUseSystemFont(false);
        QCOMPARE(settings.fontFamily(), preferred);
        QCOMPARE(settings.fontPointSize(), 10.0); // Explicit size survives system-font toggles.
    }

    void interactionAndCloseChecks()
    {
        Kwi::WindowManager windows{m_settings.get(), m_presets.get(), m_shortcuts.get(), m_palettes.get()};
        QTemporaryDir directory;
        Kwi::LaunchOptions options;
        options.launch.command = QStringList{QStringLiteral("/bin/sh"), QStringLiteral("-i")};
        options.launch.directory = directory.path();
        auto *window = windows.launch(options);
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window, 10000));
        int frameCount{0};
        QObject frameObserver;
        QObject::connect(
            window,
            &QQuickWindow::frameSwapped,
            &frameObserver,
            [&frameCount] {
                ++frameCount;
            },
            Qt::QueuedConnection);
        QTRY_VERIFY_WITH_TIMEOUT(frameCount > 0, 10000);
        QCOMPARE(window->rendererInterface()->graphicsApi(), QSGRendererInterface::Metal);
        auto *model = windows.sessions(window);
        QVERIFY(model);
        auto *session = model->currentSession();
        QVERIFY(session);
        auto *view = window->findChild<Kwi::TerminalView *>();
        QVERIFY(view);
        view->setCursorBlinking(false);
        QVERIFY(session->sendInput(QStringLiteral("printf 'KWI_UI_%s\\n' READY\n")));
        QTRY_VERIFY_WITH_TIMEOUT(session->historyText().contains(QLatin1String("KWI_UI_READY")), 5000);
        QVERIFY(inputEcho(window, view, session, QStringLiteral("idle")));
        const QString ascii{QStringLiteral(KWI_TEST_WORKLOADS "/ascii-64MiB.bin")};
        QVERIFY(QFile::exists(ascii));
        Kwi::LaunchOptions background;
        background.newTab = true;
        background.hold = true;
        background.launch.command = QStringList{QStringLiteral("/bin/cat"), ascii};
        QVERIFY(windows.launch(background));
        auto *producer = model->currentSession();
        model->setCurrentIndex(0);
        QTRY_VERIFY_WITH_TIMEOUT(view->session() == session, 5000);
        QVERIFY(inputEcho(window, view, session, QStringLiteral("another tab producing 64MiB ASCII")));
        QVERIFY(awaitGui(
            [&] {
                return producer->isFinished();
            },
            30000));
        model->closeSession(1);
        QTRY_COMPARE_WITH_TIMEOUT(model->count(), 1, 5000);
        auto original = std::make_unique<QMimeData>();
        if (const auto *clipboard = QGuiApplication::clipboard()->mimeData()) {
            for (const auto &format : clipboard->formats())
                original->setData(format, clipboard->data(format));
        }
        const auto restore = qScopeGuard([&] {
            QGuiApplication::clipboard()->setMimeData(original.release());
        });
        view->selectAll();
        view->copy();
        QVERIFY(QGuiApplication::clipboard()->text().contains(QLatin1String("KWI_UI_READY")));
        view->find(QStringLiteral("KWI_UI_READY"));
        QTRY_VERIFY_WITH_TIMEOUT(!view->isSearchBusy(), 5000);
        QVERIFY(!view->searchStatus().isEmpty());
        view->clearSearch();
        QVERIFY(session->sendInput(QStringLiteral("sleep 60\n")));
        const auto busy = [&] {
            session->refreshProcessInfo();
            return session->isBusy();
        };
        QTRY_VERIFY_WITH_TIMEOUT(busy() && session->foregroundCommandName() == QLatin1String("sleep"), 5000);
        QSignalSpy confirmation{model, &Kwi::SessionModel::closeConfirmationRequested};
        model->closeSession(0);
        QTRY_COMPARE_WITH_TIMEOUT(confirmation.count(), 1, 5000);
        model->cancelClose(confirmation.first().first().toULongLong());
        view->forceActiveFocus();
        QTest::keyClick(window, Qt::Key_C, Qt::MetaModifier); // Physical Control on macOS.
        QTRY_VERIFY_WITH_TIMEOUT(!busy(), 5000);
        model->closeAll();
        QTRY_COMPARE_WITH_TIMEOUT(windows.count(), 0, 5000);
    }

    void settingsSheetCanClose()
    {
        Kwi::WindowManager windows{m_settings.get(), m_presets.get(), m_shortcuts.get(), m_palettes.get()};
        Kwi::LaunchOptions options;
        options.launch.command = QStringList{QStringLiteral("/bin/echo"), QStringLiteral("KWI_SETTINGS")};
        options.hold = true;
        auto *window = windows.launch(options);
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window, 10000));
        window->requestActivate();
        QVERIFY(activateMacTestWindow(window, 10000));
        auto *settings = window->findChild<QObject *>(QStringLiteral("settingsView"));
        QVERIFY(settings);
        QTest::keyClick(window, Qt::Key_Comma, Qt::ControlModifier); // Command-comma.
        QTRY_VERIFY_WITH_TIMEOUT(settings->property("configViewItem").value<QObject *>(), 5000);
        auto *dialog = qobject_cast<QQuickWindow *>(settings->property("configViewItem").value<QObject *>());
        QVERIFY(dialog);
        QVERIFY(dialog->findChild<QObject *>(QStringLiteral("closeSettingsButton")));
        QVERIFY(activateMacTestWindow(dialog, 5000));
        QTest::keyClick(dialog, Qt::Key_W, Qt::ControlModifier);
        QTRY_VERIFY_WITH_TIMEOUT(!settings->property("configViewItem").value<QObject *>(), 5000);
        QCOMPARE(windows.sessions(window)->count(), 1);
        windows.sessions(window)->closeAll(true);
        QTRY_COMPARE_WITH_TIMEOUT(windows.count(), 0, 5000);
    }

    void repeatedTabs()
    {
        Kwi::WindowManager windows{m_settings.get(), m_presets.get(), m_shortcuts.get(), m_palettes.get()};
        Kwi::LaunchOptions options;
        options.launch.command = QStringList{QStringLiteral("/bin/sh"), QStringLiteral("-i")};
        auto *window = windows.launch(options);
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window, 10000));
        auto *model = windows.sessions(window);
        auto *view = window->findChild<Kwi::TerminalView *>();
        QVERIFY(view);
        view->setCursorBlinking(false);
        QJsonArray samples;
        samples.append(footprint());
        QElapsedTimer elapsed;
        elapsed.start();
        options.newTab = true;
        options.hold = true;
        options.launch.command = QStringList{QStringLiteral("/bin/sh"), QStringLiteral("-c"), QStringLiteral("printf KWI_CYCLE_DONE")};
        for (int n{0}; n < 200; ++n) {
            QVERIFY(windows.launch(options));
            QCOMPARE(model->count(), 2);
            auto *session = model->currentSession();
            QVERIFY(awaitGui(
                [&] {
                    return session->isFinished();
                },
                5000));
            QVERIFY(session->historyText().contains(QLatin1String("KWI_CYCLE_DONE")));
            model->closeSession(model->currentIndex());
            QTRY_COMPARE_WITH_TIMEOUT(model->count(), 1, 5000);
            if ((n + 1) % 20 == 0) {
                QTest::qWait(100);
                samples.append(footprint());
            }
        }
        qInfo().noquote() << "SLICE_METRICS"
                          << QJsonDocument{QJsonObject{{QStringLiteral("cycles"), 200},
                                                       {QStringLiteral("elapsed_ms"), elapsed.elapsed()},
                                                       {QStringLiteral("footprint_every_20_cycles"), samples}}}
                                 .toJson(QJsonDocument::Compact);
        QTest::qWait(2000);
        qInfo() << "SLICE_SETTLED_FOOTPRINT" << footprint();
        model->closeAll(true);
        QTRY_COMPARE_WITH_TIMEOUT(windows.count(), 0, 5000);
    }

    void lastWindowAndReopen()
    {
        Kwi::WindowManager windows{m_settings.get(), m_presets.get(), m_shortcuts.get(), m_palettes.get()};
        Kwi::LaunchOptions options;
        options.launch.command = QStringList{QStringLiteral("/bin/echo"), QStringLiteral("KWI_REOPEN")};
        options.hold = true;
        auto *window = windows.launch(options);
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window, 10000));
        auto *model = windows.sessions(window);
        QTRY_VERIFY_WITH_TIMEOUT(model->currentSession()->isFinished(), 5000);
        model->closeAll(true);
        QTRY_COMPARE_WITH_TIMEOUT(windows.count(), 0, 5000);
        QTest::qWait(1000);
        QCOMPARE(windows.count(), 0);
        // The same forced state event that Qt's Cocoa Dock-reopen delegate sends.
        QWindowSystemInterface::handleApplicationStateChanged(Qt::ApplicationActive, true);
        QTRY_COMPARE_WITH_TIMEOUT(windows.count(), 1, 5000);
    }

    void historySearchAndExport()
    {
        const QString fixture{QStringLiteral(KWI_TEST_WORKLOADS "/history-100000.txt")};
        QVERIFY2(QFile::exists(fixture), "Run scripts/macos/workloads.py before GUI workload checks");
        Kwi::WindowManager windows{m_settings.get(), m_presets.get(), m_shortcuts.get(), m_palettes.get()};
        Kwi::LaunchOptions options;
        options.launch.command = QStringList{QStringLiteral("/bin/cat"), fixture};
        options.hold = true;
        QElapsedTimer elapsed;
        elapsed.start();
        auto *window = windows.launch(options);
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window, 10000));
        auto *session = windows.sessions(window)->currentSession();
        QVERIFY(awaitGui(
            [&] {
                return session->isFinished();
            },
            30000));
        const auto feedMs = elapsed.elapsed();
        QVERIFY(session->historyText().contains(QLatin1String("KWI_HISTORY_099999")));
        auto *view = window->findChild<Kwi::TerminalView *>();
        QVERIFY(view);
        elapsed.restart();
        view->find(QStringLiteral("KWI_HISTORY_099999"));
        QVERIFY(awaitGui(
            [&] {
                return !view->isSearchBusy();
            },
            10000));
        QVERIFY(view->searchStatus().contains(QLatin1Char('1')));
        const auto searchMs = elapsed.elapsed();
        QTemporaryDir directory;
        const QString output{directory.filePath(QString::fromUtf8("export-é.txt"))};
        bool completed{false};
        int error{-1};
        auto *job = new Kwi::OutputSaveJob{session->terminal(), output, this};
        connect(job, &KJob::result, this, [&](KJob *finished) {
            error = finished->error();
            completed = true;
        });
        elapsed.restart();
        job->start();
        QVERIFY(awaitGui(
            [&] {
                return completed;
            },
            10000));
        QCOMPARE(error, 0);
        const auto exportMs = elapsed.elapsed();
        QFile file{output};
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto bytes = file.readAll();
        QVERIFY(bytes.contains("KWI_HISTORY_000000"));
        QVERIFY(bytes.contains("KWI_HISTORY_099999"));
        QFile input{fixture};
        QVERIFY(input.open(QIODevice::ReadOnly));
        QCOMPARE(bytes + '\n', input.readAll()); // Export omits the final empty line.
        qInfo().noquote() << "SLICE_HISTORY_METRICS"
                          << QJsonDocument{QJsonObject{{QStringLiteral("feed_finish_ms"), feedMs},
                                                       {QStringLiteral("search_ms"), searchMs},
                                                       {QStringLiteral("export_ms"), exportMs},
                                                       {QStringLiteral("export_bytes"), bytes.size()}}}
                                 .toJson(QJsonDocument::Compact);
        windows.sessions(window)->closeAll(true);
        QTRY_COMPARE_WITH_TIMEOUT(windows.count(), 0, 5000);
    }
};

int main(int argc, char **argv)
{
    recordMacTestProcess();
    QStandardPaths::setTestModeEnabled(true);
    KIconTheme::initTheme();
    BreezeIcons::initIcons();
    QQuickWindow::setDefaultAlphaBuffer(true);
    QApplication app{argc, argv};
    const bool tracing{qEnvironmentVariableIntValue("ZIRQ_TRACE") == 1};
    qunsetenv("ZIRQ_TRACE");
    app.setProperty("zirqTrace", tracing);
    MacLifecycleTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "MacLifecycleTest.moc"
