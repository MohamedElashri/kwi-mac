// SPDX-FileCopyrightText: 2026 Kwi contributors
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "MacGuiTestSupport.h"
#include "SessionModel.h"
#include "TerminalSession.h"
#include "TerminalView.h"
#include "WindowManager.h"
#include "settings/TerminalPaletteModel.h"
#include "settings/TerminalPresets.h"
#include "settings/TerminalSettings.h"
#include "ui/AppShortcuts.h"
#include <ActionCollection/ActionCollection>
#include <ActionCollection/ActionCollections>
#include <BreezeIcons>
#include <KIconTheme>
#include <KLocalizedString>
#include <KirigamiAppDefaults>
#include <QAbstractItemModel>
#include <QAccessible>
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QInputMethodEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QSGRendererInterface>
#include <QScopeGuard>
#include <QSet>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QStyleHints>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QWheelEvent>
#include <functional>

class MacInteractionTest : public QObject
{
    Q_OBJECT
    QTemporaryDir m_directory;
    std::unique_ptr<Kwi::TerminalSettings> m_settings;
    std::unique_ptr<Kwi::TerminalPresets> m_presets;
    std::unique_ptr<Kwi::AppShortcuts> m_shortcuts;
    std::unique_ptr<Kwi::TerminalPaletteModel> m_palettes;
    std::unique_ptr<Kwi::WindowManager> m_windows;
    QQuickWindow *m_window{};
    Kwi::TerminalSession *m_session{};
    Kwi::TerminalView *m_view{};
    QJsonArray m_optionTimings;

    static QObject *optionMatching(QObject *root, const std::function<bool(QObject *)> &matches)
    {
        QList<QObject *> pending{root};
        QSet<QObject *> visited;
        while (!pending.isEmpty()) {
            auto *item = pending.takeLast();
            if (!item || visited.contains(item))
                continue;
            visited.insert(item);
            if (matches(item))
                return item;
            pending.append(item->children());
            if (auto *visual = qobject_cast<QQuickItem *>(item))
                for (auto *child : visual->childItems())
                    pending.append(child);
            if (auto *contents = item->property("contentItem").value<QObject *>())
                pending.append(contents);
        }
        return nullptr;
    }

    static QObject *optionObject(QObject *root, const QString &name, bool prefix = false)
    {
        return optionMatching(root, [&](QObject *item) {
            return prefix ? item->objectName().startsWith(name) : item->objectName() == name;
        });
    }

    bool measureOption(const QString &name, const std::function<bool()> &operation, const std::function<QQuickWindow *()> &surface = {})
    {
        QElapsedTimer elapsed;
        elapsed.start();
        QElapsedTimer heartbeat;
        heartbeat.start();
        qint64 longestGap{0};
        QTimer observer;
        observer.setInterval(1);
        connect(&observer, &QTimer::timeout, &observer, [&] {
            longestGap = std::max(longestGap, heartbeat.nsecsElapsed());
            heartbeat.restart();
        });
        observer.start();
        bool ok = operation();
        const auto dispatch = elapsed.nsecsElapsed();
        QCoreApplication::processEvents();
        auto *window = surface ? surface() : m_window;
        if (window && window->isVisible())
            ok = !window->grabWindow().isNull() && ok;
        QTest::qWait(12);
        longestGap = std::max(longestGap, heartbeat.nsecsElapsed());
        observer.stop();
        const QJsonObject result{{QStringLiteral("option"), name},
                                 {QStringLiteral("dispatch_ms"), dispatch / 1e6},
                                 {QStringLiteral("rendered_ms"), elapsed.nsecsElapsed() / 1e6},
                                 {QStringLiteral("max_gui_gap_ms"), longestGap / 1e6},
                                 {QStringLiteral("ok"), ok}};
        m_optionTimings.append(result);
        QFile file{QDir{qEnvironmentVariable("KWI_PERF_OUTPUT")}.filePath(QString::fromLatin1(QTest::currentTestFunction()) + u'-'
                                                                          + QString::fromLatin1(QTest::currentDataTag()) + QStringLiteral("-options.json"))};
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return false;
        file.write(
            QJsonDocument{QJsonObject{{QStringLiteral("scope"),
                                       QStringLiteral("Isolated GUI action dispatch, synchronous capture and 12 ms event drain; not pixel presentation")},
                                      {QStringLiteral("measurements"), m_optionTimings}}}
                .toJson());
        qInfo().noquote() << "OPTION_TIMING" << QJsonDocument{result}.toJson(QJsonDocument::Compact);
        return ok;
    }

    QByteArray received() const
    {
        QFile file{m_directory.filePath(QStringLiteral("received.bin"))};
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
    }

