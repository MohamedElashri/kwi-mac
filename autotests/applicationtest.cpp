// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "DesktopIntegration.h"
#include "SessionModel.h"
#include "SettingsTestSupport.h"
#include "TerminalSession.h"
#include "TerminalView.h"
#include "WindowManager.h"
#include "settings/TerminalPaletteModel.h"
#include "settings/TerminalPresets.h"
#include "settings/TerminalSettings.h"
#include "ui/AppShortcuts.h"

#include <KLocalizedString>
#include <KirigamiAppDefaults>
#include <QApplication>
#include <QByteArrayView>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QObject>
#include <QPointer>
#include <QQuickItem>
#include <QQuickWindow>
#include <QStringList>
#include <QStringLiteral>
#include <QTemporaryDir>
#include <QTest>
#include <QWindow>

using namespace Kwi;

namespace
{
#ifdef Q_OS_MACOS
constexpr auto applicationModifiers = Qt::ControlModifier;
constexpr auto tabModifiers = Qt::ControlModifier;
#else
constexpr auto applicationModifiers = Qt::ControlModifier | Qt::ShiftModifier;
constexpr auto tabModifiers = Qt::AltModifier;
#endif

void typeText(QWindow *window, QByteArrayView text)
{
    for (const char character : text) {
        QTest::keyClick(window, character);
    }
}

class ApplicationFixture
{
public:
    ApplicationFixture()
    {
        resetSettingsDefaults(settings);
    }

    static LaunchOptions shell()
    {
        return {.launch = {}, .presetName = QStringLiteral("Test shell")};
    }

    bool prepareShell()
    {
        return presets.savePreset(
            QStringLiteral("Test shell"),
            {.command = {QStringLiteral("/bin/sh"), QStringLiteral("-i")}, .environment = {QStringLiteral("ENV="), QStringLiteral("BASH_ENV=")}});
    }

    TerminalSettings settings;
    TerminalPresets presets{&settings};
    AppShortcuts shortcuts;
    TerminalPaletteModel palettes{&settings};
    WindowManager windows{&settings, &presets, &shortcuts, &palettes};
};
}

class ApplicationTest : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

private Q_SLOTS:
    void keyboardTabsAndSearch();
    void detachPreservesSession();
};

// Keyboard shortcuts open, switch and close tabs and return focus from search to the terminal.
void ApplicationTest::keyboardTabsAndSearch()
{
    ApplicationFixture fixture;
    QVERIFY(fixture.prepareShell());
    QPointer<QQuickWindow> window{fixture.windows.launch(fixture.shell())};
    QVERIFY(window);
    QVERIFY(QTest::qWaitForWindowExposed(window));
    window->requestActivate();
    QVERIFY(QTest::qWaitForWindowActive(window));
    auto *sessions = fixture.windows.sessions(window);
    QVERIFY(sessions);
    QTRY_VERIFY(sessions->currentSession()->isActive());
    QPointer<TerminalSession> first{sessions->currentSession()};
    QTRY_VERIFY(qobject_cast<TerminalView *>(window->activeFocusItem()));
    auto *view = qobject_cast<TerminalView *>(window->activeFocusItem());
    QCOMPARE(view->session(), first.data());

    typeText(window, "printf '\\nKEYBOARD_READY\\n'");
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_VERIFY(first->historyText().contains(QStringLiteral("\nKEYBOARD_READY\n")));

    QTest::keyClick(window, Qt::Key_F, applicationModifiers);
    QTRY_VERIFY(window->activeFocusItem() && window->activeFocusItem() != view);
    typeText(window, "KEYBOARD_READY");
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_VERIFY(view->searchMatchCount() > 0);
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_COMPARE(window->activeFocusItem(), view);
    QCOMPARE(view->searchMatchCount(), -1);

    QTest::keyClick(window, Qt::Key_T, applicationModifiers);
    QTRY_COMPARE(sessions->count(), 2);
    QTRY_VERIFY(sessions->currentSession()->isActive());
    QPointer<TerminalSession> second{sessions->currentSession()};
    QVERIFY(first != second);
    QTest::keyClick(window, Qt::Key_1, tabModifiers);
    QTRY_COMPARE(sessions->currentSession(), first.data());
    QTest::keyClick(window, Qt::Key_2, tabModifiers);
    QTRY_COMPARE(sessions->currentSession(), second.data());
    QTest::keyClick(window, Qt::Key_W, applicationModifiers);
    QTRY_COMPARE(sessions->count(), 1);
    QTRY_VERIFY(second.isNull());
    QCOMPARE(sessions->currentSession(), first.data());
    QVERIFY(first->historyText().contains(QStringLiteral("\nKEYBOARD_READY\n")));

    // Closing the last tab can destroy the window before keyClick releases its modifiers.
    QTest::keyPress(window, Qt::Key_W, applicationModifiers);
    QTRY_COMPARE(fixture.windows.count(), 0);
    QTRY_VERIFY(window.isNull());
    QTRY_VERIFY(first.isNull());
}

