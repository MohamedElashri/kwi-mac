// SPDX-FileCopyrightText: 2026 Kwi contributors
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "Diagnostics_p.h"
#include "Frame.h"
#include "MacGuiTestSupport.h"
#include "MacMetalTiming.h"
#include "OutputSaveJob.h"
#include "SessionModel.h"
#include "Terminal.h"
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
#include <QAccessible>
#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFontDatabase>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QPixmapCache>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QScreen>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QtCore/private/qcore_mac_p.h>

extern "C" uint64_t kwi_current_drawable_sequence();
#include <algorithm>
#include <cmath>
#include <libproc.h>
#include <mach/mach.h>
#include <memory>
#include <sys/resource.h>
#include <unistd.h>

namespace
{
class KeyDelivery final : public QObject
{
public:
    QObject *window{};
    int windowPresses{0};
    int viewPresses{0};
    int lastKey{0};
    Qt::KeyboardModifiers lastModifiers{};
    QString lastText;

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::KeyPress) {
            auto *key = static_cast<QKeyEvent *>(event);
            if (watched == window)
                ++windowPresses;
            else
                ++viewPresses;
            lastKey = key->key();
            lastModifiers = key->modifiers();
            lastText = key->text();
        }
        return false;
    }
};

template<typename Predicate>
bool awaitGui(Predicate ready, int timeout)
{
    if (ready())
        return true;
    QEventLoop loop;
    QTimer observer, deadline;
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

void settle(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

QJsonObject memory()
{
    task_vm_info_data_t info{};
    mach_msg_type_number_t count{TASK_VM_INFO_COUNT};
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS)
        return {};
    rusage_info_v4 usage{};
    const bool hasWakeups = proc_pid_rusage(getpid(), RUSAGE_INFO_V4, reinterpret_cast<rusage_info_t *>(&usage)) == 0;
    return {{QStringLiteral("physical_footprint"), qint64(info.phys_footprint)},
            {QStringLiteral("rss"), qint64(info.resident_size)},
            {QStringLiteral("wakeups_available"), hasWakeups},
            {QStringLiteral("interrupt_wakeups"), qint64(usage.ri_interrupt_wkups)},
            {QStringLiteral("package_idle_wakeups"), qint64(usage.ri_pkg_idle_wkups)}};
}

double cpuSeconds()
{
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_utime.tv_sec + usage.ru_stime.tv_sec + (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1e6;
}

int durationMs()
{
    const int seconds{qEnvironmentVariableIntValue("KWI_PERF_SECONDS")};
    return qBound(1, seconds > 0 ? seconds : 60, 60) * 1000;
}

double percentile(QJsonArray values, double fraction)
{
    QList<double> numbers;
    for (const auto &value : values)
        numbers.append(value.toDouble());
    std::sort(numbers.begin(), numbers.end());
    return numbers.isEmpty() ? 0 : numbers.at(qBound(0, int(std::ceil(numbers.size() * fraction)) - 1, int(numbers.size() - 1)));
}

struct Monitor {
    QMutex mutex;
    QHash<quint64, QJsonObject> renders;
    QHash<quint64, QJsonObject> snapshots;
    qint64 lastFrame{0};
    quint64 lastDrawable{0};
} monitor;
QtMessageHandler previousHandler{};

void messageHandler(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    if (message.startsWith(QLatin1String("KWI_TRACE "))) {
        const auto event = QJsonDocument::fromJson(message.mid(10).toUtf8()).object();
        QMutexLocker lock{&monitor.mutex};
        if (event.value(QStringLiteral("kind")) == QLatin1String("render"))
            monitor.renders.insert(event.value(QStringLiteral("terminal_id")).toInteger(), event);
        if (event.value(QStringLiteral("kind")) == QLatin1String("snapshot-generation"))
            monitor.snapshots.insert(event.value(QStringLiteral("terminal_id")).toInteger(), event);
        if (event.value(QStringLiteral("kind")) == QLatin1String("frame-swapped")) {
            monitor.lastFrame = event.value(QStringLiteral("time_ns")).toInteger();
            monitor.lastDrawable = event.value(QStringLiteral("drawable_sequence")).toInteger();
        }
        return; // Keep per-frame JSON off disk and out of QtTest's warning limit.
    }
    if (previousHandler)
        previousHandler(type, context, message);
}

QJsonObject rendered(quint64 terminal)
{
    QMutexLocker lock{&monitor.mutex};
    return monitor.renders.value(terminal);
}

GhosttyQt::Frame observedFrame(quint64 terminal)
{
    QMutexLocker lock{&monitor.mutex};
    GhosttyQt::Frame frame;
    frame.terminalId = terminal;
    frame.generation = monitor.snapshots.value(terminal).value(QStringLiteral("generation")).toInteger();
    return frame;
}

bool preparedAndSwapped(const GhosttyQt::Frame &frame)
{
    QMutexLocker lock{&monitor.mutex};
    const auto event = monitor.renders.value(frame.terminalId);
    return !event.isEmpty() && quint64(event.value(QStringLiteral("generation")).toInteger()) >= frame.generation
        && monitor.lastFrame >= event.value(QStringLiteral("time_ns")).toInteger();
}

QJsonObject readJson(const QString &path)
{
    QFile file{path};
    return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject{};
}

void touch(const QString &path)
{
    QFile file{path};
    if (file.open(QIODevice::WriteOnly))
        file.close();
}
}

class MacPerformanceTest : public QObject
{
    Q_OBJECT
    std::unique_ptr<Kwi::TerminalSettings> m_settings;
    std::unique_ptr<Kwi::TerminalPresets> m_presets;
    std::unique_ptr<Kwi::AppShortcuts> m_shortcuts;
    std::unique_ptr<Kwi::TerminalPaletteModel> m_palettes;

    void record(const QString &name, QJsonObject result)
    {
        const auto tag = QString::fromLatin1(QTest::currentDataTag());
        result.insert(QStringLiteral("case"), tag);
        result.insert(QStringLiteral("trace"), Kwi::traceEnabled());
        result.insert(QStringLiteral("scope"), QStringLiteral("Native Qt/Metal diagnostic fixture; frame callbacks are not pixel presentation."));
        QDir directory{qEnvironmentVariable("KWI_PERF_OUTPUT")};
        QVERIFY(directory.mkpath(QStringLiteral(".")));
        QFile file{directory.filePath(name + (tag.isEmpty() ? QString{} : QLatin1Char('-') + tag) + QStringLiteral(".json"))};
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument{result}.toJson());
    }

    bool fit(QQuickWindow *window, Kwi::TerminalSession *session, int columns, int rows)
    {
        // Test font Menlo 10 pt is 6 x 11.5 logical pixels at this Mac's 2x/72 DPI.
        // Verify both the hardware condition and the resulting PTY grid.
        if (window->effectiveDevicePixelRatio() != 2 || qRound(window->screen()->logicalDotsPerInch()) != 72)
            return false;
        for (int i{0}; i < 8; ++i) {
            GhosttyQt::Frame frame;
            if (!session->snapshot(frame)) {
                settle(100);
                continue;
            }
            if (frame.columns == columns && frame.rows.size() == rows)
                return true;
            window->resize(window->width() + (columns - frame.columns) * 6, window->height() + qRound((rows - frame.rows.size()) * 11.5));
            settle(100);
        }
        GhosttyQt::Frame frame;
        return session->snapshot(frame) && frame.columns == columns && frame.rows.size() == rows;
    }

    Kwi::LaunchOptions command(const QStringList &arguments)
    {
        Kwi::LaunchOptions options;
        options.launch.command = arguments;
        options.hold = true;
        return options;
    }

    QStringList driver(const QString &directory, const QStringList &arguments)
    {
        return QStringList{qEnvironmentVariable("KWI_TEST_PYTHON"), QStringLiteral(KWI_TEST_DRIVER), QStringLiteral("--directory"), directory} + arguments;
    }

private Q_SLOTS:
    void init()
    {
        if (!qEnvironmentVariableIsSet("KWI_PERF_BACKGROUND"))
            activateMacTestApplication();
    }

    void initTestCase()
    {
        QVERIFY(QFile::exists(qEnvironmentVariable("KWI_TEST_PYTHON")));
        QVERIFY(!qEnvironmentVariable("KWI_PERF_OUTPUT").isEmpty());
        previousHandler = qInstallMessageHandler(messageHandler);
        KirigamiAppDefaults::apply(qApp);
        m_settings = std::make_unique<Kwi::TerminalSettings>();
        m_settings->setFontFamily(QStringLiteral("Menlo"));
        m_settings->setFontPointSize(10);
        m_settings->setScrollbackLines(100000);
        m_settings->setCursorBlinking(false);
        m_presets = std::make_unique<Kwi::TerminalPresets>(m_settings.get());
        m_shortcuts = std::make_unique<Kwi::AppShortcuts>();
        m_palettes = std::make_unique<Kwi::TerminalPaletteModel>(m_settings.get());
    }

    void cleanupTestCase()
    {
        qInstallMessageHandler(previousHandler);
    }

    void glyphCaches()
    {
        QVERIFY(Kwi::traceEnabled());
        Kwi::WindowManager windows{m_settings.get(), m_presets.get(), m_shortcuts.get(), m_palettes.get()};
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        auto *window = windows.launch(
            command(driver(directory.path(), {QStringLiteral("--glyphs"), QStringLiteral("--workload"), QStringLiteral(KWI_TEST_WORKLOADS "/unicode.txt")})));
        QVERIFY(window && activateMacTestWindow(window, 10000));
        auto *session = windows.sessions(window)->currentSession();
        auto *view = window->findChild<Kwi::TerminalView *>();
        QVERIFY(view && fit(window, session, 220, 60));
        view->forceActiveFocus();
        GhosttyQt::Frame identity;
        QVERIFY(session->snapshot(identity));
        const auto terminal = identity.terminalId;
        view->polish();
        QVERIFY(awaitGui(
            [&] {
                return observedFrame(terminal).generation >= identity.generation && preparedAndSwapped(observedFrame(terminal));
            },
            5000));
        QVERIFY(awaitGui(
            [&] {
                return !readJson(directory.filePath(QStringLiteral("ready.json"))).isEmpty();
            },
            10000));
        touch(directory.filePath(QStringLiteral("start")));
        QVERIFY(awaitGui(
            [&] {
                return !readJson(directory.filePath(QStringLiteral("started.json"))).isEmpty();
            },
            10000));
        const auto draw = [&](int pass) {
            const auto generation = observedFrame(terminal).generation;
            if (!session->sendInput(QStringLiteral("g")))
                return false;
            if (!awaitGui(
                    [&] {
                        return readJson(directory.filePath(QStringLiteral("pass.json"))).value(QStringLiteral("pass")).toInt() >= pass;
                    },
                    10000))
                return false;
            return awaitGui(
                [&] {
                    return observedFrame(terminal).generation > generation && preparedAndSwapped(observedFrame(terminal));
                },
                10000);
        };
        QJsonArray passes;
        for (int pass{1}; pass <= 2; ++pass) {
            QElapsedTimer elapsed;
            elapsed.start();
            QVERIFY(draw(pass));
            auto stats = rendered(terminal);
            stats.insert(QStringLiteral("feed_to_callback_ms"), elapsed.nsecsElapsed() / 1e6);
            passes.append(stats);
        }
        QCOMPARE(passes[0].toObject().value(QStringLiteral("rasterized_glyphs")), passes[1].toObject().value(QStringLiteral("rasterized_glyphs")));
        QCOMPARE(passes[0].toObject().value(QStringLiteral("upload_bytes")), passes[1].toObject().value(QStringLiteral("upload_bytes")));
        view->setCursorBlinking(true);
        settle(1200);
        const auto beforeBlink = rendered(terminal);
        settle(1500);
        const auto afterBlink = rendered(terminal);
        QVERIFY(afterBlink.value(QStringLiteral("time_ns")).toInteger() > beforeBlink.value(QStringLiteral("time_ns")).toInteger());
        QVERIFY(afterBlink.value(QStringLiteral("rebuilt_rows")).toInteger() > beforeBlink.value(QStringLiteral("rebuilt_rows")).toInteger());
        QCOMPARE(beforeBlink.value(QStringLiteral("rasterized_glyphs")), afterBlink.value(QStringLiteral("rasterized_glyphs")));
        QCOMPARE(beforeBlink.value(QStringLiteral("upload_bytes")), afterBlink.value(QStringLiteral("upload_bytes")));
        view->setCursorBlinking(false);
        QJsonArray changes;
        int pass{2};
        for (const auto size : {11.0, 10.0}) {
            m_settings->setFontPointSize(size);
            settle(200);
            QVERIFY(draw(++pass));
            changes.append(rendered(terminal));
        }
        for (int n{0}; n < 10; ++n) {
            QVERIFY(fit(window, session, n % 2 ? 220 : 120, n % 2 ? 60 : 40));
            const auto generation = observedFrame(terminal).generation;
            view->polish();
            QVERIFY(awaitGui(
                [&] {
                    return observedFrame(terminal).generation > generation && preparedAndSwapped(observedFrame(terminal));
                },
                10000));
        }
        record(QStringLiteral("glyph-caches"),
               {{QStringLiteral("cold_warm_passes"), passes},
                {QStringLiteral("font_changes"), changes},
                {QStringLiteral("before_blink"), beforeBlink},
                {QStringLiteral("after_blink"), afterBlink},
                {QStringLiteral("resizes"), 10},
                {QStringLiteral("dpr"), window->effectiveDevicePixelRatio()}});
        touch(directory.filePath(QStringLiteral("stop")));
        windows.sessions(window)->closeAll(true);
        QVERIFY(awaitGui(
            [&] {
                return windows.count() == 0;
            },
            5000));
    }

    void idle_data()
    {
        QTest::addColumn<int>("tabs");
        QTest::addColumn<QString>("state");
        for (int count : {1, 10}) {
            for (const auto &state : {QStringLiteral("visible"),
                                      QStringLiteral("unfocused"),
                                      QStringLiteral("hidden"),
                                      QStringLiteral("minimized"),
                                      QStringLiteral("app-hidden")}) {
                const auto name = QStringLiteral("%1-%2").arg(count).arg(state).toLatin1();
                QTest::newRow(name.constData()) << count << state;
            }
        }
    }

    void backgroundRendering()
    {
        QVERIFY(qEnvironmentVariableIsSet("KWI_PERF_BACKGROUND"));
        QVERIFY(!macTestVoiceOverEnabled());
        Kwi::WindowManager windows{m_settings.get(), m_presets.get(), m_shortcuts.get(), m_palettes.get()};
        QTemporaryDir directory;
        auto *window = windows.launch(command(driver(directory.path(), {QStringLiteral("--echo")})));
        QVERIFY(window && QTest::qWaitForWindowExposed(window, 10000));
        QVERIFY(QGuiApplication::applicationState() != Qt::ApplicationActive);
        auto *session = windows.sessions(window)->currentSession();
        QVERIFY(awaitGui(
            [&] {
                return !readJson(directory.filePath(QStringLiteral("ready.json"))).isEmpty();
            },
            10000));
        if (window->effectiveDevicePixelRatio() == 2)
            QVERIFY(fit(window, session, 120, 40));
        else
            QCOMPARE(window->effectiveDevicePixelRatio(), 1);
        session->feed(
            "\033[2J\033[HASCII ffi -> != 0123456789\r\n\033[1;31mBold red\033[0m  \033[3mItalic\033[0m  \033[4:3mCurly underline\033[0m\r\n"
            "\033[7mInverse\033[0m  \033[9mStrike\033[0m  \033[53mOverline\033[0m\r\n");
        session->feed(QStringLiteral("Combining: e\u0301  Wide: 日本語  Emoji: 😀 👩‍💻\r\n┌────┬────┐\r\n│box "
                                     "│grid│\r\n└────┴────┘\r\n")
                          .toUtf8());
        QByteArray rgba;
        for (int n = 0; n < 16 * 16; ++n) {
            rgba.append(char(255));
            rgba.append(char(32));
            rgba.append(char(64));
            rgba.append(char(255));
        }
        session->feed(QByteArray("\033[10;1H\033_Ga=T,f=32,s=16,v=16,i=41,p=1,c=8,r=4,q=2;") + rgba.toBase64() + QByteArray("\033\\"));
        GhosttyQt::Frame frame;
        QVERIFY(session->snapshot(frame));
        QCOMPARE(frame.images.size(), 1);
        QCOMPARE(frame.images.first().image.pixelColor(0, 0), QColor(255, 32, 64));
        settle(300);
        QCOMPARE(window->rendererInterface()->graphicsApi(), QSGRendererInterface::Metal);
        const QDir output{qEnvironmentVariable("KWI_PERF_OUTPUT")};
        const QImage opaque = window->grabWindow();
        QVERIFY(!opaque.isNull() && opaque.save(output.filePath(QStringLiteral("background-metal.png"))));
        int imagePixels{0};
        for (int y = 0; y < opaque.height(); ++y)
            for (int x = 0; x < opaque.width(); ++x) {
                const auto color = opaque.pixelColor(x, y);
                imagePixels +=
                    color.red() > 240 && color.green() >= 20 && color.green() <= 45 && color.blue() >= 50 && color.blue() <= 80 && color.alpha() > 240;
            }
        QVERIFY(imagePixels > 100);
        m_settings->setBackgroundOpacity(.8);
        m_settings->setBackgroundBlur(true);
        QTRY_VERIFY(macTestHasBlur(window));
        window->resize(window->width() + 60, window->height() + 46);
        m_settings->setFontPointSize(12);
        settle(300);
        QVERIFY(window->grabWindow().save(output.filePath(QStringLiteral("background-translucent-resized.png"))));
        QVERIFY(macTestHasBlur(window));
        m_settings->setBackgroundOpacity(1);
        QTRY_VERIFY(!macTestHasBlur(window));
        QVERIFY(QGuiApplication::applicationState() != Qt::ApplicationActive);
        record(QStringLiteral("background-rendering"),
               {{QStringLiteral("metal"), true},
                {QStringLiteral("dpr"), window->effectiveDevicePixelRatio()},
                {QStringLiteral("known_image_pixels"), imagePixels},
                {QStringLiteral("application_active"), false},
                {QStringLiteral("voiceover_running"), false},
                {QStringLiteral("blur_resize_remove"), true}});
        touch(directory.filePath(QStringLiteral("stop")));
        windows.sessions(window)->closeAll(true);
        QVERIFY(awaitGui(
            [&] {
                return windows.count() == 0;
            },
            5000));
    }

    void idle()
    {
        QFETCH(int, tabs);
        QFETCH(QString, state);
        QVERIFY(!Kwi::traceEnabled());
        const bool background = qEnvironmentVariableIsSet("KWI_PERF_BACKGROUND");
        QVERIFY2(!background || state == QLatin1String("hidden") || state == QLatin1String("minimized") || state == QLatin1String("app-hidden"),
                 "Background launch is only supported for hidden/minimized idle conditions");
        Kwi::WindowManager windows{m_settings.get(), m_presets.get(), m_shortcuts.get(), m_palettes.get()};
        auto options = command({QStringLiteral("/bin/sh"), QStringLiteral("-i")});
        auto *window = windows.launch(options);
        QVERIFY(window && (background ? QTest::qWaitForWindowExposed(window, 10000) : activateMacTestWindow(window, 10000)));
        if (background)
            QVERIFY(QGuiApplication::applicationState() != Qt::ApplicationActive);
        for (int n{1}; n < tabs; ++n) {
            options.newTab = true;
            QVERIFY(windows.launch(options));
        }
        auto *model = windows.sessions(window);
        QVERIFY(fit(window, model->currentSession(), 120, 40));
        QWindow other;
        if (state == QLatin1String("app-hidden"))
            hideMacTestApplication(true);
        else if (state == QLatin1String("hidden"))
            window->hide();
        else if (state == QLatin1String("minimized"))
            window->showMinimized();
        else if (state == QLatin1String("unfocused")) {
            other.resize(160, 80);
            other.show();
            other.requestActivate();
            QVERIFY(activateMacTestWindow(&other, 5000));
        }
        const auto expectedState = [&] {
            if (state == QLatin1String("visible"))
                return window->isVisible() && window->isExposed() && window->isActive() && QGuiApplication::applicationState() == Qt::ApplicationActive
                    && QGuiApplication::focusWindow() == window;
            if (state == QLatin1String("unfocused"))
                return window->isVisible() && !window->isActive() && other.isActive() && QGuiApplication::applicationState() == Qt::ApplicationActive
                    && QGuiApplication::focusWindow() == &other;
            if (state == QLatin1String("hidden"))
                return !window->isVisible() && !macTestApplicationHidden();
            if (state == QLatin1String("minimized"))
                return bool(window->windowState() & Qt::WindowMinimized) && !macTestApplicationHidden()
                    && macTestFocusState(window).value(QStringLiteral("native_window_minimized")).toBool();
            if (state == QLatin1String("app-hidden"))
                return macTestApplicationHidden();
            return true;
        };
        const auto stateRecord = [&] {
            return QJsonObject{{QStringLiteral("state"), state},
                               {QStringLiteral("tabs"), tabs},
                               {QStringLiteral("actual_tab_count"), model->count()},
                               {QStringLiteral("window_active"), window->isActive()},
                               {QStringLiteral("window_visible"), window->isVisible()},
                               {QStringLiteral("window_exposed"), window->isExposed()},
                               {QStringLiteral("window_states"), int(window->windowStates())},
                               {QStringLiteral("native_focus"), macTestFocusState(window)}};
        };
        settle(5000);
        if (!expectedState())
            record(QStringLiteral("idle-state-failed-before"), stateRecord());
        QVERIFY(expectedState());
        if (background)
            QVERIFY(!window->isActive() && QGuiApplication::applicationState() != Qt::ApplicationActive);
        const auto before = memory();
        const auto cpuBefore = cpuSeconds();
        QElapsedTimer timer;
        timer.start();
        settle(durationMs()); // One deadline timer; no polling/observer wakeups.
        const auto measured = timer.nsecsElapsed() / 1e9;
        const auto cpu = (cpuSeconds() - cpuBefore) / measured * 100;
        if (!expectedState())
            record(QStringLiteral("idle-state-failed-after"), stateRecord());
        QVERIFY(expectedState());
        if (background)
            QVERIFY(!window->isActive() && QGuiApplication::applicationState() != Qt::ApplicationActive);
        record(QStringLiteral("idle"),
               {{QStringLiteral("tabs"), tabs},
                {QStringLiteral("state"), state},
                {QStringLiteral("measured_seconds"), measured},
                {QStringLiteral("cpu_percent_one_core"), cpu},
                {QStringLiteral("before"), before},
                {QStringLiteral("after"), memory()},
                {QStringLiteral("window_visible"), window->isVisible()},
                {QStringLiteral("window_exposed"), window->isExposed()},
                {QStringLiteral("window_active"), window->isActive()},
                {QStringLiteral("native_app_hidden"), macTestApplicationHidden()},
                {QStringLiteral("voiceover_running"), macTestVoiceOverEnabled()},
                {QStringLiteral("launch_background"), background},
                {QStringLiteral("application_active"), QGuiApplication::applicationState() == Qt::ApplicationActive},
                {QStringLiteral("note"),
                 QStringLiteral("Hidden is QWindow::hide; app-hidden uses NSApp hide (the native Cmd-H action). Unfocused uses another owned window. App "
                                "Nap/energy attribution is separate.")}});
        other.hide();
        if (state == QLatin1String("app-hidden"))
            hideMacTestApplication(false, !background);
        if (!background)
            window->showNormal();
        model->closeAll(true);
        QVERIFY(awaitGui(
            [&] {
                return windows.count() == 0;
            },
            5000));
    }

    void interactive_data()
    {
        QTest::addColumn<int>("columns");
        QTest::addColumn<int>("rows");
        QTest::addColumn<bool>("loaded");
        QTest::addColumn<double>("opacity");
        QTest::addColumn<bool>("accessible");
        QTest::addColumn<bool>("separateWindow");
        QTest::newRow("idle120") << 120 << 40 << false << 1.0 << false << false;
        QTest::newRow("loaded120") << 120 << 40 << true << 1.0 << false << false;
        QTest::newRow("loaded220") << 220 << 60 << true << 1.0 << false << false;
        QTest::newRow("translucent120") << 120 << 40 << true << .8 << false << false;
        QTest::newRow("accessible120") << 120 << 40 << true << 1.0 << true << false;
        QTest::newRow("multiwindow120") << 120 << 40 << true << 1.0 << false << true;
    }

    void interactive()
    {
        QFETCH(int, columns);
        QFETCH(int, rows);
        QFETCH(bool, loaded);
        QFETCH(double, opacity);
        QFETCH(bool, accessible);
        QFETCH(bool, separateWindow);
        QVERIFY(Kwi::traceEnabled());
        m_settings->setBackgroundOpacity(opacity);
        QAccessible::setActive(accessible);
        QTemporaryDir front, back;
        QVERIFY(front.isValid() && back.isValid());
        Kwi::WindowManager windows{m_settings.get(), m_presets.get(), m_shortcuts.get(), m_palettes.get()};
        auto *window = windows.launch(command(driver(front.path(), {QStringLiteral("--echo"), QStringLiteral("--seconds"), QStringLiteral("75")})));
        QVERIFY(window && activateMacTestWindow(window, 10000));
        QVERIFY(watchMacMetalTiming(window));
        // Another window can swap after this terminal is prepared. Correlate
        // responses with this window's render-thread drawable, never a global
        // "last swapped" frame from the producer window.
        struct ResponseFrames {
            QMutex mutex;
            qint64 lastFrame{0};
            quint64 lastDrawable{0};
            QJsonArray times;
            bool collect{false};
        };
        const auto responseFrames = std::make_shared<ResponseFrames>();
        connect(
            window,
            &QQuickWindow::frameSwapped,
            window,
            [responseFrames] {
                QMutexLocker lock{&responseFrames->mutex};
                responseFrames->lastFrame = Kwi::traceTime();
                responseFrames->lastDrawable = kwi_current_drawable_sequence();
                if (responseFrames->collect)
                    responseFrames->times.append(responseFrames->lastFrame);
            },
            Qt::DirectConnection);
        const auto responsePreparedAndSwapped = [&](const GhosttyQt::Frame &frame) {
            const auto event = rendered(frame.terminalId);
            QMutexLocker lock{&responseFrames->mutex};
            return !event.isEmpty() && quint64(event.value(QStringLiteral("generation")).toInteger()) >= frame.generation
                && responseFrames->lastFrame >= event.value(QStringLiteral("time_ns")).toInteger();
        };
        auto *model = windows.sessions(window);
        auto *session = model->currentSession();
        auto *view = window->findChild<Kwi::TerminalView *>();
        QVERIFY(view && fit(window, session, columns, rows));
        QVERIFY(awaitGui(
            [&] {
                return !readJson(front.filePath(QStringLiteral("ready.json"))).isEmpty();
            },
            10000));
        Kwi::TerminalSession *producer{};
        QQuickWindow *producerWindow{};
        if (loaded) {
            auto background =
                command(driver(back.path(),
                               {QStringLiteral("--seconds"), QString::number(durationMs() / 1000 + 5), QStringLiteral("--rate"), QStringLiteral("8388608")}));
            background.newTab = !separateWindow;
            producerWindow = windows.launch(background);
            QVERIFY(producerWindow);
            producer = windows.sessions(producerWindow)->currentSession();
            QVERIFY(awaitGui(
                [&] {
                    return !readJson(back.filePath(QStringLiteral("ready.json"))).isEmpty();
                },
                10000));
            model->setCurrentIndex(0);
            QVERIFY(awaitGui(
                [&] {
                    return view->session() == session;
                },
                5000));
        }
        QVERIFY(fit(window, session, columns, rows));
        QVERIFY(activateMacTestWindow(window, 10000));
        view->forceActiveFocus();
        GhosttyQt::Frame identity;
        QVERIFY(session->snapshot(identity));
        const auto terminal = identity.terminalId;
        view->polish();
        QVERIFY(awaitGui(
            [&] {
                return observedFrame(terminal).generation >= identity.generation && responsePreparedAndSwapped(observedFrame(terminal));
            },
            5000));
        auto *accessibleFrame = QAccessible::queryAccessibleInterface(view);
        QVERIFY(accessibleFrame);
        QAccessible::setActive(accessible);
        touch(front.filePath(QStringLiteral("start")));
        if (loaded)
            touch(back.filePath(QStringLiteral("start")));
        QVERIFY(awaitGui(
            [&] {
                return !readJson(front.filePath(QStringLiteral("started.json"))).isEmpty();
            },
            10000));
        if (loaded)
            QVERIFY(awaitGui(
                [&] {
                    return !readJson(back.filePath(QStringLiteral("started.json"))).isEmpty();
                },
                10000));
        QJsonArray samples;
        QJsonArray sampleEvents;
        {
            QMutexLocker lock{&responseFrames->mutex};
            responseFrames->times = {};
            responseFrames->collect = true;
        }
        QElapsedTimer elapsed;
        elapsed.start();
        int sequence{0};
        KeyDelivery keys;
        keys.window = window;
        window->installEventFilter(&keys);
        view->installEventFilter(&keys);
        while (elapsed.elapsed() < durationMs()) {
            const auto stamp{Kwi::traceTime()};
            QTest::keyClick(window, Qt::Key_A);
            const auto marker = QStringLiteral("ECHO_%1_61").arg(++sequence, 6, 10, QLatin1Char('0'));
            const bool echoed = awaitGui(
                [&] {
                    return accessibleFrame->text(QAccessible::Value).contains(marker);
                },
                2000);
            if (!echoed) {
                auto *focus = window->activeFocusItem();
                const bool keyboardSelection = view->keyboardSelectionMode();
                const int windowPresses = keys.windowPresses;
                const int viewPresses = keys.viewPresses;
                const int lastKey = keys.lastKey;
                const auto lastModifiers = keys.lastModifiers;
                const auto lastText = keys.lastText;
                bool echoAfterLeavingSelection = false;
                if (keyboardSelection) {
                    view->setKeyboardSelectionMode(false);
                    QTest::keyClick(window, Qt::Key_A);
                    echoAfterLeavingSelection = awaitGui(
                        [&] {
                            return accessibleFrame->text(QAccessible::Value).contains(marker);
                        },
                        2000);
                }
                const bool directAccepted = !echoAfterLeavingSelection && session->sendInput(QStringLiteral("a"));
                const bool echoAfterDirectInput = directAccepted
                    && awaitGui(
                                                      [&] {
                                                          return accessibleFrame->text(QAccessible::Value).contains(marker);
                                                      },
                                                      2000);
                record(QStringLiteral("interactive-failed"),
                       {{QStringLiteral("expected_echo"), marker},
                        {QStringLiteral("measured_ms"), elapsed.elapsed()},
                        {QStringLiteral("sample_events"), sampleEvents},
                        {QStringLiteral("window_active"), window->isActive()},
                        {QStringLiteral("native_focus"), macTestFocusState(window)},
                        {QStringLiteral("focus_window_matches"), QGuiApplication::focusWindow() == window},
                        {QStringLiteral("view_active_focus"), view->hasActiveFocus()},
                        {QStringLiteral("keyboard_selection_before_diagnostic_retry"), keyboardSelection},
                        {QStringLiteral("echo_after_leaving_keyboard_selection"), echoAfterLeavingSelection},
                        {QStringLiteral("window_key_presses"), windowPresses},
                        {QStringLiteral("view_key_presses"), viewPresses},
                        {QStringLiteral("last_key"), lastKey},
                        {QStringLiteral("last_modifiers"), int(lastModifiers)},
                        {QStringLiteral("last_key_text"), lastText},
                        {QStringLiteral("direct_input_accepted"), directAccepted},
                        {QStringLiteral("echo_after_direct_input"), echoAfterDirectInput},
                        {QStringLiteral("view_visible"), view->isVisible()},
                        {QStringLiteral("focus_item_class"), focus ? QString::fromLatin1(focus->metaObject()->className()) : QString{}},
                        {QStringLiteral("focus_item_name"), focus ? focus->objectName() : QString{}},
                        {QStringLiteral("session_finished"), session->isFinished()},
                        {QStringLiteral("session_read_only"), session->isReadOnly()},
                        {QStringLiteral("accessible_text_tail"), accessibleFrame->text(QAccessible::Value).right(512)},
                        {QStringLiteral("history_tail"), session->historyText().right(512)},
                        {QStringLiteral("driver_ready"), readJson(front.filePath(QStringLiteral("ready.json")))},
                        {QStringLiteral("driver_started"), readJson(front.filePath(QStringLiteral("started.json")))},
                        {QStringLiteral("driver_result"), readJson(front.filePath(QStringLiteral("result.json")))},
                        {QStringLiteral("driver_error"), readJson(front.filePath(QStringLiteral("error.json")))},
                        {QStringLiteral("voiceover_running"), macTestVoiceOverEnabled()}});
            }
            QVERIFY2(echoed, qPrintable(marker));
            const auto frame = observedFrame(terminal);
            QVERIFY(frame.generation > 0);
            QVERIFY(awaitGui(
                [&] {
                    return responsePreparedAndSwapped(frame);
                },
                2000));
            qint64 observed;
            quint64 drawable;
            {
                QMutexLocker lock{&responseFrames->mutex};
                observed = responseFrames->lastFrame;
                drawable = responseFrames->lastDrawable;
            }
            samples.append((observed - stamp) / 1e6);
            sampleEvents.append(QJsonObject{{QStringLiteral("event_trace_time_ns"), stamp},
                                            {QStringLiteral("prepared_frame_callback_ns"), observed},
                                            {QStringLiteral("drawable_sequence"), qint64(drawable)},
                                            {QStringLiteral("terminal_id"), qint64(terminal)},
                                            {QStringLiteral("generation"), qint64(frame.generation)}});
        }
        QJsonArray frames;
        {
            QMutexLocker lock{&responseFrames->mutex};
            responseFrames->collect = false;
            frames = responseFrames->times;
        }
        QJsonArray intervals;
        int missed{0};
        for (int i{1}; i < frames.size(); ++i) {
            const double ms{(frames[i].toInteger() - frames[i - 1].toInteger()) / 1e6};
            intervals.append(ms);
            missed += ms > 1000.0 / 60 * 1.5;
        }
        QJsonObject result{{QStringLiteral("samples_ms"), samples},
                           {QStringLiteral("sample_events"), sampleEvents},
                           {QStringLiteral("measured_ms"), elapsed.elapsed()},
                           {QStringLiteral("sample_count"), samples.size()},
                           {QStringLiteral("window_key_presses"), keys.windowPresses},
                           {QStringLiteral("view_key_presses"), keys.viewPresses},
                           {QStringLiteral("event_to_echo_prepared_frame_p50_ms"), percentile(samples, .5)},
                           {QStringLiteral("event_to_echo_prepared_frame_p95_ms"), percentile(samples, .95)},
                           {QStringLiteral("event_to_echo_prepared_frame_p99_ms"), percentile(samples, .99)},
                           {QStringLiteral("frame_callback_intervals_ms"), intervals},
                           {QStringLiteral("frame_interval_p95_ms"), percentile(intervals, .95)},
                           {QStringLiteral("frame_interval_p99_ms"), percentile(intervals, .99)},
                           {QStringLiteral("missed_callback_intervals"), missed},
                           {QStringLiteral("columns"), columns},
                           {QStringLiteral("rows"), rows},
                           {QStringLiteral("opacity"), opacity},
                           {QStringLiteral("requested_alpha_buffer_bits"), window->format().alphaBufferSize()},
                           {QStringLiteral("window_color_alpha"), window->color().alphaF()},
                           {QStringLiteral("accessibility_active"), QAccessible::isActive()},
                           {QStringLiteral("voiceover_running"), macTestVoiceOverEnabled()},
                           {QStringLiteral("separate_output_window"), separateWindow},
                           {QStringLiteral("physical_presentation_measured"), false}};
        if (producer) {
            const bool completed = awaitGui(
                [&] {
                    return !readJson(back.filePath(QStringLiteral("result.json"))).isEmpty();
                },
                10000);
            if (!completed) {
                result.insert(QStringLiteral("producer_error"), readJson(back.filePath(QStringLiteral("error.json"))));
                result.insert(QStringLiteral("producer_ready"), readJson(back.filePath(QStringLiteral("ready.json"))));
                result.insert(QStringLiteral("producer_started"), readJson(back.filePath(QStringLiteral("started.json"))));
                result.insert(QStringLiteral("producer_finished"), producer->isFinished());
                record(QStringLiteral("interactive-failed"), result);
            }
            QVERIFY(completed);
            result.insert(QStringLiteral("background"), readJson(back.filePath(QStringLiteral("result.json"))));
        }
        record(QStringLiteral("interactive"), result);
        touch(front.filePath(QStringLiteral("stop")));
        touch(back.filePath(QStringLiteral("stop")));
        if (separateWindow && producerWindow)
            windows.sessions(producerWindow)->closeAll(true);
        windows.sessions(window)->closeAll(true);
        QVERIFY(awaitGui(
            [&] {
                return windows.count() == 0;
            },
            5000));
        QAccessible::setActive(false);
    }

    void retainedMemory()
    {
        QVERIFY(!Kwi::traceEnabled());
        Kwi::WindowManager windows{m_settings.get(), m_presets.get(), m_shortcuts.get(), m_palettes.get()};
        auto *window = windows.launch(command({QStringLiteral("/bin/sh"), QStringLiteral("-i")}));
        QVERIFY(window && activateMacTestWindow(window, 10000));
        auto *model = windows.sessions(window);
        QVERIFY(fit(window, model->currentSession(), 120, 40));
        auto options = command({QStringLiteral("/bin/sh"), QStringLiteral("-c"), QStringLiteral("printf KWI_MEMORY_FIXED")});
        options.newTab = true;
        QJsonArray samples;
        for (int batch{0}; batch < 6; ++batch) {
            for (int n{0}; n < 100; ++n) {
                // QtTest keeps this whole function inside an outer native pool.
                // Match QApplication::exec's per-event autorelease lifetime;
                // otherwise NSProcessInfo environment snapshots accumulate in
                // the observer even though normal app launches release them.
                QMacAutoReleasePool autoreleasePool;
                QVERIFY(windows.launch(options));
                auto *session = model->currentSession();
                QVERIFY(awaitGui(
                    [&] {
                        return session->isFinished();
                    },
                    5000));
                QVERIFY(session->historyText().contains(QLatin1String("KWI_MEMORY_FIXED")));
                model->closeSession(model->currentIndex());
                QVERIFY(awaitGui(
                    [&] {
                        return model->count() == 1;
                    },
                    5000));
            }
            settle(3000);
            auto sample = memory();
            sample.insert(QStringLiteral("cycles"), (batch + 1) * 100);
            sample.insert(QStringLiteral("sessions_alive"), model->findChildren<Kwi::TerminalSession *>().size());
            sample.insert(QStringLiteral("window_objects"), window->findChildren<QObject *>().size());
            samples.append(sample);
            const int profilePause = qBound(0, qEnvironmentVariableIntValue("KWI_PERF_PROFILE_PAUSE_MS"), 30000);
            if (profilePause) {
                record(QStringLiteral("retained-memory-checkpoint"), {{QStringLiteral("samples"), samples}});
                settle(profilePause); // Optional heap inspection only; off for standard measurements.
            }
        }
        if (auto *engine = qmlEngine(window))
            engine->collectGarbage();
        settle(3000);
        const auto afterGc = memory();
        QPixmapCache::clear();
        settle(3000);
        record(QStringLiteral("retained-memory"),
               {{QStringLiteral("samples"), samples},
                {QStringLiteral("after_explicit_qml_gc_diagnostic_only"), afterGc},
                {QStringLiteral("after_explicit_pixmap_cache_clear_diagnostic_only"), memory()},
                {QStringLiteral("pixmap_cache_limit_kib"), QPixmapCache::cacheLimit()}});
        model->closeAll(true);
        QVERIFY(awaitGui(
            [&] {
                return windows.count() == 0;
            },
            5000));
    }
    void streamRetention_data()
    {
        QTest::addColumn<bool>("images");
        QTest::addColumn<int>("history");
        QTest::newRow("history-finite") << false << 100000;
        QTest::newRow("history-unlimited") << false << -1;
        QTest::newRow("image-replacement") << true << 100000;
    }

    void streamRetention()
    {
        QFETCH(bool, images);
        QFETCH(int, history);
        m_settings->setScrollbackLines(history);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QString workload{QStringLiteral(KWI_TEST_WORKLOADS "/history-100000.txt")};
        int rate{2097152};
        if (images) {
            QByteArray payload;
            int packetSize{0};
            for (int n{0}; n < 8; ++n) {
                QByteArray rgba;
                for (int pixel{0}; pixel < 256; ++pixel) {
                    rgba.append(char(n * 32));
                    rgba.append(char(255 - n * 32));
                    rgba.append(char(64));
                    rgba.append(char(255));
                }
                const QByteArray packet{QByteArray("\x1b[H\x1b_Ga=T,f=32,s=16,v=16,i=1,p=1,c=8,r=4,q=2;") + rgba.toBase64() + QByteArray("\x1b\\")};
                if (packetSize)
                    QCOMPARE(packet.size(), packetSize);
                packetSize = packet.size();
                payload += packet;
            }
            workload = directory.filePath(QStringLiteral("rgba-replacement.bin"));
            QFile file{workload};
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(payload), payload.size());
            rate = packetSize * 60; // One complete, fixed-size image per tick.
        }
        QFile source{workload};
        QVERIFY(source.open(QIODevice::ReadOnly));
        const auto sourceHash = QCryptographicHash::hash(source.readAll(), QCryptographicHash::Sha256).toHex();
        QStringList arguments{QStringLiteral("--workload"),
                              workload,
                              QStringLiteral("--seconds"),
                              QStringLiteral("20"),
                              QStringLiteral("--rate"),
                              QString::number(rate)};
        arguments.append(images ? QStringLiteral("--keep-viewport") : QStringLiteral("--seed"));
        Kwi::WindowManager windows{m_settings.get(), m_presets.get(), m_shortcuts.get(), m_palettes.get()};
        auto child = driver(directory.path(), arguments);
        if (history < 0) {
            QStringList shell{
                QStringLiteral("/bin/sh"),
                QStringLiteral("-c"),
                QStringLiteral("printf 'KWI_HISTORY_ONCE_ANCHOR\\n'; n=0; while [ \"$n\" -lt 50 ]; do printf '\\n'; n=$((n+1)); done; exec \"$@\""),
                QStringLiteral("kwi-history")};
            shell.append(child);
            child = shell;
        }
        auto *window = windows.launch(command(child));
        QVERIFY(window && activateMacTestWindow(window, 10000));
        QCOMPARE(window->rendererInterface()->graphicsApi(), QSGRendererInterface::Metal);
        auto *session = windows.sessions(window)->currentSession();
        QVERIFY(fit(window, session, 120, 40));
        QVERIFY(awaitGui(
            [&] {
                return QFile::exists(directory.filePath(QStringLiteral("ready.json")));
            },
            10000));
        touch(directory.filePath(QStringLiteral("start")));
        const auto started = directory.filePath(images ? QStringLiteral("started.json") : QStringLiteral("seeded.json"));
        QVERIFY(awaitGui(
            [&] {
                return QFile::exists(started);
            },
            10000));
        const bool anchorInitiallyRetained = history < 0 && session->historyText().contains(QLatin1String("KWI_HISTORY_ONCE_ANCHOR"));
        QJsonArray samples;
        for (int sample{0}; sample <= 4; ++sample) {
            if (sample)
                settle(5000);
            GhosttyQt::Frame frame;
            QVERIFY(session->snapshot(frame));
            auto point = memory();
            point.insert(QStringLiteral("nominal_seconds"), sample * 5);
            point.insert(QStringLiteral("total_rows"), qint64(frame.scrollTotal));
            point.insert(QStringLiteral("visible_images"), frame.images.size());
            samples.append(point);
        }
        QVERIFY2(awaitGui(
                     [&] {
                         return QFile::exists(directory.filePath(QStringLiteral("result.json")));
                     },
                     10000),
                 qPrintable(QString::fromUtf8(QJsonDocument{readJson(directory.filePath(QStringLiteral("error.json")))}.toJson())));
        GhosttyQt::Frame final;
        QVERIFY(session->snapshot(final));
        if (images) {
            QCOMPARE(final.images.size(), 1);
            QCOMPARE(final.images.first().image.size(), QSize(16, 16));
            const auto color = final.images.first().image.pixelColor(0, 0);
            QVERIFY(color.red() % 32 == 0 && color.red() <= 224);
            QCOMPARE(color.green(), 255 - color.red());
            QCOMPARE(color.blue(), 64);
            QCOMPARE(color.alpha(), 255);
        } else if (history > 0)
            QVERIFY(final.scrollTotal <= 100040);
        else
            QVERIFY(final.scrollTotal > 100040);
        record(QStringLiteral("stream-retention"),
               {{QStringLiteral("samples"), samples},
                {QStringLiteral("history_lines"), history},
                {QStringLiteral("unique_anchor_initially_retained"), anchorInitiallyRetained},
                {QStringLiteral("unique_anchor_finally_retained"), history < 0 && session->historyText().contains(QLatin1String("KWI_HISTORY_ONCE_ANCHOR"))},
                {QStringLiteral("source_sha256"), QString::fromLatin1(sourceHash)},
                {QStringLiteral("producer"), readJson(directory.filePath(QStringLiteral("result.json")))}});
        touch(directory.filePath(QStringLiteral("stop")));
        windows.sessions(window)->closeAll(true);
        QVERIFY(awaitGui(
            [&] {
                return windows.count() == 0;
            },
            5000));
        m_settings->setScrollbackLines(100000);
    }

    void historyUnderOutput()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        Kwi::WindowManager windows{m_settings.get(), m_presets.get(), m_shortcuts.get(), m_palettes.get()};
        auto options = command(driver(directory.path(),
                                      {QStringLiteral("--workload"),
                                       QStringLiteral(KWI_TEST_WORKLOADS "/history-100000.txt"),
                                       QStringLiteral("--seed"),
                                       QStringLiteral("--seconds"),
                                       QStringLiteral("20"),
                                       QStringLiteral("--rate"),
                                       QStringLiteral("2097152")}));
        auto *window = windows.launch(options);
        QVERIFY(window && activateMacTestWindow(window, 10000));
        auto *session = windows.sessions(window)->currentSession();
        auto *view = window->findChild<Kwi::TerminalView *>();
        QVERIFY(view && fit(window, session, 120, 40));
        QVERIFY(awaitGui(
            [&] {
                return !readJson(directory.filePath(QStringLiteral("ready.json"))).isEmpty();
            },
            10000));
        touch(directory.filePath(QStringLiteral("start")));
        QVERIFY(awaitGui(
            [&] {
                return !readJson(directory.filePath(QStringLiteral("seeded.json"))).isEmpty();
            },
            10000));
        QJsonArray heartbeat;
        QTimer observer;
        observer.setTimerType(Qt::PreciseTimer);
        qint64 previous{Kwi::traceTime()};
        connect(&observer, &QTimer::timeout, this, [&] {
            const auto stamp{Kwi::traceTime()};
            heartbeat.append(qMax(0.0, (stamp - previous) / 1e6 - 20));
            previous = stamp;
        });
        observer.start(20);
        QElapsedTimer elapsed;
        elapsed.start();
        view->find(QStringLiteral("needle-996"));
        const bool searchCompleted = awaitGui(
            [&] {
                return !view->isSearchBusy();
            },
            5000);
        const auto searchMs = elapsed.elapsed();
        view->clearSearch();
        QVERIFY(!view->isSearchBusy());
        bool completed{false};
        int error{-1};
        QString errorText;
        const auto output = directory.filePath(QStringLiteral("streamed output é.txt"));
        auto *job = new Kwi::OutputSaveJob{session->terminal(), output, this};
        connect(job, &KJob::result, this, [&](KJob *finished) {
            error = finished->error();
            errorText = finished->errorText();
            completed = true;
        });
        elapsed.restart();
        job->start();
        QVERIFY(awaitGui(
            [&] {
                return completed;
            },
            10000));
        const auto exportMs = elapsed.elapsed();
        // Finite history may evict a tracked row while saving. Its existing
        // defined outcome is an atomic failure, never a partial output file.
        if (error)
            QVERIFY(!QFile::exists(output));
        else {
            QFile saved{output};
            QVERIFY(saved.open(QIODevice::ReadOnly));
            QVERIFY(saved.readAll().contains("KWI_HISTORY_"));
        }
        GhosttyQt::Frame frame;
        QVERIFY(session->snapshot(frame));
        observer.stop();
        record(QStringLiteral("history-under-output"),
               {{QStringLiteral("search_completed"), searchCompleted},
                {QStringLiteral("search_or_cancel_ms"), searchMs},
                {QStringLiteral("export_ms"), exportMs},
                {QStringLiteral("export_error"), error},
                {QStringLiteral("export_error_text"), errorText},
                {QStringLiteral("retained_total_rows"), qint64(frame.scrollTotal)},
                {QStringLiteral("heartbeat_lateness_ms"), heartbeat},
                {QStringLiteral("heartbeat_p95_late_ms"), percentile(heartbeat, .95)},
                {QStringLiteral("producer_still_active"), !QFile::exists(directory.filePath(QStringLiteral("result.json")))}});
        touch(directory.filePath(QStringLiteral("stop")));
        windows.sessions(window)->closeAll(true);
        QVERIFY(awaitGui(
            [&] {
                return windows.count() == 0;
            },
            5000));
    }
};

