// SPDX-FileCopyrightText: 2026 Kwi contributors
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "DesktopIntegration.h"
#include "MacGuiTestSupport.h"
#include "OutputSaveJob.h"
#include "SessionModel.h"
#include "TerminalSearch.h"
#include "TerminalSession.h"
#include "TerminalView.h"
#include "WindowManager.h"
#include "settings/PresetEditor.h"
#include "settings/TerminalPaletteModel.h"
#include "settings/TerminalPresets.h"
#include "settings/TerminalSettings.h"
#include "ui/AppShortcuts.h"
#include "ui/WebShortcutModel.h"
#include <BreezeIcons>
#include <KConfigGroup>
#include <KIconTheme>
#include <KLocalizedQmlContext>
#include <KLocalizedString>
#include <KNotification>
#include <KSharedConfig>
#include <KirigamiAppDefaults>
#include <QApplication>
#include <QDesktopServices>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QUrlQuery>
#include <Terminal.h>
#include <cstring>

namespace
{
QString normalized(const QString &value)
{
    return QFileInfo(value).canonicalFilePath().normalized(QString::NormalizationForm_C);
}

bool writeJson(const QString &path, const QJsonObject &object)
{
    QFile file{path};
    return file.open(QIODevice::WriteOnly | QIODevice::NewOnly) && file.write(QJsonDocument{object}.toJson()) > 0;
}

int persistentState(const QString &output, bool managed)
{
    if (managed) {
        const auto path = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
        QDir{}.mkpath(path);
        QFile file{path + QStringLiteral("/kwiterminalrc")};
        if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
            return 2;
        file.write("[Terminal]\nScrollbackLines[$i]=123\n[Appearance]\nUseSystemFont[$i]=false\nFontFamily[$i]=Menlo\n");
    }
    Kwi::TerminalSettings settings;
    Kwi::TerminalPresets presets{&settings};
    if (managed) {
        settings.setScrollbackLines(999);
        settings.setFontFamily(QStringLiteral("Courier"));
        settings.resetTerminalDefaults();
    }
    const auto selected = presets.preset(presets.defaultPresetName());
    return writeJson(output,
                     {{QStringLiteral("font"), settings.fontFamily()},
                      {QStringLiteral("opacity"), settings.backgroundOpacity()},
                      {QStringLiteral("history"), settings.scrollbackLines()},
                      {QStringLiteral("immutable_history"), settings.isScrollbackLinesImmutable()},
                      {QStringLiteral("option_as_alt"), settings.optionAsAlt()},
                      {QStringLiteral("scheme"), settings.colorScheme()},
                      {QStringLiteral("preset"), presets.defaultPresetName()},
                      {QStringLiteral("preset_command"), selected ? QJsonArray::fromStringList(selected->launch.command) : QJsonArray{}},
                      {QStringLiteral("config_directory"), QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)}})
        ? 0
        : 2;
}
}

class MacCompatibilityTest : public QObject
{
    Q_OBJECT
    std::unique_ptr<Kwi::TerminalSettings> m_settings;
    std::unique_ptr<Kwi::TerminalPresets> m_presets;
    std::unique_ptr<Kwi::AppShortcuts> m_shortcuts;
    std::unique_ptr<Kwi::TerminalPaletteModel> m_palettes;
    QList<QUrl> m_opened;

    QJsonObject readState(const QString &output, const QString &home = {})
    {
        QProcess child;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.remove(QStringLiteral("KWI_GUI_TEST_PID_FILE"));
        if (!home.isEmpty()) {
            QDir{}.mkpath(home);
            environment.insert(QStringLiteral("HOME"), home);
            environment.insert(QStringLiteral("CFFIXED_USER_HOME"), home);
        }
        child.setProcessEnvironment(environment);
        child.start(QCoreApplication::applicationFilePath(), {home.isEmpty() ? QStringLiteral("--persisted") : QStringLiteral("--managed"), output});
        if (!child.waitForFinished(10000) || child.exitCode() != 0)
            return {};
        QFile file{output};
        if (!file.open(QIODevice::ReadOnly))
            return {};
        return QJsonDocument::fromJson(file.readAll()).object();
    }

private Q_SLOTS:
    void opened(const QUrl &url)
    {
        m_opened.append(url);
    }
    void init()
    {
        activateMacTestApplication();
    }