// A detached session keeps its process and history and accepts input after its original window closes.
void ApplicationTest::detachPreservesSession()
{
    ApplicationFixture fixture;
    QVERIFY(fixture.prepareShell());
    auto options = fixture.shell();
    QPointer<QQuickWindow> first{fixture.windows.launch(options)};
    QVERIFY(first);
    QVERIFY(QTest::qWaitForWindowExposed(first));
    auto *source = fixture.windows.sessions(first);
    QVERIFY(source);
    QTRY_VERIFY(source->currentSession()->isActive());
    QPointer<TerminalSession> original{source->currentSession()};
    original->send("printf '\\nBEFORE_DETACH\\n'\n");
    QTRY_VERIFY(original->historyText().contains(QStringLiteral("\nBEFORE_DETACH\n")));
    const auto pid = original->processId();

    options.newTab = true;
    QCOMPARE(fixture.windows.launch(options), first.data());
    QCOMPARE(source->count(), 2);
    source->detachSession(0);
    QTRY_COMPARE(fixture.windows.count(), 2);
    QCOMPARE(source->count(), 1);
    QQuickWindow *detached{nullptr};

    const auto windows = QGuiApplication::allWindows();

    for (auto *window : windows) {
        auto *quick = qobject_cast<QQuickWindow *>(window);

        if (quick && quick != first && fixture.windows.sessions(quick)) {
            detached = quick;
            break;
        }
    }

    QVERIFY(detached);
    QVERIFY(QTest::qWaitForWindowExposed(detached));
    auto *destination = fixture.windows.sessions(detached);
    QCOMPARE(destination->currentSession(), original.data());
    QCOMPARE(original->processId(), pid);
    QVERIFY(original->historyText().contains(QStringLiteral("\nBEFORE_DETACH\n")));
    source->closeAll(true);
    QTRY_COMPARE(fixture.windows.count(), 1);
    QVERIFY(original->sendInput(QStringLiteral("printf '\\nAFTER_DETACH\\n'\n")));
    QTRY_VERIFY(original->historyText().contains(QStringLiteral("\nAFTER_DETACH\n")));

    destination->closeAll(true);
    QTRY_COMPARE(fixture.windows.count(), 0);
    QTRY_VERIFY(original.isNull());
}

int main(int argc, char **argv)
{
    QTemporaryDir directory{QDir{QDir::tempPath()}.canonicalPath() + QStringLiteral("/applicationtest-XXXXXX")};

    if (!directory.isValid()) {
        return 1;
    }

    qputenv("XDG_CONFIG_HOME", QFile::encodeName(directory.path()));
    qputenv("XDG_DATA_HOME", QFile::encodeName(directory.filePath(QStringLiteral("data"))));
    qputenv("XDG_STATE_HOME", QFile::encodeName(directory.filePath(QStringLiteral("state"))));
    qputenv("XDG_CACHE_HOME", QFile::encodeName(directory.filePath(QStringLiteral("cache"))));
    qputenv("SHELL", "/bin/sh");
    QQuickWindow::setDefaultAlphaBuffer(true);
    QApplication application{argc, argv};
    KirigamiAppDefaults::apply(&application);
    application.setQuitOnLastWindowClosed(false);
    KLocalizedString::setApplicationDomain("kwi");
    DesktopIntegration::initializeApplicationIcon();
    ApplicationTest test;

    return QTest::qExec(&test, argc, argv);
}

#include "applicationtest.moc"