int main(int argc, char **argv)
{
    if (argc == 3 && QByteArrayView{argv[1]} == QByteArrayView{"--process-metrics"}) {
        rusage_info_v4 usage{};
        if (proc_pid_rusage(QByteArray(argv[2]).toInt(), RUSAGE_INFO_V4, reinterpret_cast<rusage_info_t *>(&usage)) != 0)
            return 1;
        const auto json = QJsonDocument{QJsonObject{{QStringLiteral("physical_footprint"), qint64(usage.ri_phys_footprint)},
                                                    {QStringLiteral("rss"), qint64(usage.ri_resident_size)}}}
                              .toJson(QJsonDocument::Compact);
        fprintf(stdout, "%s\n", json.constData());
        return 0;
    }
    recordMacTestProcess();
    (void)kwi_current_drawable_sequence(); // Keep the opt-in test library linked.
    QStandardPaths::setTestModeEnabled(true);
    KIconTheme::initTheme();
    BreezeIcons::initIcons();
    QQuickWindow::setDefaultAlphaBuffer(true); // Match the standalone application.
    QApplication app{argc, argv};
    QCoreApplication::setApplicationName(QStringLiteral("kwi-macos-performance"));
    app.setQuitOnLastWindowClosed(false);
    app.setProperty("kwiTrace", qEnvironmentVariableIntValue("KWI_PERF_TRACE") != 0);
    app.setProperty("kwiTraceLevel", 2);
    MacPerformanceTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "MacPerformanceTest.moc"