    void initTestCase()
    {
        QApplication::setQuitOnLastWindowClosed(false);
        KirigamiAppDefaults::apply(qApp);
        m_settings = std::make_unique<Kwi::TerminalSettings>();
        m_presets = std::make_unique<Kwi::TerminalPresets>(m_settings.get());
        m_shortcuts = std::make_unique<Kwi::AppShortcuts>();
        m_palettes = std::make_unique<Kwi::TerminalPaletteModel>(m_settings.get());
    }

    void cleanup()
    {
        m_presets->setDefaultPreset(Kwi::TerminalPresets::builtinPresetName());
        for (const auto &entry : m_presets->entries()) {
            const auto preset = entry.toMap();
            if (preset.value(QStringLiteral("editable")).toBool())
                m_presets->removePreset(preset.value(QStringLiteral("name")).toString());
        }
        m_settings->resetAppearanceDefaults();
        m_settings->resetTerminalDefaults();
        m_settings->resetBehaviorDefaults();
        m_settings->save();
    }

    void presetsSettingsAndRelaunch()
    {
        QTemporaryDir directory{QDir::tempPath() + QStringLiteral("/kwi preset-é-XXXXXX")};
        QVERIFY(directory.isValid());
        m_settings->setFontFamily(QStringLiteral("Menlo"));
        m_settings->setFontPointSize(11);
        m_settings->setBackgroundOpacity(.8);
        m_settings->setScrollbackLines(321);
        m_settings->setOptionAsAlt(true);
        QVERIFY(m_settings->save());
        Kwi::LaunchSettings launch;
        launch.command = {QStringLiteral("/bin/sh"),
                          QStringLiteral("-c"),
                          QStringLiteral("printf 'KWI_PRESET_READY:%s\\n' \"$KWI_VALUE\"; exec /bin/sleep 20")};
        launch.directoryMode = Kwi::LaunchSettings::DirectoryMode::Custom;
        launch.directory = directory.path();
        launch.environment = {QStringLiteral("KWI_VALUE=literal = spaced ; $HOME")};
        const QString name{QStringLiteral("Phase 3-é")};
        QVERIFY(m_presets->savePreset(name, launch));
        QVERIFY(m_presets->setDefaultPreset(name));
        const auto state = readState(directory.filePath(QStringLiteral("persisted.json")));
        QVERIFY(!state.isEmpty());
        QCOMPARE(state.value(QStringLiteral("font")).toString(), QStringLiteral("Menlo"));
        QCOMPARE(state.value(QStringLiteral("history")).toInt(), 321);
        QVERIFY(state.value(QStringLiteral("option_as_alt")).toBool());
        QCOMPARE(state.value(QStringLiteral("opacity")).toDouble(), .8);
        QCOMPARE(state.value(QStringLiteral("preset")).toString(), name);
        QCOMPARE(state.value(QStringLiteral("preset_command")).toArray(), QJsonArray::fromStringList(launch.command));

        const auto saved = m_presets->preset(name);
        QVERIFY(saved && saved->editable && QFile::exists(saved->path));
        Kwi::SessionModel sessions{m_settings.get(), m_presets.get()};
        const auto close = qScopeGuard([&] {
            sessions.closeAll(true);
        });
        QVERIFY(sessions.launchSession({}));
        auto *active = sessions.currentSession();
        QTRY_VERIFY_WITH_TIMEOUT(active->historyText().contains(QLatin1String("KWI_PRESET_READY:literal = spaced ; $HOME")), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(normalized(active->workingDirectory()), normalized(directory.path()), 5000);
        const auto pid = active->processId();
        QVERIFY(pid > 0);
        {
            KConfig external{saved->path, KConfig::SimpleConfig};
            KConfigGroup(&external, QStringLiteral("General")).writeEntry("Name", QStringLiteral("Externally renamed"));
            QVERIFY(external.sync());
        }
        QTRY_VERIFY_WITH_TIMEOUT(m_presets->preset(QStringLiteral("Externally renamed")).has_value(), 5000);
        QCOMPARE(m_presets->defaultPresetName(), QStringLiteral("Externally renamed"));
        QVERIFY(m_presets->updatePreset(QStringLiteral("Externally renamed"), name, launch));
        QVERIFY(!m_presets->importPreset(QUrl::fromLocalFile(saved->path))); // duplicate storage/name
        QVERIFY(!m_presets->savePreset(QStringLiteral("System Shell"), launch));
        auto invalid = launch;
        invalid.environment = {QStringLiteral("1INVALID=value")};
        QVERIFY(!m_presets->savePreset(QStringLiteral("Invalid preset"), invalid));
        QVERIFY(!m_presets->removePreset(QStringLiteral("System Shell")));
        QVERIFY(m_presets->removePreset(name));
        QCOMPARE(m_presets->defaultPresetName(), Kwi::TerminalPresets::builtinPresetName());
        QCOMPARE(active->currentPresetName(), Kwi::TerminalPresets::builtinPresetName());
        QCOMPARE(active->processId(), pid);
        const auto managed = readState(directory.filePath(QStringLiteral("managed.json")), directory.filePath(QStringLiteral("managed-home")));
        QVERIFY(!managed.isEmpty());
        QVERIFY(managed.value(QStringLiteral("immutable_history")).toBool());
        QCOMPARE(managed.value(QStringLiteral("history")).toInt(), 123);
        QCOMPARE(managed.value(QStringLiteral("font")).toString(), QStringLiteral("Menlo"));
        m_settings->resetAppearanceDefaults();
        m_settings->resetTerminalDefaults();
        QVERIFY(m_settings->save());
    }

    void presetEditorSaveCopyAndDiscard()
    {
        Kwi::PresetEditor editor{m_presets.get()};
        QVERIFY(editor.load(Kwi::TerminalPresets::builtinPresetName()));
        editor.newCopy();
        editor.setName(QStringLiteral("Copied preset"));
        editor.setCommand(QStringLiteral("/usr/bin/printf '%s' 'literal ; $HOME'"));
        QVERIFY(editor.save());
        const auto saved = m_presets->preset(QStringLiteral("Copied preset"));
        QVERIFY(saved);
        QCOMPARE(saved->launch.command, (QStringList{QStringLiteral("/usr/bin/printf"), QStringLiteral("%s"), QStringLiteral("literal ; $HOME")}));
        editor.setCommand(QStringLiteral("/bin/false"));
        QVERIFY(editor.isDirty());
        editor.discard();
        QVERIFY(!editor.isDirty());
        QCOMPARE(m_presets->preset(QStringLiteral("Copied preset"))->launch.command, saved->launch.command);
        editor.setName(QStringLiteral("Renamed preset"));
        QVERIFY(editor.save());
        QVERIFY(!m_presets->preset(QStringLiteral("Copied preset")));
        QVERIFY(m_presets->preset(QStringLiteral("Renamed preset")));
        QVERIFY(editor.removeCurrent());
        QVERIFY(!m_presets->preset(QStringLiteral("Renamed preset")));
    }

    void paletteRoundTrip()
    {
        QVERIFY(m_palettes->entries().size() >= 14);
        const auto original = m_palettes->schemeData(QStringLiteral("system"));
        QVERIFY(!original.isEmpty());
        const auto ansi = original.value(QStringLiteral("ansi")).toList();
        QCOMPARE(ansi.size(), 16);
        QVERIFY(m_palettes->saveScheme({}, QStringLiteral("Phase 3 colors-é"), QColor{Qt::white}, QColor{Qt::black}, ansi));
        const QString id = m_settings->colorScheme();
        QVERIFY(id.startsWith(QLatin1String("user:")));
        QCOMPARE(m_palettes->schemeData(id).value(QStringLiteral("foreground")).value<QColor>(), QColor{Qt::white});
        QTemporaryDir directory;
        const auto state = readState(directory.filePath(QStringLiteral("palette.json")));
        QCOMPARE(state.value(QStringLiteral("scheme")).toString(), id);
        QVERIFY(m_palettes->saveScheme(id, QStringLiteral("Phase 3 edited"), QColor{Qt::yellow}, QColor{Qt::black}, ansi));
        QCOMPARE(m_palettes->schemeData(id).value(QStringLiteral("foreground")).value<QColor>(), QColor{Qt::yellow});
        const auto source = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/kwi/color-schemes/user/") + id.mid(5);
        const auto importedPath = directory.filePath(QStringLiteral("imported.colorscheme"));
        QVERIFY(QFile::copy(source, importedPath));
        {
            KConfig imported{importedPath, KConfig::SimpleConfig};
            KConfigGroup(&imported, QStringLiteral("General")).writeEntry("Description", QStringLiteral("Imported phase 3"));
            QVERIFY(imported.sync());
        }
        QVERIFY(m_palettes->importScheme(QUrl::fromLocalFile(importedPath)));
        const auto importedId = m_settings->colorScheme();
        QVERIFY(importedId != id);
        QCOMPARE(m_palettes->schemeData(importedId).value(QStringLiteral("foreground")).value<QColor>(), QColor{Qt::yellow});
        QVERIFY(!m_palettes->importScheme(QUrl::fromLocalFile(importedPath))); // duplicate name
        QVERIFY(!m_palettes->removeScheme(QStringLiteral("system")));
        QVERIFY(m_palettes->removeScheme(id));
        QVERIFY(m_palettes->removeScheme(importedId));
        QCOMPARE(m_settings->colorScheme(), QStringLiteral("system"));
    }

    void historyRetentionPreference()
    {
        QQmlEngine engine;
        KLocalization::setupLocalizedContext(&engine);
        QQmlComponent component{&engine};
        component.loadFromModule(QStringLiteral("org.kde.kwi"), QStringLiteral("TerminalSettings"));
        QVERIFY2(!component.isError(), qPrintable(component.errorString()));
        QQuickWindow window;
        std::unique_ptr<QObject> page{component.createWithInitialProperties({{QStringLiteral("appSettings"), QVariant::fromValue(m_settings.get())}})};
        QVERIFY2(page, qPrintable(component.errorString()));
        auto *item = qobject_cast<QQuickItem *>(page.get());
        QVERIFY(item);
        item->setParentItem(window.contentItem());
        item->setSize(QSizeF{650, 550});
        window.resize(650, 550);
        window.show();
        QVERIFY(activateMacTestWindow(&window, 5000));
        auto *notice = page->findChild<QObject *>(QStringLiteral("historyRetentionNotice"));
        auto *mode = page->findChild<QObject *>(QStringLiteral("scrollbackMode"));
        QVERIFY(notice && mode);
        m_settings->setScrollbackLines(-1);
        QTRY_VERIFY(notice->property("visible").toBool());
        QCOMPARE(mode->property("currentIndex").toInt(), 2);
        QCOMPARE(mode->property("currentText").toString(), QStringLiteral("No line limit"));
        QVERIFY(notice->property("text").toString().contains(QLatin1String("discarded")));
        m_settings->setScrollbackLines(100);
        QTRY_VERIFY(!notice->property("visible").toBool());
    }

    void sessionsMoveDetachAndCwd()
    {
        Kwi::WindowManager windows{m_settings.get(), m_presets.get(), m_shortcuts.get(), m_palettes.get()};
        QTemporaryDir directory{QDir::tempPath() + QStringLiteral("/kwi sessions-é-XXXXXX")};
        Kwi::LaunchOptions options;
        options.launch.command = QStringList{QStringLiteral("/bin/sh"), QStringLiteral("-i")};
        options.launch.directory = directory.path();
        options.title = QStringLiteral("Phase 3 custom title");
        auto *first = windows.launch(options);
        QVERIFY(first && activateMacTestWindow(first, 10000));
        auto *source = windows.sessions(first);
        auto *session = source->currentSession();
        QTRY_VERIFY_WITH_TIMEOUT(session->isActive(), 5000);
        QCOMPARE(session->customTitle(), options.title);
        QTRY_COMPARE_WITH_TIMEOUT(normalized(session->workingDirectory()), normalized(directory.path()), 5000);
        const auto pid = session->processId();
        QVERIFY(session->sendInput(QStringLiteral("printf 'KWI_SESSION_BEFORE\\n'\n")));
        QTRY_VERIFY_WITH_TIMEOUT(session->historyText().contains(QLatin1String("KWI_SESSION_BEFORE\n")), 5000);
        source->newSession();
        QCOMPARE(source->count(), 2);
        QTRY_VERIFY_WITH_TIMEOUT(source->currentSession()->isActive(), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(normalized(source->currentSession()->workingDirectory()), normalized(directory.path()), 5000);
        auto *secondSession = source->currentSession();
        source->moveSession(1, 0);
        QCOMPARE(source->currentSession(), secondSession);
        source->setCurrentIndex(source->indexOf(session));
        source->activateLastUsed();
        QCOMPARE(source->currentSession(), secondSession);

        options.title.clear();
        auto *second = windows.launch(options);
        QVERIFY(second && activateMacTestWindow(second, 10000));
        auto *destination = windows.sessions(second);
        const auto token = windows.beginTabDrag(source, session);
        QVERIFY(!token.isEmpty() && windows.acceptsTabDrag(token));
        QVERIFY(!windows.dropTab(QStringLiteral("invalid"), destination, 0));
        QVERIFY(windows.dropTab(token, destination, 0));
        windows.finishTabDrag(token, int(Qt::MoveAction));
        QTRY_VERIFY_WITH_TIMEOUT(destination->indexOf(session) >= 0, 5000);
        QCOMPARE(session->processId(), pid);
        QVERIFY(source->indexOf(session) < 0);
        destination->detachSession(destination->indexOf(session));
        QTRY_COMPARE_WITH_TIMEOUT(windows.count(), 3, 5000);
        QCOMPARE(session->processId(), pid);
        QVERIFY(session->sendInput(QStringLiteral("printf 'KWI_SESSION_AFTER\\n'\n")));
        QTRY_VERIFY_WITH_TIMEOUT(session->historyText().contains(QLatin1String("KWI_SESSION_AFTER\n")), 5000);
        const auto allWindows = QGuiApplication::allWindows();
        for (auto *window : allWindows) {
            if (auto *model = windows.sessions(qobject_cast<QQuickWindow *>(window)))
                model->closeAll(true);
        }
        QTRY_COMPARE_WITH_TIMEOUT(windows.count(), 0, 5000);
    }

    void desktopLinksAndWebProviders()
    {
        Kwi::SessionModel sessions{m_settings.get(), m_presets.get()};
        Kwi::DesktopIntegration desktop{&sessions};
        QDesktopServices::setUrlHandler(QStringLiteral("https"), this, "opened");
        QDesktopServices::setUrlHandler(QStringLiteral("file"), this, "opened");
        const auto cleanup = qScopeGuard([] {
            QDesktopServices::unsetUrlHandler(QStringLiteral("https"));
            QDesktopServices::unsetUrlHandler(QStringLiteral("file"));
        });
        const QUrl link{QStringLiteral("https://example.invalid/search?q=literal%20%26%20%C3%A9")};
        desktop.openUrl(link);
        QTRY_VERIFY_WITH_TIMEOUT(m_opened.contains(link), 5000);
        QTemporaryDir directory;
        const QUrl folder = QUrl::fromLocalFile(directory.path() + QLatin1Char('/'));
        desktop.openUrl(folder);
        QTRY_VERIFY_WITH_TIMEOUT(m_opened.contains(folder), 5000);
        const auto count = m_opened.size();
        desktop.openUrl(QUrl{QStringLiteral("relative/path")});
        QCOMPARE(m_opened.size(), count);
        QVERIFY(!desktop.errorString().isEmpty());
        desktop.openUrl(QUrl{QStringLiteral("https://example.invalid/") + QString(9000, QLatin1Char('x'))});
        QCOMPARE(m_opened.size(), count);
        QVERIFY(!desktop.errorString().isEmpty());
        desktop.openCurrentDirectory(nullptr);
        QVERIFY(!desktop.errorString().isEmpty());
        Kwi::WebShortcutModel providers;
        providers.refresh(QStringLiteral("phase three literal & Unicode-é"));
        QVERIFY(providers.count() > 0);
        const auto url = providers.searchUrl(0);
        QVERIFY(url.isValid() && url.scheme() == QLatin1String("https"));
        QVERIFY(!providers.searchUrl(-1).isValid());
        QVERIFY(!providers.canConfigure()); // no KDE System Settings in this distribution
        const QString literal{QStringLiteral("<b>literal & café</b>")};
        providers.refresh(literal);
        bool exactQuery{false};
        for (int row{0}; row < providers.count(); ++row) {
            const auto candidate = providers.searchUrl(row);
            for (const auto &item : QUrlQuery{candidate}.queryItems(QUrl::FullyDecoded))
                exactQuery |= item.second == literal;
        }
        QVERIFY(exactQuery);
        auto config = KSharedConfig::openConfig(QStringLiteral("kuriikwsfilterrc"));
        KConfigGroup group{config, QStringLiteral("General")};
        const bool enabled = group.readEntry("EnableWebShortcuts", true);
        const auto restore = qScopeGuard([&] {
            group.writeEntry("EnableWebShortcuts", enabled);
            config->sync();
        });
        group.writeEntry("EnableWebShortcuts", false);
        QVERIFY(config->sync());
        providers.refresh(literal);
        QCOMPARE(providers.count(), 0);
    }

    void notificationPolicyAndActivation()
    {
        auto config = KSharedConfig::openConfig(QStringLiteral("kwi.notifyrc"), KConfig::SimpleConfig);
        KConfigGroup(config, QStringLiteral("Event/bell")).writeEntry("Action", QString{});
        QVERIFY(config->sync()); // policy test: no actual OS popup
        Kwi::WindowManager windows{m_settings.get(), m_presets.get(), m_shortcuts.get(), m_palettes.get()};
        Kwi::LaunchOptions options;
        options.launch.command = QStringList{QStringLiteral("/bin/sh"), QStringLiteral("-i")};
        auto *window = windows.launch(options);
        QVERIFY(window && activateMacTestWindow(window, 10000));
        auto *model = windows.sessions(window);
        auto *session = model->currentSession();
        auto *desktop = window->property("desktopIntegration").value<Kwi::DesktopIntegration *>();
        QVERIFY(desktop);
        session->feed("\a");
        QVERIFY(!session->attention());
        desktop->setSessionFocused(false);
        session->feed("\a");
        QVERIFY(session->attention());
        auto *notification = desktop->findChild<KNotification *>();
        QVERIFY(notification && notification->defaultAction());
        session->feed("\a\a");
        QCOMPARE(desktop->findChildren<KNotification *>().size(), 1);
        QSignalSpy activated{desktop, &Kwi::DesktopIntegration::sessionActivationRequested};
        QVERIFY(QMetaObject::invokeMethod(notification->defaultAction(), "activated"));
        QCOMPARE(activated.count(), 1);
        QCOMPARE(model->currentSession(), session);
        desktop->setSessionFocused(true);
        QVERIFY(!session->attention());
        model->closeAll(true);
        QTRY_COMPARE_WITH_TIMEOUT(windows.count(), 0, 5000);
    }

    void historySearchExportAndProgress()
    {
        Kwi::Session session;
        session.resize(80, 10);
        session.setScrollbackLines(100);
        for (int n{0}; n < 5000; ++n)
            session.feed(QStringLiteral("item_%1 Unicode-é\r\n").arg(n, 4, 10, QLatin1Char('0')).toUtf8());
        QVERIFY(!session.historyText().contains(QLatin1String("item_0000")));
        QVERIFY(session.historyText().contains(QLatin1String("item_4999")));
        Kwi::TerminalView view;
        view.setSession(&session);
        view.selectAll();
        QVERIFY(session.selectionText().contains(QStringLiteral("Unicode-é")));
        Kwi::TerminalSearch search;
        search.setSession(&session);
        QSignalSpy applied{&search, &Kwi::TerminalSearch::matchApplied};
        search.find(QStringLiteral("item_4980"));
        QTRY_VERIFY_WITH_TIMEOUT(!search.isBusy() && !applied.isEmpty(), 3000);
        QVERIFY(session.selectionText().contains(QLatin1String("item_4980")));
        search.find(QStringLiteral("no-such-history-value"));
        QTRY_VERIFY_WITH_TIMEOUT(!search.isBusy(), 3000);
        QCOMPARE(search.status(), QStringLiteral("No matches"));
        search.clear();
        QTemporaryDir directory;
        const QString output = directory.filePath(QStringLiteral("output space-é.txt"));
        int error{-1};
        auto *save = new Kwi::OutputSaveJob{session.terminal(), output, this};
        connect(save, &KJob::result, this, [&](KJob *job) {
            error = job->error();
        });
        save->start();
        QTRY_VERIFY_WITH_TIMEOUT(error != -1, 3000);
        QCOMPARE(error, 0);
        QFile file{output};
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(file.readAll().contains(QStringLiteral("item_4999 Unicode-é").toUtf8()));
        int failure{-1};
        auto *invalid = new Kwi::OutputSaveJob{session.terminal(), directory.path(), this};
        connect(invalid, &KJob::result, this, [&](KJob *job) {
            failure = job->error();
        });
        invalid->start();
        QTRY_VERIFY_WITH_TIMEOUT(failure != -1, 3000);
        QVERIFY(failure != 0);
        session.feed("\033]9;4;1;42\a");
        QCOMPARE(session.progressState(), Kwi::Session::ProgressState::Normal);
        QCOMPARE(session.progressPercent(), 42);
        session.feed("\033]9;4;2;50\a");
        QCOMPARE(session.progressState(), Kwi::Session::ProgressState::Error);
        session.feed("\033]9;4;3\a");
        QCOMPARE(session.progressState(), Kwi::Session::ProgressState::Indeterminate);
        session.feed("\033]9;4;4;50\a");
        QCOMPARE(session.progressState(), Kwi::Session::ProgressState::Paused);
        session.feed("\033]0;Phase 3 title\a");
        QCOMPARE(session.applicationTitle(), QStringLiteral("Phase 3 title"));
        session.resetTerminal();
        QCOMPARE(session.progressState(), Kwi::Session::ProgressState::None);
        QVERIFY(!session.hasApplicationTitle());
        session.setScrollbackLines(0);
        session.clearScrollback();
        session.feed("KWI_AFTER_RESET\r\n");
        QVERIFY(session.historyText().contains(QLatin1String("KWI_AFTER_RESET")));
    }

    void exportCancelResizeAndTerminalClose()
    {
        QTemporaryDir directory;
        const auto output = directory.filePath(QStringLiteral("cancel output-é.txt"));
        const QByteArray original{"preserve existing output"};
        {
            QFile file{output};
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(original), original.size());
        }
        auto session = std::make_unique<Kwi::Session>();
        session->resize(80, 10);
        session->setScrollbackLines(10000);
        for (int n{0}; n < 5000; ++n)
            session->feed(QStringLiteral("row_%1 Unicode-é\r\n").arg(n).toUtf8());
        const auto preserved = [&] {
            QFile file{output};
            return file.open(QIODevice::ReadOnly) && file.readAll() == original;
        };
        {
            auto job = std::make_unique<Kwi::OutputSaveJob>(session->terminal(), output);
            job->setAutoDelete(false);
            QSignalSpy result{job.get(), &KJob::result};
            job->start();
            QVERIFY(job->kill(KJob::EmitResult));
            QCOMPARE(result.count(), 1);
            QCOMPARE(job->error(), int(KJob::KilledJobError));
            QVERIFY(preserved());
        }
        {
            auto job = std::make_unique<Kwi::OutputSaveJob>(session->terminal(), output);
            job->setAutoDelete(false);
            QSignalSpy result{job.get(), &KJob::result};
            job->start();
            session->resize(90, 12);
            QTRY_COMPARE_WITH_TIMEOUT(result.count(), 1, 3000);
            QVERIFY(job->error() != 0);
            QVERIFY(job->errorText().contains(QLatin1String("history changed")));
            QVERIFY(preserved());
        }
        {
            auto job = std::make_unique<Kwi::OutputSaveJob>(session->terminal(), output);
            job->setAutoDelete(false);
            QSignalSpy result{job.get(), &KJob::result};
            session.reset();
            job->start();
            QTRY_COMPARE_WITH_TIMEOUT(result.count(), 1, 3000);
            QVERIFY(job->error() != 0);
            QVERIFY(job->errorText().contains(QLatin1String("no longer available")));
            QVERIFY(preserved());
        }
    }

    void readOnlyRepliesAndMonitoring()
    {
        Kwi::Session session;
        Kwi::TerminalLaunch launch;
        launch.program = QStringLiteral("/bin/cat");
        QVERIFY(session.start(launch));
        QTRY_COMPARE_WITH_TIMEOUT(session.state(), Kwi::Session::State::Running, 3000);
        session.setReadOnly(true);
        QVERIFY(!session.sendInput(QStringLiteral("KWI_BLOCKED\n")));
        QVERIFY(!session.pasteApprovedText(QStringLiteral("KWI_BLOCKED_PASTE\n")));
        QSignalSpy replies{session.terminal(), SIGNAL(writeRequested(QByteArray))};
        QVERIFY(replies.isValid());
        session.feed("\033[6n");
        QVERIFY(!replies.isEmpty());
        session.setReadOnly(false);
        QVERIFY(session.sendInput(QStringLiteral("\nKWI_AFTER_READONLY\n")));
        QTRY_VERIFY_WITH_TIMEOUT(session.historyText().contains(QLatin1String("KWI_AFTER_READONLY")), 3000);
        QVERIFY(!session.historyText().contains(QLatin1String("KWI_BLOCKED")));
        QSignalSpy activity{&session, &Kwi::Session::activityDetected};
        QSignalSpy silence{&session, &Kwi::Session::silenceDetected};
        session.setMonitorActivity(true);
        session.setSilenceSeconds(1);
        session.setMonitorSilence(true);
        session.feed("activity one\r\n");
        session.feed("activity two\r\n");
        QCOMPARE(activity.count(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(silence.count(), 1, 2500);
        session.setMonitorActivity(false);
        session.setMonitorSilence(false);
        session.close();
        QTRY_VERIFY_WITH_TIMEOUT(!session.isActive(), 3000);
    }
};

int main(int argc, char **argv)
{
    const bool helper = argc == 3 && (!std::strcmp(argv[1], "--persisted") || !std::strcmp(argv[1], "--managed"));
    if (!helper)
        recordMacTestProcess();
    QStandardPaths::setTestModeEnabled(true);
    KIconTheme::initTheme();
    BreezeIcons::initIcons();
    QQuickWindow::setDefaultAlphaBuffer(true);
    QApplication app{argc, argv};
    QCoreApplication::setApplicationName(QStringLiteral("kwi-macos-compatibility-test"));
    KLocalizedString::setApplicationDomain("kwi");
    if (helper)
        return persistentState(QString::fromLocal8Bit(argv[2]), !std::strcmp(argv[1], "--managed"));
    MacCompatibilityTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "MacCompatibilityTest.moc"