    bool launch()
    {
        QFile script{m_directory.filePath(QStringLiteral("input.py"))};
        if (!script.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return false;
        script.write(
            "import os,sys,tty\ntty.setraw(0)\nf=open(sys.argv[1],'wb',buffering=0)\n"
            "os.write(1,b'KWI_INPUT_READY\\r\\n')\n"
            "while True:\n b=os.read(0,4096)\n if not b: break\n f.write(b)\n os.write(1,b'RX:'+b.hex().encode()+b'\\r\\n')\n");
        script.close();
        m_windows = std::make_unique<Kwi::WindowManager>(m_settings.get(), m_presets.get(), m_shortcuts.get(), m_palettes.get());
        Kwi::LaunchOptions options;
        options.launch.command = QStringList{QStringLiteral("/usr/bin/python3"), script.fileName(), m_directory.filePath(QStringLiteral("received.bin"))};
        m_window = m_windows->launch(options);
        if (!m_window || !activateMacTestWindow(m_window))
            return false;
        m_session = m_windows->sessions(m_window)->currentSession();
        m_view = m_window->findChild<Kwi::TerminalView *>();
        if (!m_view)
            return false;
        m_view->forceActiveFocus();
        return true;
    }

    void nativeKey(unsigned short code, const QString &text, const QString &base, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
    {
        postMacTestKey(m_window, code, modifiers, text, base);
        postMacTestKey(m_window, code, modifiers, text, base, true);
    }

    void screenshot(const QString &name)
    {
        QTest::qWait(150);
        const QImage image{m_window->grabWindow()};
        QVERIFY(!image.isNull());
        QCOMPARE(m_window->rendererInterface()->graphicsApi(), QSGRendererInterface::Metal);
        const QString path = qEnvironmentVariable("KWI_PERF_OUTPUT") + QLatin1Char('/') + name + QStringLiteral(".png");
        QVERIFY(image.save(path));
    }

private Q_SLOTS:
    void auditedUrl(const QUrl &)
    {
    }

    void initTestCase()
    {
        QVERIFY(m_directory.isValid());
        QApplication::setQuitOnLastWindowClosed(false);
        KirigamiAppDefaults::apply(qApp);
        m_settings = std::make_unique<Kwi::TerminalSettings>();
        m_presets = std::make_unique<Kwi::TerminalPresets>(m_settings.get());
        m_shortcuts = std::make_unique<Kwi::AppShortcuts>();
        m_palettes = std::make_unique<Kwi::TerminalPaletteModel>(m_settings.get());
    }

    void init()
    {
        m_optionTimings = {};
        m_settings->setUseSystemFont(false);
        m_settings->setFontFamily(QStringLiteral("Menlo"));
        m_settings->setFontPointSize(10);
        m_settings->setCursorBlinking(false);
        m_settings->setOptionAsAlt(false);
        m_settings->setBidirectionalText(true);
        QVERIFY(launch());
        QTRY_VERIFY_WITH_TIMEOUT(m_session->historyText().contains(QLatin1String("KWI_INPUT_READY")), 5000);
        QTRY_VERIFY(m_view->hasActiveFocus());
    }

    void cleanup()
    {
        if (m_windows) {
            const auto windows = QGuiApplication::allWindows();
            for (auto *window : windows)
                if (auto *model = m_windows->sessions(qobject_cast<QQuickWindow *>(window)))
                    model->closeAll(true);
            QTRY_COMPARE_WITH_TIMEOUT(m_windows->count(), 0, 5000);
            m_windows.reset();
        }
        m_settings->resetBehaviorDefaults();
        m_settings->resetAppearanceDefaults();
        m_settings->save();
        if (!macTestVoiceOverEnabled())
            QAccessible::setActive(false);
    }

    void cocoaOptionControlAndCommands()
    {
        nativeKey(9, QString::fromUtf8("√"), QStringLiteral("v"), Qt::AltModifier);
        QTRY_COMPARE(received(), QString::fromUtf8("√").toUtf8());
        m_settings->setOptionAsAlt(true);
        QTRY_VERIFY(m_view->optionAsAlt());
        QVERIFY(m_settings->save());
        // The empty Cocoa characters field is an Option-E dead key. Alt mode
        // must send ESC e immediately, without starting native composition.
        nativeKey(14, {}, QStringLiteral("e"), Qt::AltModifier);
        QTRY_COMPARE(received(), QString::fromUtf8("√").toUtf8() + QByteArray("\x1b") + "e");
        nativeKey(14, QStringLiteral("e"), QStringLiteral("e"));
        QTRY_VERIFY(received().endsWith(QByteArray("\033ee")));
        nativeKey(8, QString(QChar(3)), QStringLiteral("c"), Qt::MetaModifier);
        QTRY_VERIFY(received().endsWith(QByteArray(1, '\x03')));
        const auto before = received();
        nativeKey(8, QStringLiteral("c"), QStringLiteral("c"), Qt::ControlModifier);
        QTest::qWait(80);
        QCOMPARE(received(), before); // Command-C is not terminal Ctrl-C.
        auto *model = m_windows->sessions(m_window);
        nativeKey(17, QStringLiteral("t"), QStringLiteral("t"), Qt::ControlModifier);
        QTRY_COMPARE(model->count(), 2);
        screenshot(QStringLiteral("desktop-tabs-top"));
        m_settings->setTabBarAtBottom(true);
        QTest::qWait(150);
        screenshot(QStringLiteral("desktop-tabs-bottom"));
        m_settings->setTabBarAtBottom(false);
        QTest::qWait(150);
        nativeKey(13, QStringLiteral("w"), QStringLiteral("w"), Qt::ControlModifier);
        QTRY_COMPARE(model->count(), 1);
    }

    void bidirectionalText()
    {
        m_settings->setBidirectionalText(true);
        m_settings->setFontPointSize(14);
        m_window->resize(1000, 650);
        m_session->setCustomTitle(QStringLiteral("Bidirectional text"));
        m_session->feed(QStringLiteral("\033[2J\033[Habc שלום 123\r\n"
                                       "Arabic:  السلام عليكم — السَّلَامُ عَلَيْكُمْ\r\n"
                                       "Persian: فارسی می‌نویسم؛ یک کتاب ۱۲۳\r\n"
                                       "Hebrew:  שָׁלוֹם עוֹלָם (123)\r\n"
                                       "Mixed:   build مرحبا 123 (test) שלום ✓\r\n"
                                       "Isolate: \u2067العربية 123 (abc)\u2069 end\r\n"
                                       "Styles:  \033[31mسلا\033[32mم بالعالم\033[0m\r\n"
                                       "Wide:    مرحبا 😀 中文 שלום\r\n"
                                       "\033[1;1H")
                            .toUtf8());
        QTRY_VERIFY(m_session->historyText().contains(QStringLiteral("فارسی")));
        QTRY_VERIFY(m_view->bidirectionalText());
        QTest::qWait(150);
        const auto first = m_view->inputMethodQuery(Qt::ImCursorRectangle).toRectF();
        const qreal cw = first.width();
        m_session->feed("\033[1;5H"); // Logical first Hebrew letter appears at visual column 11.
        QTRY_VERIFY(qAbs(m_view->inputMethodQuery(Qt::ImCursorRectangle).toRectF().x() - first.x() - 11 * cw) < 1);
        QAccessible::setActive(true);
        auto *accessible = QAccessible::queryAccessibleInterface(m_view);
        QVERIFY(accessible && accessible->textInterface());
        auto *text = accessible->textInterface();
        QVERIFY(text->text(0, -1).startsWith(QStringLiteral("abc שלום 123")));
        const auto hebrewRect = text->characterRect(4);
        const auto digitRect = text->characterRect(9);
        QVERIFY(hebrewRect.left() > digitRect.left());
        QCOMPARE(text->offsetAtPoint(hebrewRect.center()), 4);
        // Mouse endpoints are visual; the copied result remains in logical order.
        const auto cell = [&](qreal x, int y = 0) {
            return m_view->mapToScene(QPointF(first.x() + x * cw, first.y() + (y + 0.5) * first.height())).toPoint();
        };
        QTest::mousePress(m_window, Qt::LeftButton, Qt::NoModifier, cell(11.9));
        QTest::mouseMove(m_window, cell(8.0), 60);
        QTest::mouseRelease(m_window, Qt::LeftButton, Qt::NoModifier, cell(8.0));
        QTRY_COMPARE(m_view->selectedText(), QStringLiteral("שלום"));
        m_view->copy();
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("שלום"));
        m_view->setKeyboardSelectionMode(true);
        QTRY_VERIFY(qAbs(m_view->selectionCursorRectangle().x() - first.x() - 11 * cw) < 1);
        QTest::keyClick(m_window, Qt::Key_Left);
        QTRY_VERIFY(qAbs(m_view->selectionCursorRectangle().x() - first.x() - 10 * cw) < 1);
        QTest::keyClick(m_window, Qt::Key_Left, Qt::ShiftModifier);
        QTRY_COMPARE(m_view->selectedText(), QStringLiteral("לו"));
        m_view->setKeyboardSelectionMode(false);
        QTest::mousePress(m_window, Qt::LeftButton, Qt::ControlModifier | Qt::AltModifier, cell(6.1));
        QTest::mouseMove(m_window, cell(9.9), 60);
        QTest::mouseRelease(m_window, Qt::LeftButton, Qt::ControlModifier | Qt::AltModifier, cell(9.9));
        QTRY_COMPARE(m_view->selectedText(), QStringLiteral("ום 3"));
        m_view->find(QStringLiteral("שלום"));
        QTRY_VERIFY(!m_view->isSearchBusy());
        QVERIFY(!m_view->searchStatus().isEmpty());
        m_view->clearSearch();
        m_settings->setBidirectionalText(false);
        QTRY_VERIFY(!m_view->bidirectionalText());
        QTRY_VERIFY(qAbs(m_view->inputMethodQuery(Qt::ImCursorRectangle).toRectF().x() - first.x() - 4 * cw) < 1);
        m_settings->setBidirectionalText(true);
        QTRY_VERIFY(m_view->bidirectionalText());
        m_session->feed("\033[10;1Hcompose> ");
        setMacTestPreedit(m_window, QStringLiteral("سلام"));
        QTest::qWait(150);
        QVERIFY(received().isEmpty());
        screenshot(QStringLiteral("bidirectional-text"));
        commitMacTestText(m_window, QStringLiteral("سلام فارسی שלום"));
        QTRY_COMPARE(received(), QStringLiteral("سلام فارسی שלום").toUtf8());
        m_settings->setBidirectionalText(true);
    }

    void tabHistoryRetainsLogicalRtlSelection()
    {
        QTest::failOnWarning(QRegularExpression(QStringLiteral(".*TerminalTabButton.*")));
        m_settings->setConfirmCloseRunning(false);
        auto *model = m_windows->sessions(m_window);
        const QString selected = QStringLiteral("selection سلام שלום");
        m_session->feed((QStringLiteral("\033[2J\033[H") + selected).toUtf8());
        QTest::qWait(80);
        m_view->selectAll();
        QTRY_COMPARE(m_view->selectedText(), selected);
        Kwi::LaunchRequest request;
        request.command = QStringList{QStringLiteral("/bin/sleep"), QStringLiteral("60")};
        request.contextDirectory = m_directory.path();
        QVERIFY(model->launchSession(request));
        QPointer<Kwi::TerminalSession> second = model->currentSession();
        QVERIFY(model->launchSession(request));
        QPointer<Kwi::TerminalSession> third = model->currentSession();
        model->setCurrentIndex(0);
        QTRY_COMPARE(m_view->session(), m_session);
        QTRY_COMPARE(m_view->selectedText(), selected);
        QVERIFY(model->launchSession(request));
        QPointer<Kwi::TerminalSession> inserted = model->currentSession();
        QCOMPARE(model->indexOf(inserted), 1);
        QCOMPARE(model->indexOf(second), 2);
        QCOMPARE(model->indexOf(third), 3);
        model->setCurrentIndex(model->indexOf(m_session));
        QTRY_COMPARE(m_view->selectedText(), selected);
        model->setCurrentIndex(model->indexOf(inserted));
        model->setCurrentIndex(model->indexOf(third));
        model->closeSession(model->indexOf(third));
        QTRY_COMPARE(model->count(), 3);
        QTRY_COMPARE(model->currentSession(), inserted.data());
        model->setCurrentIndex(model->indexOf(m_session));
        QTRY_COMPARE(m_view->selectedText(), selected);
        m_view->find(QStringLiteral("سلام"));
        QTRY_VERIFY(!m_view->isSearchBusy());
        auto *bar = m_window->findChild<QQuickItem *>(QStringLiteral("terminalSearchBar"));
        QVERIFY(bar);
        m_window->setProperty("findVisible", true);
        QTRY_VERIFY(bar->isVisible());
        QTRY_VERIFY(bar->mapToScene(QPointF(0, bar->height())).y() <= m_view->mapToScene(QPointF()).y() + 1);
    }

    void readmeScreenshot()
    {
        const auto originalScheme = QGuiApplication::styleHints()->colorScheme();
        const auto restoreAppearance = qScopeGuard([&] {
            QGuiApplication::styleHints()->setColorScheme(originalScheme);
        });
        QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Dark);
        m_settings->setFontPointSize(14);
        m_settings->setColorScheme(QStringLiteral("Breeze"));
        m_settings->setBackgroundOpacity(1);
        m_settings->setBackgroundBlur(false);
        m_settings->setTabBarAtBottom(false);
        m_window->resize(940, 470);
        m_session->setCustomTitle(QStringLiteral("Overview"));
        auto *model = m_windows->sessions(m_window);
        for (const auto &title : {QStringLiteral("Build"), QStringLiteral("Notes")}) {
            Kwi::LaunchRequest request;
            request.command = QStringList{QStringLiteral("/bin/sleep"), QStringLiteral("60")};
            request.contextDirectory = m_directory.path();
            QVERIFY(model->launchSession(request, {}, false, title));
        }
        model->setCurrentIndex(0);
        QTest::qWait(300);
        // Feed only controlled demo text and capture this window's rendered surface.
        // No shell profile, desktop pixels or real user/host/path data is included.
        m_session->feed(
            "\033[2J\033[H\r\n"
            "  \033[1;38;2;124;191;255mKwi for macOS\033[0m\r\n"
            "  A terminal workspace for Apple Silicon.\r\n\r\n"
            "  \033[38;2;132;210;158m>\033[0m make help\r\n\r\n"
            "    \033[38;2;124;191;255mbuild\033[0m       Compile the application\r\n"
            "    \033[38;2;124;191;255mtest\033[0m        Run tooling and native tests\r\n"
            "    \033[38;2;124;191;255mrun\033[0m         Open Kwi\r\n"
            "    \033[38;2;124;191;255mpackage\033[0m     Create a verified application bundle\r\n"
            "    \033[38;2;124;191;255mci\033[0m          Build, test and package\r\n\r\n"
            "  \033[38;2;132;210;158m>\033[0m ./colors\r\n\r\n"
            "    \033[48;2;231;113;130m    \033[48;2;237;179;112m    \033[48;2;235;212;128m    "
            "\033[48;2;132;210;158m    \033[48;2;117;199;204m    \033[48;2;124;165;234m    "
            "\033[48;2;180;151;231m    \033[0m\r\n\r\n");
        m_session->feed(QStringLiteral("  Unicode: café  λ →  日本語  ┌─┬─┐\r\n\r\n").toUtf8());
        m_session->feed("  \033[38;2;132;210;158m>\033[0m ");
        QTRY_VERIFY(m_session->historyText().contains(QLatin1String("A terminal workspace")));
        QTest::qWait(400);
        screenshot(QStringLiteral("readme"));
    }

    void commandFindSettingsZoomAndFullscreen()
    {
        m_session->feed("marker one\r\nmarker two\r\n");
        nativeKey(3, QStringLiteral("f"), QStringLiteral("f"), Qt::ControlModifier);
        QTRY_VERIFY(m_window->property("findVisible").toBool());
        auto *find = m_window->findChild<QQuickItem *>(QStringLiteral("findField"));
        QVERIFY(find);
        QTRY_VERIFY(find->hasActiveFocus());
        nativeKey(53, QString(QChar(27)), QString(QChar(27)));
        QTRY_VERIFY(!m_window->property("findVisible").toBool());
        QVERIFY(activateMacTestWindow(m_window));
        m_view->forceActiveFocus();
        QTRY_VERIFY(m_view->hasActiveFocus());
        QVERIFY(m_window->property("terminalActionsEnabled").toBool());
        QElapsedTimer configureTimer;
        configureTimer.start();
        nativeKey(43, QStringLiteral(","), QStringLiteral(","), Qt::ControlModifier);
        auto *settings = m_window->findChild<QObject *>(QStringLiteral("settingsView"));
        QVERIFY(settings);
        QTRY_VERIFY(settings->property("configViewItem").value<QObject *>());
        auto *sheet = qobject_cast<QQuickWindow *>(settings->property("configViewItem").value<QObject *>());
        const auto createdMs = configureTimer.elapsed();
        QVERIFY(sheet && activateMacTestWindow(sheet));
        QVERIFY(!sheet->grabWindow().isNull());
        qInfo().noquote() << "CONFIGURE_OPEN"
                          << QJsonDocument{QJsonObject{
                                               {QStringLiteral("created_ms"), createdMs},
                                               {QStringLiteral("captured_ms"), configureTimer.elapsed()},
                                               {QStringLiteral("scope"), QStringLiteral("Native Command-comma to sheet creation and populated capture")}}}
                                 .toJson(QJsonDocument::Compact);
        QQuickItem *fontSize{};
        QTRY_VERIFY(fontSize = sheet->findChild<QQuickItem *>(QStringLiteral("fontSizeSetting")));
        auto *editor = qobject_cast<QQuickItem *>(fontSize->property("contentItem").value<QObject *>());
        QVERIFY(editor);
        for (const auto size : {14.5, 48.0}) {
            m_settings->setFontPointSize(size);
            QTRY_COMPARE(fontSize->property("value").toInt(), qRound(size * 10));
            QTRY_VERIFY(editor->width() - editor->property("leftPadding").toReal() - editor->property("rightPadding").toReal()
                        >= editor->property("contentWidth").toReal());
            QTRY_VERIFY(editor->height() - editor->property("topPadding").toReal() - editor->property("bottomPadding").toReal()
                        >= editor->property("contentHeight").toReal());
            const auto image = sheet->grabWindow();
            QVERIFY(!image.isNull());
            QVERIFY(image.save(QDir{qEnvironmentVariable("KWI_PERF_OUTPUT")}.filePath(QStringLiteral("appearance-font-%1.png").arg(size))));
        }
        m_settings->setFontPointSize(10);
        const auto count = m_windows->sessions(m_window)->count();
        postMacTestKey(sheet, 17, Qt::ControlModifier, QStringLiteral("t"), QStringLiteral("t"));
        postMacTestKey(sheet, 17, Qt::ControlModifier, QStringLiteral("t"), QStringLiteral("t"), true);
        QTest::qWait(80);
        QCOMPARE(m_windows->sessions(m_window)->count(), count);
        postMacTestKey(sheet, 13, Qt::ControlModifier, QStringLiteral("w"), QStringLiteral("w"));
        postMacTestKey(sheet, 13, Qt::ControlModifier, QStringLiteral("w"), QStringLiteral("w"), true);
        QTRY_VERIFY(!settings->property("configViewItem").value<QObject *>());
        QVERIFY(activateMacTestWindow(m_window));
        m_view->forceActiveFocus();
        nativeKey(24, QStringLiteral("+"), QStringLiteral("+"), Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY(m_session->zoomOffset() > 0);
        nativeKey(29, QStringLiteral("0"), QStringLiteral("0"), Qt::ControlModifier);
        QTRY_COMPARE(m_session->zoomOffset(), 0);
        watchMacTestFullScreen(m_window);
        nativeKey(3, QString(QChar(6)), QStringLiteral("f"), Qt::ControlModifier | Qt::MetaModifier);
        QTRY_VERIFY_WITH_TIMEOUT(m_window->windowState() & Qt::WindowFullScreen, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(macTestFullScreenSettled(m_window, true), 5000);
        screenshot(QStringLiteral("native-fullscreen"));
        nativeKey(3, QString(QChar(6)), QStringLiteral("f"), Qt::ControlModifier | Qt::MetaModifier);
        QTRY_VERIFY_WITH_TIMEOUT(macTestFullScreenSettled(m_window, false), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!(m_window->windowState() & Qt::WindowFullScreen), 5000);
    }

    void settingsOptionsResponsiveness_data()
    {
        QTest::addColumn<QString>("module");
        for (const char *module : {"appearance", "behavior", "terminal", "presets", "keyboard"})
            QTest::newRow(module) << QString::fromLatin1(module);
    }

    void settingsOptionsResponsiveness()
    {
        QFETCH(QString, module);
        auto *settings = m_window->findChild<QObject *>(QStringLiteral("settingsView"));
        QVERIFY(settings);
        const auto sheet = [&] {
            return qobject_cast<QQuickWindow *>(settings->property("configViewItem").value<QObject *>());
        };
        const auto invoke = [](QObject *object, const char *method) {
            return object && QMetaObject::invokeMethod(object, method);
        };
        {
            QVERIFY(measureOption(
                module + QStringLiteral("/open"),
                [&] {
                    return QMetaObject::invokeMethod(settings, "open", Q_ARG(QVariant, QVariant{module}));
                },
                sheet));
            QVERIFY(sheet() && activateMacTestWindow(sheet()));
            auto *page = sheet()->findChild<QObject *>(module + QStringLiteral("Settings"));
            QVERIFY2(page, qPrintable(module));
            const auto control = [&](const QString &name) {
                return optionObject(page, name);
            };
            QStringList checkboxes;
            QStringList combos;
            QStringList spins;
            if (module == QLatin1String("appearance")) {
                checkboxes = {QStringLiteral("systemFontSetting"), QStringLiteral("backgroundBlurSetting"), QStringLiteral("showTabStatusIconsSetting")};
                combos = {QStringLiteral("fontFamilySetting"), QStringLiteral("colorSchemeSelector"), QStringLiteral("tabBarPositionSetting")};
                spins = {QStringLiteral("fontSizeSetting")};
            } else if (module == QLatin1String("behavior")) {
                checkboxes = {QStringLiteral("confirmCloseRunningSetting"),
                              QStringLiteral("confirmMultilinePasteSetting"),
                              QStringLiteral("copyOnSelectSetting"),
                              QStringLiteral("optionAsAltSetting"),
                              QStringLiteral("requireCtrlForLinksSetting")};
            } else if (module == QLatin1String("terminal")) {
                checkboxes = {QStringLiteral("windowTitleFromApplication"), QStringLiteral("cursorBlinkingSetting"), QStringLiteral("bidirectionalText")};
                combos = {QStringLiteral("scrollbackMode"), QStringLiteral("cursorStyleSetting")};
                spins = {QStringLiteral("scrollbackLines"), QStringLiteral("silenceInterval")};
                for (const QString name : {QStringLiteral("titleFormatField"), QStringLiteral("remoteTitleFormatField")}) {
                    auto *field = control(name);
                    QVERIFY(field);
                    const auto original = field->property("text");
                    QVERIFY(measureOption(
                        module + u'/' + name,
                        [&] {
                            return field->setProperty("text", QString{original.toString() + u' '}) && invoke(field, "textEdited")
                                && invoke(field, "editingFinished");
                        },
                        sheet));
                    field->setProperty("text", original);
                    QVERIFY(invoke(field, "editingFinished"));
                }
                for (const QString name : {QStringLiteral("titleFormatInsert"), QStringLiteral("remoteTitleFormatInsert")}) {
                    QVERIFY(measureOption(
                        module + u'/' + name,
                        [&] {
                            return invoke(control(name), "click");
                        },
                        sheet));
                    QVERIFY(invoke(control(QStringLiteral("titleFormatMenu")), "close"));
                }
            } else if (module == QLatin1String("presets")) {
                QVERIFY(QMetaObject::invokeMethod(page, "newCopy"));
                auto *presetName = control(QStringLiteral("presetName"));
                QVERIFY(presetName && presetName->setProperty("text", QStringLiteral("Option audit")) && invoke(presetName, "textEdited"));
                combos = {QStringLiteral("defaultPresetSelector"), QStringLiteral("presetSelector"), QStringLiteral("presetDirectoryMode")};
                for (const QString name : {QStringLiteral("presetName"), QStringLiteral("presetCommand"), QStringLiteral("presetDirectory")}) {
                    auto *field = control(name);
                    QVERIFY(field);
                    const auto original = field->property("text");
                    const auto value = name == QLatin1String("presetDirectory") ? m_directory.path() : original.toString() + u' ';
                    QVERIFY(measureOption(
                        module + u'/' + name,
                        [&] {
                            return field->setProperty("text", value) && invoke(field, "textEdited");
                        },
                        sheet));
                    field->setProperty("text", original);
                    QVERIFY(invoke(field, "textEdited"));
                }
                for (const QString name :
                     {QStringLiteral("editPresetButton"), QStringLiteral("presetActionsButton"), QStringLiteral("addEnvironmentVariable")}) {
                    QVERIFY(measureOption(
                        module + u'/' + name,
                        [&] {
                            return invoke(control(name), "click");
                        },
                        sheet));
                    for (const QString popup : {QStringLiteral("presetActionsMenu"), QStringLiteral("environmentVariableDialog")})
                        if (auto *item = control(popup))
                            QVERIFY(invoke(item, "close"));
                }
                QVERIFY(measureOption(
                    module + QStringLiteral("/save-environment"),
                    [&] {
                        if (!QMetaObject::invokeMethod(page, "editVariable", Q_ARG(int, -1)))
                            return false;
                        control(QStringLiteral("environmentVariableName"))->setProperty("text", QStringLiteral("KWI_OPTION"));
                        control(QStringLiteral("environmentVariableValue"))->setProperty("text", QStringLiteral("audit value"));
                        return invoke(control(QStringLiteral("environmentVariableDialog")), "saveVariable");
                    },
                    sheet));
                QVERIFY(measureOption(
                    module + QStringLiteral("/save"),
                    [&] {
                        return invoke(control(QStringLiteral("savePresetButton")), "click");
                    },
                    sheet));
                QVERIFY(m_presets->preset(QStringLiteral("Option audit")));
                QVERIFY(measureOption(
                    module + QStringLiteral("/remove-environment"),
                    [&] {
                        return invoke(control(QStringLiteral("removeEnvironmentVariable0")), "click");
                    },
                    sheet));
                QVERIFY(measureOption(
                    module + QStringLiteral("/discard"),
                    [&] {
                        return invoke(control(QStringLiteral("resetPresetButton")), "click");
                    },
                    sheet));
            }
            if (module == QLatin1String("keyboard")) {
                QSet<QString> names;
                auto models = sheet()->findChildren<QAbstractItemModel *>();
                if (auto *model = qobject_cast<QAbstractItemModel *>(page->property("model").value<QObject *>()))
                    models.append(model);
                for (auto *model : models) {
                    const auto roles = model->roleNames();
                    const int role = roles.key(QByteArrayLiteral("actionName"), roles.key(QByteArrayLiteral("actionDescription"), -1));
                    if (role >= 0)
                        for (int row{0}; row < model->rowCount(); ++row) {
                            const auto data = model->data(model->index(row, 0), role);
                            const auto name = data.value<QObject *>() ? data.value<QObject *>()->property("name").toString() : data.toString();
                            if (!name.isEmpty())
                                names.insert(name);
                        }
                }
                QObject *filter{};
                for (auto *item : sheet()->findChildren<QObject *>())
                    if (item->inherits("QQuickTextField") && item->property("placeholderText").toString().contains(QLatin1String("Filter"))) {
                        filter = item;
                        break;
                    }
                QVERIFY(filter && !names.isEmpty());
                for (const auto &name : names)
                    QVERIFY(measureOption(
                        module + QStringLiteral("/filter/") + name,
                        [&] {
                            return filter->setProperty("text", name);
                        },
                        sheet));
                filter->setProperty("text", QString{});
                QCoreApplication::processEvents();
                QObject *delegate{};
                QTRY_VERIFY(delegate = optionMatching(page, [](QObject *item) {
                                auto *visual = qobject_cast<QQuickItem *>(item);
                                return visual && visual->isVisible() && visual->inherits("QQuickItemDelegate") && visual->property("model").isValid();
                            }));
                QVERIFY(measureOption(
                    module + QStringLiteral("/edit-shortcut"),
                    [&] {
                        return invoke(delegate, "click");
                    },
                    sheet));
                for (auto *item : page->findChildren<QObject *>())
                    if (item->inherits("QQuickDialog") && item->property("actionDescription").isValid())
                        QVERIFY(invoke(item, "close"));
            }
            for (const auto &name : checkboxes) {
                auto *item = control(name);
                QVERIFY(item);
                const auto original = item->property("checked").toBool();
                QVERIFY(measureOption(
                    module + u'/' + name,
                    [&] {
                        return invoke(item, "click");
                    },
                    sheet));
                QCOMPARE(item->property("checked").toBool(), !original);
                QVERIFY(invoke(item, "click"));
            }
            for (const auto &name : combos) {
                auto *item = control(name);
                QVERIFY(item);
                auto *popup = item->property("popup").value<QObject *>();
                QVERIFY(measureOption(
                    module + u'/' + name + QStringLiteral("/popup"),
                    [&] {
                        return invoke(popup, "open");
                    },
                    sheet));
                QVERIFY(invoke(popup, "close"));
                const int original = item->property("currentIndex").toInt();
                const int count = item->property("count").toInt();
                if (count > 1) {
                    const int next = original == 0 ? count - 1 : 0;
                    QVERIFY(measureOption(
                        module + u'/' + name + QStringLiteral("/change"),
                        [&] {
                            return item->setProperty("currentIndex", next) && QMetaObject::invokeMethod(item, "activated", Q_ARG(int, next));
                        },
                        sheet));
                    item->setProperty("currentIndex", original);
                    QVERIFY(QMetaObject::invokeMethod(item, "activated", Q_ARG(int, original)));
                }
            }
            for (const auto &name : spins) {
                auto *item = control(name);
                QVERIFY(item);
                const int original = item->property("value").toInt();
                const int next = std::min(original + item->property("stepSize").toInt(), item->property("to").toInt());
                QVERIFY(measureOption(
                    module + u'/' + name,
                    [&] {
                        return item->setProperty("value", next) && invoke(item, "valueModified");
                    },
                    sheet));
                item->setProperty("value", original);
                QVERIFY(invoke(item, "valueModified"));
            }
            if (module == QLatin1String("appearance")) {
                auto *opacity = control(QStringLiteral("backgroundOpacitySetting"));
                QVERIFY(opacity);
                QVERIFY(measureOption(
                    module + QStringLiteral("/opacity"),
                    [&] {
                        return opacity->setProperty("value", 90) && invoke(opacity, "moved");
                    },
                    sheet));
                opacity->setProperty("value", 100);
                QVERIFY(invoke(opacity, "moved"));
                QVERIFY(measureOption(
                    module + QStringLiteral("/palette-actions"),
                    [&] {
                        return invoke(control(QStringLiteral("colorSchemeActionsButton")), "click");
                    },
                    sheet));
                QVERIFY(invoke(control(QStringLiteral("colorSchemeActionsMenu")), "close"));
                QVERIFY(measureOption(
                    module + QStringLiteral("/palette-editor"),
                    [&] {
                        return QMetaObject::invokeMethod(page, "editScheme", Q_ARG(bool, true));
                    },
                    sheet));
                auto *editor = control(QStringLiteral("colorSchemeEditor"));
                QVERIFY(editor);
                for (int n{0}; n < 18; ++n) {
                    const auto name = n < 16 ? QStringLiteral("schemeAnsiButton%1").arg(n)
                        : n == 16            ? QStringLiteral("schemeForegroundButton")
                                             : QStringLiteral("schemeBackgroundButton");
                    auto *button = optionObject(editor, name);
                    QVERIFY(button);
                    QVERIFY(measureOption(
                        module + QStringLiteral("/color-picker-%1").arg(n),
                        [&] {
                            return invoke(button, "click");
                        },
                        sheet));
                    for (auto *dialog : button->findChildren<QObject *>())
                        if (dialog->inherits("QQuickColorDialog")) {
                            QVERIFY(invoke(dialog, "close"));
                            break;
                        }
                }
                QVERIFY(invoke(editor, "close"));
            }
            QList<QPointer<QObject>> pickers;
            for (auto *dialog : page->findChildren<QObject *>())
                if (dialog->inherits("QQuickFileDialog") || dialog->inherits("QQuickFolderDialog"))
                    pickers.append(dialog);
            for (const auto &dialog : pickers)
                if (dialog) {
                    QVERIFY(measureOption(
                        module + QStringLiteral("/native-picker/") + QString::fromLatin1(dialog->metaObject()->className()),
                        [&] {
                            return invoke(dialog, "open");
                        },
                        sheet));
                    QVERIFY(invoke(dialog, "close"));
                }
            if (auto *reset = control(QStringLiteral("restoreDefaultsButton")))
                QVERIFY(measureOption(
                    module + QStringLiteral("/restore-defaults"),
                    [&] {
                        return invoke(reset, "click");
                    },
                    sheet));
            QVERIFY(measureOption(module + QStringLiteral("/close"), [&] {
                return sheet()->close();
            }));
            QTRY_VERIFY(!sheet());
            QVERIFY(activateMacTestWindow(m_window));
            QVERIFY(measureOption(
                module + QStringLiteral("/reopen"),
                [&] {
                    return QMetaObject::invokeMethod(settings, "open", Q_ARG(QVariant, QVariant{module}));
                },
                sheet));
            QVERIFY(sheet()->close());
            QTRY_VERIFY(!sheet());
            QVERIFY(activateMacTestWindow(m_window));
        }
    }

    void terminalOptionsResponsiveness()
    {
        const auto object = [&](const QString &name) {
            return m_window->findChild<QObject *>(name);
        };
        const auto call = [](QObject *item, const char *method) {
            return item && QMetaObject::invokeMethod(item, method);
        };
        const auto trigger = [&](const QString &name) {
            auto *action = object(name);
            return action && action->property("enabled").toBool() && call(action, "trigger");
        };
        const auto closePopup = [&](const QString &name) {
            auto *popup = object(name);
            return popup && call(popup, "close")
                && QTest::qWaitFor(
                       [&] {
                           return !popup->property("visible").toBool();
                       },
                       2000);
        };
        for (const QString scheme : {QStringLiteral("file"), QStringLiteral("https"), QStringLiteral("http")})
            QDesktopServices::setUrlHandler(scheme, this, "auditedUrl");
        const auto restoreHandlers = qScopeGuard([] {
            for (const QString scheme : {QStringLiteral("file"), QStringLiteral("https"), QStringLiteral("http")})
                QDesktopServices::unsetUrlHandler(scheme);
        });
        m_session->feed("\r\n\033]133;A\a$ \033]133;B\aoption-audit\r\n\033]133;C\aKWI_OPTION_WORD https://example.invalid/test\r\n\033]133;D;0\a");
        for (const QString name : {QStringLiteral("applicationMenu"), QStringLiteral("newTabPresetMenu")}) {
            QVERIFY(measureOption(name + QStringLiteral("/open"), [&] {
                return call(object(name), "open");
            }));
            QVERIFY(closePopup(name));
        }
        auto *tabMenu = object(QStringLiteral("tabContextMenu"));
        QVERIFY(tabMenu && tabMenu->setProperty("targetSession", QVariant::fromValue(m_session)));
        QVERIFY(measureOption(QStringLiteral("tab-context/open"), [&] {
            return call(tabMenu, "open");
        }));
        QVERIFY(call(tabMenu, "close"));
        for (const auto &name : {QStringLiteral("renameTabAction"), QStringLiteral("searchTabsAction"), QStringLiteral("openLinkChooserAction")}) {
            QVERIFY(measureOption(name, [&] {
                return trigger(name);
            }));
            for (const auto &popup : {QStringLiteral("renameDialog"), QStringLiteral("chooserDialog")})
                QVERIFY(closePopup(popup));
        }
        for (const auto &name : {QStringLiteral("selectionModeAction"),
                                 QStringLiteral("selectAllAction"),
                                 QStringLiteral("copyAction"),
                                 QStringLiteral("copyCommandOutputAction"),
                                 QStringLiteral("readOnlyAction"),
                                 QStringLiteral("readOnlyAction"),
                                 QStringLiteral("monitorActivityAction"),
                                 QStringLiteral("monitorActivityAction"),
                                 QStringLiteral("monitorSilenceAction"),
                                 QStringLiteral("monitorSilenceAction"),
                                 QStringLiteral("zoomInAction"),
                                 QStringLiteral("zoomOutAction"),
                                 QStringLiteral("resetZoomAction"),
                                 QStringLiteral("previousPromptAction"),
                                 QStringLiteral("nextPromptAction"),
                                 QStringLiteral("openDirectoryAction")})
            QVERIFY(measureOption(name, [&] {
                return trigger(name);
            }));
        m_view->selectAll();
        QVERIFY(measureOption(QStringLiteral("terminal-context/selected-text"), [&] {
            QTest::mouseClick(m_window, Qt::RightButton, Qt::NoModifier, m_view->mapToScene(QPointF{30, 30}).toPoint());
            return true;
        }));
        QTRY_VERIFY(object(QStringLiteral("terminalContextMenu"))->property("visible").toBool());
        QVERIFY(closePopup(QStringLiteral("terminalContextMenu")));
        for (const QString name : {QStringLiteral("aboutDialog"), QStringLiteral("aboutKdeDialog")}) {
            auto *dialog = object(name);
            const auto surface = [&] {
                return qobject_cast<QQuickWindow *>(dialog->property("window").value<QObject *>());
            };
            QVERIFY(measureOption(
                name,
                [&] {
                    return call(dialog, "open");
                },
                surface));
            QVERIFY(surface());
            if (name == QLatin1String("aboutDialog")) {
                QVERIFY(surface()->grabWindow().save(QDir{qEnvironmentVariable("KWI_PERF_OUTPUT")}.filePath(QStringLiteral("about-libraries.png"))));
                auto *page = optionObject(surface(), QStringLiteral("aboutPage"));
                auto *license = optionObject(page, QStringLiteral("libraryLicenseDialog"));
                QVERIFY(license && license->property("licenseText").toString().isEmpty());
                auto *button = optionObject(page, QStringLiteral("libraryLicenseButton_"), true);
                QVERIFY(button);
                QVERIFY(measureOption(
                    QStringLiteral("about/license-on-demand"),
                    [&] {
                        return call(button, "click");
                    },
                    surface));
                QVERIFY(!license->property("licenseText").toString().isEmpty());
                QTRY_VERIFY(license->property("visible").toBool());
                QTRY_VERIFY(license->property("opened").toBool());
                QVERIFY(license->property("width").toReal() >= 300);
                QVERIFY(license->property("height").toReal() >= 200);
                auto *licenseContent = qobject_cast<QQuickItem *>(license->property("contentItem").value<QObject *>());
                QVERIFY(licenseContent && licenseContent->window());
                QVERIFY(QTest::qWaitForWindowExposed(licenseContent->window()));
                QVERIFY(
                    licenseContent->window()->grabWindow().save(QDir{qEnvironmentVariable("KWI_PERF_OUTPUT")}.filePath(QStringLiteral("about-license.png"))));
                QVERIFY(call(license, "close"));
                QTRY_VERIFY(license->property("licenseText").toString().isEmpty());
            }
            QVERIFY(surface()->close());
            QCoreApplication::processEvents();
            QVERIFY(activateMacTestWindow(m_window));
        }
        auto *save = object(QStringLiteral("saveOutputDialog"));
        QVERIFY(measureOption(QStringLiteral("save-output/native-picker"), [&] {
            return trigger(QStringLiteral("saveOutputAction"));
        }));
        QVERIFY(call(save, "close"));
        auto *notifications = object(QStringLiteral("notificationsAction"));
        QVERIFY(notifications);
        QVERIFY(measureOption(QStringLiteral("notifications/availability"), [&] {
            return notifications->property("enabled").isValid();
        }));
        watchMacTestFullScreen(m_window);
        QVERIFY(measureOption(QStringLiteral("fullscreen/enter-with-animation"), [&] {
            return trigger(QStringLiteral("fullscreenAction"))
                && QTest::qWaitFor(
                       [&] {
                           return macTestFullScreenSettled(m_window, true);
                       },
                       12000);
        }));
        QVERIFY(measureOption(QStringLiteral("fullscreen/exit-with-animation"), [&] {
            return trigger(QStringLiteral("fullscreenAction"))
                && QTest::qWaitFor(
                       [&] {
                           return macTestFullScreenSettled(m_window, false);
                       },
                       12000);
        }));
        QGuiApplication::clipboard()->setText(QStringLiteral("KWI_PASTE_AUDIT"));
        QVERIFY(measureOption(QStringLiteral("pasteAction"), [&] {
            return trigger(QStringLiteral("pasteAction"));
        }));
        QVERIFY(measureOption(QStringLiteral("pasteSelectionAction"), [&] {
            auto *action = object(QStringLiteral("pasteSelectionAction"));
            return action && (!action->property("enabled").toBool() || trigger(QStringLiteral("pasteSelectionAction")));
        }));
        QVERIFY(measureOption(QStringLiteral("findAction"), [&] {
            return trigger(QStringLiteral("findAction"));
        }));
        auto *find = object(QStringLiteral("findField"));
        QVERIFY(find);
        QVERIFY(measureOption(QStringLiteral("find/query"), [&] {
            return find->setProperty("text", QStringLiteral("KWI_OPTION"));
        }));
        QTRY_VERIFY(!m_view->isSearchBusy());
        for (const QString name : {QStringLiteral("findNextAction"), QStringLiteral("findPreviousAction")})
            QVERIFY(measureOption(name, [&] {
                return trigger(name);
            }));
        QVERIFY(QMetaObject::invokeMethod(m_window, "hideFind"));
        m_settings->setScrollbackLines(100000);
        QByteArray block;
        for (int n{0}; n < 1000; ++n)
            block.append("Option audit retained output KWI_OPTION_WORD 0123456789 abcdefghijklmnopqrstuvwxyz\r\n");
        for (int n{0}; n < 50; ++n)
            m_session->feed(block);
        QVERIFY(measureOption(QStringLiteral("large-history/select-all-50k"), [&] {
            return trigger(QStringLiteral("selectAllAction"));
        }));
        QVERIFY(measureOption(QStringLiteral("large-history/copy-50k"), [&] {
            return trigger(QStringLiteral("copyAction"));
        }));
        QVERIFY(measureOption(QStringLiteral("large-history/context-menu-50k"), [&] {
            QTest::mouseClick(m_window, Qt::RightButton, Qt::NoModifier, m_view->mapToScene(QPointF{30, 30}).toPoint());
            return true;
        }));
        QVERIFY(closePopup(QStringLiteral("terminalContextMenu")));
        QVERIFY(measureOption(QStringLiteral("large-history/search-dispatch-50k"), [&] {
            m_view->find(QStringLiteral("KWI_OPTION_WORD"));
            return true;
        }));
        QTRY_VERIFY_WITH_TIMEOUT(!m_view->isSearchBusy(), 10000);
        QVERIFY(measureOption(QStringLiteral("large-history/clear-history-50k"), [&] {
            return trigger(QStringLiteral("clearHistoryAction"));
        }));
        QVERIFY(measureOption(QStringLiteral("resetTerminalAction"), [&] {
            return trigger(QStringLiteral("resetTerminalAction"));
        }));
        m_settings->setConfirmCloseRunning(false);
        auto *model = m_windows->sessions(m_window);
        QVERIFY(measureOption(QStringLiteral("newTabAction"), [&] {
            return trigger(QStringLiteral("newTabAction"));
        }));
        QTRY_COMPARE(model->count(), 2);
        model->setCurrentIndex(1);
        for (const QString name : {QStringLiteral("previousTabAction"),
                                   QStringLiteral("nextTabAction"),
                                   QStringLiteral("lastUsedTabAction"),
                                   QStringLiteral("moveTabLeftAction"),
                                   QStringLiteral("moveTabRightAction")}) {
            if (name == QLatin1String("moveTabLeftAction"))
                model->setCurrentIndex(1);
            else if (name == QLatin1String("moveTabRightAction"))
                model->setCurrentIndex(0);
            QVERIFY(measureOption(name, [&] {
                return trigger(name);
            }));
        }
        for (int n{2}; n < 9; ++n)
            QVERIFY(trigger(QStringLiteral("newTabAction")));
        QTRY_COMPARE(model->count(), 9);
        const unsigned short digitCodes[]{18, 19, 20, 21, 23, 22, 26, 28, 25};
        for (int n{0}; n < 9; ++n) {
            const QString digit{QString::number(n + 1)};
            QVERIFY(measureOption(QStringLiteral("switch-tab-%1").arg(n + 1), [&] {
                nativeKey(digitCodes[n], digit, digit, Qt::ControlModifier);
                return true;
            }));
            QTRY_COMPARE(model->currentIndex(), n);
        }
        while (model->count() > 2)
            model->closeSession(model->count() - 1);
        QVERIFY(measureOption(QStringLiteral("detachTabAction"), [&] {
            return trigger(QStringLiteral("detachTabAction"));
        }));
        QTRY_COMPARE(m_windows->count(), 2);
        QVERIFY(activateMacTestWindow(m_window));
        QVERIFY(measureOption(QStringLiteral("newWindowAction"), [&] {
            return trigger(QStringLiteral("newWindowAction"));
        }));
        QTRY_COMPARE(m_windows->count(), 3);
        QVERIFY(activateMacTestWindow(m_window));
        QPointer<QQuickWindow> original{m_window};
        QVERIFY(measureOption(
            QStringLiteral("closeTabAction"),
            [&] {
                return trigger(QStringLiteral("closeTabAction"));
            },
            [&] {
                return original.data();
            }));
        for (auto *window : QGuiApplication::allWindows())
            if (auto *candidate = qobject_cast<QQuickWindow *>(window); candidate && m_windows->sessions(candidate)) {
                m_window = candidate;
                break;
            }
        QPointer<QQuickWindow> quitting{m_window};
        QVERIFY(measureOption(
            QStringLiteral("quitAction"),
            [&] {
                return trigger(QStringLiteral("quitAction"));
            },
            [&] {
                return quitting.data();
            }));
        QTRY_COMPARE(m_windows->count(), 0);
    }

    void nativePreeditCommitCancelAndCursor()
    {
        // PTY history can be ready before the view's next polish. The probe's
        // ready line ends at column zero; wait for that displayed cursor first.
        QTRY_VERIFY(m_view->inputMethodQuery(Qt::ImCursorRectangle).toRectF().x() < m_view->inputMethodQuery(Qt::ImCursorRectangle).toRectF().width());
        const auto initial = m_view->inputMethodQuery(Qt::ImCursorRectangle).toRectF();
        QVERIFY(initial.width() > 0 && initial.height() > 0);
        setMacTestPreedit(m_window, QStringLiteral("日本"));
        QTest::qWait(80);
        QVERIFY(received().isEmpty());
        const auto composing = m_view->inputMethodQuery(Qt::ImCursorRectangle).toRectF();
        QVERIFY(composing.x() > initial.x());
        screenshot(QStringLiteral("native-preedit"));
        commitMacTestText(m_window, QStringLiteral("日本"));
        QTRY_COMPARE(received(), QStringLiteral("日本").toUtf8());
        setMacTestPreedit(m_window, QStringLiteral("focus commit"));
        // Cocoa commits the marked text to the originating view before the
        // focus transfer. Preserve that native policy and prove no duplication.
        m_view->setFocus(false);
        QTRY_COMPARE(received(), QStringLiteral("日本focus commit").toUtf8());
        m_view->forceActiveFocus();
        nativeKey(0, QStringLiteral("a"), QStringLiteral("a"));
        QTRY_COMPARE(received(), QStringLiteral("日本focus commita").toUtf8());
        // Changing the Option policy explicitly cancels preedit. Its reset
        // must not submit an unfinished command through Cocoa's unmarkText.
        setMacTestPreedit(m_window, QStringLiteral("discard on mode change"));
        m_view->setOptionAsAlt(true);
        QTest::qWait(60);
        QCOMPARE(received(), QStringLiteral("日本focus commita").toUtf8());
        m_session->setReadOnly(true);
        QVERIFY(!m_view->inputMethodQuery(Qt::ImEnabled).toBool());
        commitMacTestText(m_window, QStringLiteral("blocked"));
        QTest::qWait(60);
        QCOMPARE(received(), QStringLiteral("日本focus commita").toUtf8());
    }

    void kittyReleaseFocusAndWheel()
    {
        m_session->feed("\033[>3u");
        nativeKey(7, QStringLiteral("x"), QStringLiteral("x"));
        QTRY_VERIFY(received().contains("120;1:3u"));
        postMacTestKey(m_window, 0, Qt::NoModifier, QStringLiteral("A"), QStringLiteral("A"), false, true);
        postMacTestKey(m_window, 0, Qt::NoModifier, QStringLiteral("A"), QStringLiteral("A"), true, true);
        QTRY_VERIFY(received().contains(";65:3u")); // Kitty modifier 1 + Caps Lock 64.

        m_session->feed("\033[<u\033[?1004h");
        m_view->setFocus(false);
        QTRY_VERIFY(received().contains("\033[O"));
        m_view->forceActiveFocus();
        QTRY_VERIFY(received().endsWith("\033[I"));
        m_session->feed("\033[?1004l");
        const auto before = received();
        for (int n = 0; n < 200; ++n)
            m_session->feed(QByteArray::number(n) + " history\r\n");
        QTest::qWait(80);
        const auto position = m_view->scrollPosition();
        m_view->scrollHistory(QPoint(0, 100), {});
        QTRY_VERIFY(m_view->scrollPosition() < position);
        QCOMPARE(received(), before);
        m_session->feed("\033[?1000h\033[?1006h");
        QWheelEvent wheel{QPointF(20, 20), m_view->mapToGlobal(QPointF(20, 20)), {}, QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false};
        QCoreApplication::sendEvent(m_view, &wheel);
        QTRY_VERIFY(received().contains("\033[<64;"));
    }

    void clipboardConfirmationAndModalShortcuts()
    {
        auto *clipboard = QGuiApplication::clipboard();
        const QString original = clipboard->text();
        const auto restore = qScopeGuard([&] {
            clipboard->setText(original);
        });
        m_session->feed("\033[?2004h");
        clipboard->setText(QStringLiteral("paste-é"));
        nativeKey(9, QStringLiteral("v"), QStringLiteral("v"), Qt::ControlModifier);
        QTRY_COMPARE(received(), QByteArray("\033[200~") + QStringLiteral("paste-é").toUtf8() + "\033[201~");
        clipboard->setText(QStringLiteral("first\nsecond"));
        QSignalSpy confirmation{m_view, &Kwi::TerminalView::multilinePasteRequested};
        nativeKey(9, QStringLiteral("v"), QStringLiteral("v"), Qt::ControlModifier);
        QTRY_COMPARE(confirmation.count(), 1);
        QTRY_VERIFY(!m_window->property("terminalActionsEnabled").toBool());
        auto *model = m_windows->sessions(m_window);
        nativeKey(17, QStringLiteral("t"), QStringLiteral("t"), Qt::ControlModifier);
        QTest::qWait(80);
        QCOMPARE(model->count(), 1);
        QVERIFY(!received().contains("first"));
        // Escape cancels the existing confirmation; no pasted bytes leak.
        nativeKey(53, QString(QChar(27)), QString(QChar(27)));
        QTRY_VERIFY(m_window->property("terminalActionsEnabled").toBool());
        QVERIFY(!received().contains("first"));
        if (!clipboard->supportsSelection()) {
            const auto before = received();
            m_view->pasteSelection();
            QTest::qWait(50);
            QCOMPARE(received(), before);
        }
    }

    void customShortcutPersistsAndResets()
    {
        QQmlEngine engine;
        QQmlComponent component{&engine};
        component.setData("import org.kde.kirigami.actioncollection as AC\nAC.ActionModel { collectionName: 'org.kde.kwi.tabs' }", QUrl{});
        std::unique_ptr<QObject> object{component.create()};
        QVERIFY2(object, qPrintable(component.errorString()));
        auto *model = qobject_cast<QAbstractItemModel *>(object.get());
        QVERIFY(model);
        const auto roles = model->roleNames();
        const int descriptionRole = roles.key("actionDescription", -1);
        const int shortcutRole = roles.key("shortcut", -1);
        QVERIFY(descriptionRole >= 0 && shortcutRole >= 0);
        const auto findNewTab = [&]() {
            for (int n = 0; n < model->rowCount(); ++n) {
                const auto index = model->index(n, 0);
                auto *description = model->data(index, descriptionRole).value<QObject *>();
                if (description && description->property("name").toString() == QLatin1String("new_tab"))
                    return index;
            }
            return QModelIndex{};
        };
        auto target = findNewTab();
        QVERIFY(target.isValid());
        const auto original = model->data(target, shortcutRole);
        auto *collection = KirigamiActions::ActionCollections::self()->collection(QStringLiteral("org.kde.kwi.tabs"));
        QVERIFY(collection);
        auto *action = collection->action(QStringLiteral("new_tab"));
        QVERIFY(action);
        const auto reset = qScopeGuard([&] {
            model->setData(findNewTab(), original, shortcutRole);
            QMetaObject::invokeMethod(model, "save");
        });
        QVERIFY(model->setData(target, QKeySequence(QStringLiteral("Ctrl+Shift+Y")).toString(QKeySequence::NativeText), shortcutRole));
        // The editor stages changes until its Save button invokes this method.
        QVERIFY(QMetaObject::invokeMethod(model, "save"));
        QTRY_COMPARE(action->shortcut(), QKeySequence(QStringLiteral("Ctrl+Shift+Y")));
        const QString resultPath = m_directory.filePath(QStringLiteral("shortcut.txt"));
        const auto persisted = [&](const QString &sequence) {
            // Exercise an actual normal writer shutdown, then a new reader.
            // KConfig can keep shared writes buffered until its owners exit.
            QProcess child;
            auto environment = QProcessEnvironment::systemEnvironment();
            environment.remove(QStringLiteral("KWI_GUI_TEST_PID_FILE"));
            child.setProcessEnvironment(environment);
            child.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--shortcut-write"), resultPath, sequence});
            if (!child.waitForFinished(10000) || child.exitCode() != 0)
                return QString{};
            child.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--shortcut-state"), resultPath});
            if (!child.waitForFinished(10000) || child.exitCode() != 0)
                return QString{};
            QFile file{resultPath};
            return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()).trimmed() : QString{};
        };
        QTest::qWait(100);
        QCOMPARE(persisted(QStringLiteral("Ctrl+Shift+Y")), QStringLiteral("Ctrl+Shift+Y"));
        QVERIFY(activateMacTestWindow(m_window));
        m_view->forceActiveFocus();
        nativeKey(16, QStringLiteral("Y"), QStringLiteral("Y"), Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_COMPARE(m_windows->sessions(m_window)->count(), 2);
        target = findNewTab();
        QVERIFY(target.isValid());
        // Reset in this editor reverts staged edits. Its Default radio stages
        // the default sequence instead; restore that value and Save explicitly.
        QVERIFY(model->setData(target, original, shortcutRole));
        QVERIFY(QMetaObject::invokeMethod(model, "save"));
        QTRY_COMPARE(action->shortcut(), QKeySequence(QStringLiteral("Ctrl+T")));
        QTest::qWait(100);
        QCOMPARE(persisted(QStringLiteral("Ctrl+T")), QStringLiteral("Ctrl+T"));
    }

    void accessibilityCaretPreservesTyping()
    {
        QAccessible::setActive(true);
        auto *accessible = QAccessible::queryAccessibleInterface(m_view);
        QVERIFY(accessible && accessible->textInterface());
        QTRY_VERIFY(macTestSetAccessibilityCaret(m_window, QStringLiteral("KWI_INPUT_READY"), 2));
        QCOMPARE(accessible->textInterface()->cursorPosition(), 2);
        QVERIFY2(!m_view->keyboardSelectionMode(), "Native accessibility caret tracking must not capture terminal typing");
        nativeKey(0, QStringLiteral("a"), QStringLiteral("a"));
        QTRY_COMPARE(received(), QByteArray("a"));
    }

    void accessibilityContextTeardown()
    {
        QAccessible::setActive(true);
        auto engine = std::make_unique<QQmlEngine>();
        auto context = std::make_unique<QQmlContext>(engine->rootContext());
        std::unique_ptr<QObject> object;
        {
            QQmlComponent component{engine.get()};
            component.setData(
                "import QtQuick\nimport org.kde.kwi.terminal\nTerminalView { Accessible.name: 'Owned terminal'; Accessible.description: 'Owned description' }",
                QUrl{});
            object.reset(component.create(context.get()));
            QVERIFY2(object, qPrintable(component.errorString()));
        }
        auto *accessible = QAccessible::queryAccessibleInterface(object.get());
        QVERIFY(accessible);
        QCOMPARE(accessible->text(QAccessible::Name), QStringLiteral("Owned terminal"));
        QCOMPARE(accessible->text(QAccessible::Description), QStringLiteral("Owned description"));
        context.reset();
        engine.reset(); // The view can outlive its QML engine during host teardown.
        QVERIFY(!qmlContext(object.get()) || !qmlContext(object.get())->isValid());
        QCOMPARE(accessible->text(QAccessible::Name), i18n("Terminal"));
        QVERIFY(!accessible->text(QAccessible::Description).isEmpty());
        m_windows.reset(); // Also exercise direct destruction of a focused native host.
        QCoreApplication::processEvents();
    }

    void metalCorpusAccessibilityAndLifecycle()
    {
        m_session->feed(
            "\033[2J\033[HASCII ffi -> != 0123456789\r\n"
            "\033[1;31mBold red\033[0m  \033[3mItalic\033[0m  \033[4:3mCurly underline\033[0m\r\n"
            "\033[7mInverse\033[0m  \033[9mStrike\033[0m  \033[53mOverline\033[0m\r\n");
        m_session->feed(QStringLiteral("Combining: e\u0301  Wide: 日本語  Emoji: 😀 👩‍💻\r\n┌────┬────┐\r\n│box "
                                       "│grid│\r\n└────┴────┘\r\n")
                            .toUtf8());
        screenshot(QStringLiteral("metal-corpus"));
        QByteArray rgba;
        for (int n = 0; n < 16 * 16; ++n) {
            rgba.append(char(255));
            rgba.append(char(32));
            rgba.append(char(64));
            rgba.append(char(255));
        }
        m_session->feed(QByteArray("\033[10;1H\033_Ga=T,f=32,s=16,v=16,i=41,p=1,c=8,r=4,q=2;") + rgba.toBase64() + QByteArray("\033\\"));
        GhosttyQt::Frame imageFrame;
        QVERIFY(m_session->snapshot(imageFrame));
        QCOMPARE(imageFrame.images.size(), 1);
        QCOMPARE(imageFrame.images.first().image.pixelColor(0, 0), QColor(255, 32, 64));
        screenshot(QStringLiteral("metal-image"));
        QAccessible::setActive(true);
        auto *accessible = QAccessible::queryAccessibleInterface(m_view);
        QVERIFY(accessible && accessible->textInterface());
        QVERIFY(accessible->text(QAccessible::Name).startsWith(QLatin1String("Terminal")));
        QVERIFY(accessible->textInterface()->text(0, -1).contains(QStringLiteral("日本語")));
        m_view->selectAll();
        QTRY_VERIFY(accessible->textInterface()->selectionCount() > 0);
        const auto native = macTestAccessibility(m_window);
        QFile record{qEnvironmentVariable("KWI_PERF_OUTPUT") + QStringLiteral("/native-accessibility.json")};
        QVERIFY(record.open(QIODevice::WriteOnly));
        record.write(QJsonDocument(native).toJson());
        bool found = false;
        for (const auto &entry : native.value(QStringLiteral("entries")).toArray())
            if (entry.toObject().value(QStringLiteral("value")).toString().contains(QStringLiteral("日本語")))
                found = true;
        QVERIFY2(found, "The Cocoa accessibility bridge must expose the terminal's actual text");
        const auto pid = m_session->processId();
        m_window->showMinimized();
        QTRY_VERIFY(m_window->windowState() & Qt::WindowMinimized);
        m_window->showNormal();
        QVERIFY(activateMacTestWindow(m_window));
        QCOMPARE(m_session->processId(), pid);
        const auto originalScheme = QGuiApplication::styleHints()->colorScheme();
        const auto restoreAppearance = qScopeGuard([&] {
            QGuiApplication::styleHints()->setColorScheme(originalScheme);
        });
        QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Dark);
        QTRY_COMPARE(QGuiApplication::styleHints()->colorScheme(), Qt::ColorScheme::Dark);
        screenshot(QStringLiteral("metal-dark"));
        QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Light);
        QTRY_COMPARE(QGuiApplication::styleHints()->colorScheme(), Qt::ColorScheme::Light);
        screenshot(QStringLiteral("metal-light"));
        m_settings->setBackgroundOpacity(.8);
        m_settings->setBackgroundBlur(true);
        QTRY_VERIFY(macTestHasBlur(m_window));
        screenshot(QStringLiteral("metal-translucent"));
        m_window->resize(m_window->width() + 60, m_window->height() + 46);
        m_settings->setFontPointSize(12);
        screenshot(QStringLiteral("metal-resized-font"));
        QTRY_VERIFY(macTestHasBlur(m_window));
        m_settings->setBackgroundOpacity(1);
        QTRY_VERIFY(!macTestHasBlur(m_window));
        nativeKey(4, QStringLiteral("h"), QStringLiteral("h"), Qt::ControlModifier);
        QTRY_VERIFY_WITH_TIMEOUT(macTestApplicationHidden(), 3000);
        hideMacTestApplication(false);
        QVERIFY(activateMacTestWindow(m_window));
        QCOMPARE(m_session->processId(), pid);
    }
};

int main(int argc, char **argv)
{
    const bool helper = argc >= 3
        && (QString::fromLocal8Bit(argv[1]) == QLatin1String("--shortcut-state")
            || (argc == 4 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--shortcut-write")));
    if (!helper)
        recordMacTestProcess();
    QStandardPaths::setTestModeEnabled(true);
    KIconTheme::initTheme();
    BreezeIcons::initIcons();
    QQuickWindow::setDefaultAlphaBuffer(true);
    QApplication app{argc, argv};
    QCoreApplication::setApplicationName(QStringLiteral("kwi-macos-interaction-test"));
    KLocalizedString::setApplicationDomain("kwi");
    if (helper) {
        KirigamiAppDefaults::apply(qApp);
        Kwi::AppShortcuts shortcuts;
        auto *collection = KirigamiActions::ActionCollections::self()->collection(QStringLiteral("org.kde.kwi.tabs"));
        auto *action = collection ? collection->action(QStringLiteral("new_tab")) : nullptr;
        if (argc == 4) {
            QQmlEngine engine;
            QQmlComponent component{&engine};
            component.setData("import org.kde.kirigami.actioncollection as AC\nAC.ActionModel { collectionName: 'org.kde.kwi.tabs' }", QUrl{});
            std::unique_ptr<QObject> object{component.create()};
            auto *model = qobject_cast<QAbstractItemModel *>(object.get());
            if (!model)
                return 3;
            const auto roles = model->roleNames();
            bool updated = false;
            for (int n = 0; n < model->rowCount(); ++n) {
                const auto index = model->index(n, 0);
                auto *description = model->data(index, roles.key("actionDescription")).value<QObject *>();
                if (description && description->property("name").toString() == QLatin1String("new_tab")) {
                    updated = model->setData(index, QKeySequence(QString::fromLocal8Bit(argv[3])).toString(QKeySequence::NativeText), roles.key("shortcut"));
                    break;
                }
            }
            if (!updated || !QMetaObject::invokeMethod(model, "save"))
                return 4;
        }
        QFile output{QString::fromLocal8Bit(argv[2])};
        if (!action || !output.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return 2;
        output.write(action->shortcut().toString(QKeySequence::PortableText).toUtf8());
        return 0;
    }
    MacInteractionTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "MacInteractionTest.moc"
