// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "Part.h"

#include "DesktopIntegration.h"
#include "PresetProperties.h"
#include "Session.h"
#include "SessionModel.h"
#include "TerminalSession.h"
#include "TerminalView.h"
#include "settings/TerminalPresets.h"
#include "settings/TerminalSettings.h"
#include "ui/SettingsDialog.h"
#include "ui/TerminalWidget.h"
#include "ui/WebShortcuts_p.h"

#include <KActionCollection>
#include <KIO/KUriFilterSearchProviderActions>
#include <KLocalizedString>
#include <KMessageBox>
#include <KStandardActions>
#include <KXMLGUIClient>
#include <KXMLGUIFactory>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QFontMetrics>
#include <QIcon>
#include <QInputDialog>
#include <QKeyEvent>
#include <QMenu>
#include <QMessageBox>
#include <QPoint>
#include <QPushButton>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>
#include <QtGlobal>
#include <algorithm>
#include <unistd.h>
#include <utility>

namespace Kwi
{
class Part::Page final : public QWidget
{
public:
    explicit Page(QWidget *parent)
        : QWidget{parent}
        , terminal{new TerminalWidget}
    {
        setFocusPolicy(Qt::StrongFocus);
        auto *contents = new QVBoxLayout(this);
        contents->setContentsMargins(0, 0, 0, 0);
    }

    ~Page() override
    {
        // Delete the detached Quick widget even if reattachment never ran.
        delete terminal;
    }

    void detach()
    {
        if (terminal->parentWidget() != this) {
            return;
        }

        const bool focused{hasFocus() || window()->focusWidget() == terminal};
        setFocusProxy(nullptr);
        layout()->removeWidget(terminal);
        terminal->hide();
        terminal->setParent(nullptr);

        // Yakuake can request focus through this cached pointer while Quick is detached.
        terminal->setFocusProxy(this);

        if (focused) {
            setFocus(Qt::OtherFocusReason);
        }
    }

    void attach()
    {
        if (terminal->parentWidget() == this) {
            return;
        }

        const bool focused{hasFocus() || window()->focusWidget() == this};
        terminal->setFocusProxy(nullptr);
        layout()->addWidget(terminal);
        setFocusProxy(terminal);
        terminal->show();

        if (focused) {
            terminal->setFocus(Qt::OtherFocusReason);
        }
    }

    TerminalWidget *const terminal;
};

namespace
{
class TerminalActionsClient final : public KXMLGUIClient
{
    Q_DISABLE_COPY_MOVE(TerminalActionsClient)

public:
    TerminalActionsClient()
    {
        setComponentName(QStringLiteral("kwipart"), i18nd("kwi", "Kwi Terminal"));

        // Dolphin builds against our QTabWidget, where a MenuBar would cover the terminal.
        setXML(QStringLiteral("<gui name=\"kwipart\" version=\"1\" translationDomain=\"kwi\"/>"));
    }
};
}

Part::Part(QWidget *, QObject *parent, const KPluginMetaData &metadata, const QVariantList &)
    : KParts::ReadOnlyPart{parent, metadata}
    , m_settings{new TerminalSettings(this)}
    , m_presets{new TerminalPresets(m_settings, this)}
    , m_sessions{new SessionModel(m_settings, m_presets, this)}
    , m_desktop{new DesktopIntegration(m_sessions, DesktopIntegration::Mode::Embedded, this)}
    , m_tabs{new QTabWidget}
    , m_terminalClient{std::make_unique<TerminalActionsClient>()}
{
    setComponentName(QStringLiteral("kwipart"), i18nd("kwi", "Kwi Terminal"));
    setWidget(m_tabs);
    m_tabs->setObjectName(QStringLiteral("kwiPartTabs"));
    m_tabs->setDocumentMode(true);
    m_tabs->setTabBarAutoHide(true);
    m_tabs->setTabsClosable(true);
    m_tabs->setMovable(true);
    m_tabs->installEventFilter(this);

    const auto updateTabPosition = [this] {
        m_tabs->setTabPosition(m_settings->tabBarAtBottom() ? QTabWidget::South : QTabWidget::North);
    };

    connect(m_settings, &TerminalSettings::tabBarAtBottomChanged, m_tabs, updateTabPosition);
    updateTabPosition();

    connect(m_tabs, &QObject::destroyed, this, [this] {
        m_destroying = true;
        m_pages.clear();
        m_tabs = nullptr;
    });

    insertChildClient(m_terminalClient.get());
    m_terminalClient->actionCollection()->addAssociatedWidget(m_tabs);
    setupActions();

    connect(m_tabs, &QTabWidget::currentChanged, this, [this](int index) {
        if (!m_syncingTabs) {
            m_sessions->setCurrentIndex(index);
        }
    });

    connect(m_tabs, &QTabWidget::tabCloseRequested, m_sessions, [this](int index) {
        m_sessions->closeSession(index);
    });

    connect(m_tabs->tabBar(), &QTabBar::tabMoved, m_sessions, &SessionModel::moveSession);
    m_tabs->tabBar()->setContextMenuPolicy(Qt::CustomContextMenu);

    connect(m_tabs->tabBar(), &QWidget::customContextMenuRequested, this, [this](QPoint position) {
        const int index{m_tabs->tabBar()->tabAt(position)};

        if (index < 0) {
            return;
        }

        tabContextMenu(m_tabs->tabBar()->mapToGlobal(position), m_pages.key(static_cast<Page *>(m_tabs->widget(index))));
    });

    connect(m_sessions, &SessionModel::sessionAdded, this, &Part::addSessionWidget);
    connect(m_sessions, &SessionModel::sessionRemoved, this, &Part::removeSessionWidget);
    connect(m_sessions, &SessionModel::currentIndexChanged, this, &Part::activateSession);
    connect(m_sessions, &SessionModel::allClosed, this, &QObject::deleteLater);
    connect(m_sessions, &SessionModel::closeConfirmationRequested, this, [this](quint64 requestId, SessionModel::CloseScope scope, const QString &title) {
        if (scope == SessionModel::CloseScope::Session) {
            showCloseConfirmation(requestId, title);
        }
    });

    connect(qApp, &QApplication::focusChanged, this, &Part::updateHostFocus);
    connect(m_desktop, &DesktopIntegration::errorChanged, this, [this] {
        if (activeWidget()) {
            activeWidget()->setHostError(m_desktop->errorString());
        }
    });

    connect(m_desktop, &DesktopIntegration::sessionActivationRequested, this, [this](TerminalSession *session) {
        if (auto *page = m_pages.value(session)) {
            page->setFocus(Qt::OtherFocusReason);
        }

        updateHostFocus();
    });

    // Leave the first session dormant so the host can attach a PTY or choose a program.
    addSession({}, {}, false);
}

Part::~Part()
{
    m_destroying = true;

    // Hosts may disconnect widget signals before teardown, so unplug the clients here.
    if (factory()) {
        factory()->removeClient(this);
    }

    // Release the scene before its sessions, unless the host already deleted widget().
    if (widget()) {
        delete widget();
    }

    m_pages.clear();
    removeChildClient(m_terminalClient.get());
    delete m_desktop;
    delete m_sessions;
}

TerminalSession *Part::activeSession() const
{
    return m_sessions->currentSession();
}

TerminalWidget *Part::activeWidget() const
{
    auto *page = m_pages.value(activeSession());

    return page && page->terminal->terminalView() ? page->terminal : nullptr;
}

void Part::addSession(const QString &presetName, const QString &directory, bool start)
{
    if (!directory.isEmpty() && (!QDir::isAbsolutePath(directory) || directory.contains(QChar::Null))) {
        return;
    }

    auto ownedSession = std::make_unique<TerminalSession>(m_settings, m_presets);

    if (!presetName.isEmpty() && !ownedSession->setCurrentPreset(presetName)) {
        return;
    }

    LaunchRequest request{};
    request.directory = directory;
    request.contextDirectory = currentWorkingDirectory();
    auto *session = ownedSession.release();
    m_launchRequests.insert(session, request);
    m_sessions->adoptSession(session);

    if (start) {
        QTimer::singleShot(0, session, [this, session] {
            startShell(session);
        });
    }
}

void Part::addSessionWidget(TerminalSession *session)
{
    auto *container = new Page(m_tabs);
    auto *page = container->terminal;

    if (!m_embeddingPending) {
        container->attach();
    }

    page->setObjectName(QStringLiteral("kwiPartTerminal"));
    page->installEventFilter(this);
    page->setSession(session);
    m_pages.insert(session, container);
    m_syncingTabs = true;
    m_tabs->insertTab(m_sessions->indexOf(session), container, session->title());
    m_syncingTabs = false;

    connect(
        page,
        &TerminalWidget::overrideShortcut,
        this,
        [this](QKeyEvent *event, bool &override) {
            // Preserve Part shortcuts even when Kate reserves the other terminal keys.
            const QKeySequence pressed{event->keyCombination()};
            const auto actions = m_terminalClient->actionCollection()->actions();

            for (auto *action : actions) {
                if (!action->isEnabled()) {
                    continue;
                }

                const auto shortcuts = action->shortcuts();

                if (std::ranges::any_of(shortcuts, [&pressed](const QKeySequence &shortcut) {
                        return pressed.matches(shortcut) != QKeySequence::NoMatch;
                    })) {
                    override = false;

                    return;
                }
            }

            Q_EMIT overrideShortcut(event, override);
        },
        Qt::DirectConnection);

    connect(page, &TerminalWidget::contextMenuRequested, this, &Part::contextMenu);
    connect(page, &TerminalWidget::linkActivated, m_desktop, &DesktopIntegration::openUrl);
    connect(page, &TerminalWidget::multilinePasteRequested, this, [this](Session *origin, const QString &text) {
        auto *dialog = new QMessageBox(QMessageBox::Warning,
                                       i18nd("kwi", "Paste Multiple Lines?"),
                                       i18nd("kwi", "Pasting this text may run commands."),
                                       QMessageBox::Ok | QMessageBox::Cancel,
                                       widget()->window());
        dialog->setAttribute(Qt::WA_DeleteOnClose);

        connect(this, &QObject::destroyed, dialog, &QObject::deleteLater);
        dialog->setTextFormat(Qt::PlainText);
        dialog->setDetailedText(text.left(2000));
        dialog->setDefaultButton(QMessageBox::Cancel);
        dialog->button(QMessageBox::Ok)->setText(i18nd("kwi", "Paste"));
        auto *doNotAskAgain = new QCheckBox(i18ndc("kwi", "@option:check", "Do not ask again"), dialog);
        doNotAskAgain->setEnabled(!m_settings->isConfirmMultilinePasteImmutable());
        dialog->setCheckBox(doNotAskAgain);
        const QPointer<Session> target{origin};

        connect(dialog, &QMessageBox::finished, this, [this, target, text, doNotAskAgain](int result) {
            if (result == QMessageBox::Ok && target && activeSession() == target) {
                if (doNotAskAgain->isChecked()) {
                    m_settings->setConfirmMultilinePaste(false);
                    m_settings->save();
                }

                target->pasteApprovedText(text);
            }
        });

        dialog->open();
    });

    connect(session, &TerminalSession::titleChanged, this, [this, session, container] {
        if (m_destroying) {
            return;
        }

        m_tabs->setTabText(m_tabs->indexOf(container), session->title());

        if (activeSession() == session) {
            Q_EMIT setWindowCaption(session->title());
        }
    });

    connect(session, &Session::foregroundCommandChanged, this, [this, session] {
        // Kate AppOutput uses this signal to refresh process state even with a fixed caption.
        if (!m_destroying && activeSession() == session) {
            Q_EMIT setWindowCaption(session->title());
        }
    });

    connect(session, &TerminalSession::workingDirectoryChanged, this, [this, session](const QString &directory) {
        if (activeSession() == session) {
            Q_EMIT currentDirectoryChanged(directory);
        }
    });

    connect(session, &TerminalSession::activityDetected, this, [this, session] {
        if (activeSession() == session) {
            Q_EMIT activityDetected();
        }
    });

    connect(session, &TerminalSession::silenceDetected, this, [this, session] {
        if (activeSession() == session) {
            Q_EMIT silenceDetected();
        }
    });

    connect(session, &TerminalSession::monitoringChanged, this, &Part::updateActions);
    const auto updateTabIcon = [this, session, container] {
        if (m_destroying) {
            return;
        }

        m_tabs->setTabIcon(m_tabs->indexOf(container), m_settings->showTabStatusIcons() ? QIcon::fromTheme(session->iconName()) : QIcon{});
    };

    connect(session, &TerminalSession::iconNameChanged, container, updateTabIcon);
    connect(m_settings, &TerminalSettings::showTabStatusIconsChanged, container, updateTabIcon);
    updateTabIcon();

    connect(session, &TerminalSession::readOnlyChanged, this, &Part::updateActions);
    connect(session, &Session::activeChanged, this, &Part::updateActions);

    if (auto *terminal = page->terminalView()) {
        connect(terminal, &TerminalView::selectionChanged, this, &Part::updateActions);
        connect(terminal, &QObject::destroyed, this, &Part::updateActions);
    }

    activateSession();
}

void Part::removeSessionWidget(TerminalSession *session)
{
    if (m_destroying) {
        return;
    }

    m_pendingPtys.remove(session);
    m_launchRequests.remove(session);
    auto *page = m_pages.take(session);

    if (!page) {
        return;
    }

    m_syncingTabs = true;
    m_tabs->removeTab(m_tabs->indexOf(page));
    m_syncingTabs = false;
    delete page;
}

void Part::activateSession()
{
    if (m_destroying) {
        return;
    }

    auto *page = m_pages.value(activeSession());

    if (page) {
        m_syncingTabs = true;
        m_tabs->setCurrentWidget(page);
        m_syncingTabs = false;
        m_tabs->setFocusProxy(page);

        // Hotspot installs a filter on widget()->focusWidget() before showing the Part.
        // Set the proxy's focus now so that lookup succeeds.
        page->setFocus(Qt::OtherFocusReason);

        Q_EMIT setWindowCaption(activeSession()->title());

        const auto directory = currentWorkingDirectory();

        if (!directory.isEmpty()) {
            Q_EMIT currentDirectoryChanged(directory);
        }
    }

    updateActions();
    updateHostFocus();
}

void Part::startShell(TerminalSession *session, const QString &directory)
{
    auto *page = m_pages.value(session);

    if (!session || !page || !page->terminal->terminalView()
        || (session->state() != Session::State::NotStarted && session->state() != Session::State::Failed)) {
        return;
    }

    auto request = m_launchRequests.value(session);

    if (!directory.isEmpty()) {
        request.directory = directory;
    }

    const auto launch = resolveLaunch(session->launchSettings(), request);
    const auto pty = m_pendingPtys.take(session);
    page->terminal->setHostError({});
    session->start(launch, pty.fileDescriptor());
}

void Part::startProgram(const QString &program, const QStringList &arguments)
{
    auto *session = activeSession();

    if (!session || !activeWidget() || (session->state() != Session::State::NotStarted && session->state() != Session::State::Failed)) {
        return;
    }

    if (program.isEmpty()) {
        startShell(session);

        return;
    }

    // Konsole's published callers supply argv[0], unlike QProcess.
    auto request = m_launchRequests.value(session);
    request.command = QStringList{program} + (arguments.isEmpty() ? QStringList{} : arguments.mid(1));
    const auto launch = resolveLaunch(session->launchSettings(), request);
    const auto pty = m_pendingPtys.take(session);
    activeWidget()->setHostError({});
    session->start(launch, pty.fileDescriptor());
}

void Part::showShellInDir(const QString &directory)
{
    startShell(activeSession(), directory);
}

void Part::sendInput(const QString &text)
{
    if (auto *session = activeSession()) {
        session->sendInput(text);
    }
}

int Part::terminalProcessId()
{
    return activeSession() ? activeSession()->processId() : 0;
}

int Part::foregroundProcessId()
{
    return activeSession() ? activeSession()->foregroundCommandId() : -1;
}

QString Part::foregroundProcessName()
{
    return activeSession() ? activeSession()->foregroundCommandName() : QString{};
}

QString Part::currentWorkingDirectory() const
{
    return activeSession() ? activeSession()->workingDirectory() : QString{};
}

QStringList Part::availableProfiles() const
{
    return m_presets->names();
}

QString Part::currentProfileName() const
{
    return activeSession() ? activeSession()->currentPresetName() : QString{};
}

bool Part::setCurrentProfile(const QString &name)
{
    return activeSession() && activeSession()->setCurrentPreset(name);
}

QVariant Part::profileProperty(const QString &name) const
{
    return PresetProperties::property(activeSession(), name);
}

void Part::createSession(const QString &presetName, const QString &directory)
{
    addSession(presetName, directory, true);
}

void Part::newTab()
{
    const QString selected{currentProfileName()};
    addSession(m_presets->preset(selected) ? selected : m_presets->defaultPresetName(), {}, true);
}

void Part::setMonitorSilenceEnabled(bool enabled)
{
    if (auto *session = activeSession()) {
        session->setMonitorSilence(enabled);
    }
}

void Part::setMonitorActivityEnabled(bool enabled)
{
    if (auto *session = activeSession()) {
        session->setMonitorActivity(enabled);
    }
}

bool Part::isBlurEnabled()
{
    return m_settings->backgroundBlur() && m_settings->backgroundOpacity() < 1.0;
}

void Part::openTeletype(int fd, bool runShell)
{
    auto *session = activeSession();

    if (!session || !activeWidget() || (session->state() != Session::State::NotStarted && session->state() != Session::State::Failed)) {
        return;
    }

    const QDBusUnixFileDescriptor pty{fd};

    if (!pty.isValid() || !isatty(pty.fileDescriptor())) {
        activeWidget()->setHostError(i18nd("kwi", "The supplied terminal descriptor is invalid."));

        return;
    }

    activeWidget()->setHostError({});
    m_pendingPtys.remove(session);

    if (runShell) {
        m_pendingPtys.insert(session, pty);

        // Allow startProgram() in the same host call to override the default shell.
        if (m_tabs->isVisible()) {
            QTimer::singleShot(0, session, [this, session] {
                startShell(session);
            });
        }
    } else {
        session->attachTeletype(pty.fileDescriptor());
    }
}

bool Part::openFile()
{
    return false;
}

bool Part::openUrl(const QUrl &url)
{
    setUrl(url);

    Q_EMIT started(nullptr);

    showShellInDir(url.isLocalFile() ? url.toLocalFile() : QDir::homePath());

    Q_EMIT completed();

    return true;
}

void Part::showSettings(QWidget *parent)
{
    SettingsDialog::showDialog(parent ? parent : widget()->window(), m_settings, m_presets);
}

void Part::showManageProfilesDialog(QWidget *parent)
{
    SettingsDialog::showDialog(parent ? parent : widget()->window(), m_settings, m_presets, currentProfileName(), SettingsDialog::Page::Presets);
}

void Part::showEditCurrentProfileDialog(QWidget *parent)
{
    SettingsDialog::showDialog(parent ? parent : widget()->window(), m_settings, m_presets, currentProfileName(), SettingsDialog::Page::Presets);
}

void Part::setContextMenuAdditionalActions(const QList<QAction *> &actions)
{
    m_additionalActions.clear();

    for (auto *action : actions) {
        if (action) {
            m_additionalActions.append(action);
        }
    }
}

void Part::setupActions()
{
    auto *collection = m_terminalClient->actionCollection();
    const auto add = [this, collection](const char *id, const QString &text, const char *icon, const QKeySequence &shortcut, auto callback) {
        auto *action = collection->addAction(QString::fromLatin1(id));
        action->setText(text);
        action->setIcon(QIcon::fromTheme(QString::fromLatin1(icon)));

        if (!shortcut.isEmpty()) {
            KActionCollection::setDefaultShortcut(action, shortcut);
        }

        connect(action, &QAction::triggered, this, callback);

        return action;
    };

    auto *copy = collection->addAction(KStandardActions::Copy, this, [this] {
        if (activeWidget()) {
            activeWidget()->terminalView()->copy();
        }
    });

    KActionCollection::setDefaultShortcut(copy, QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C));

    add("copy-output", i18ndc("kwi", "@action", "Copy Last Command Output"), "edit-copy", {}, [this] {
        if (activeWidget() && !activeWidget()->terminalView()->copyCommandOutput()) {
            KMessageBox::information(widget()->window(), i18ndc("kwi", "@info", "No command output is marked by the shell in retained history."));
        }
    });

    auto *paste = collection->addAction(KStandardActions::Paste, this, [this] {
        if (activeWidget()) {
            activeWidget()->terminalView()->paste();
        }
    });

    KActionCollection::setDefaultShortcuts(paste, {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_V), QKeySequence(Qt::SHIFT | Qt::Key_Insert)});

    add("paste-selection", i18nd("kwi", "Paste Selection"), "edit-paste", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Insert), [this] {
        if (activeWidget()) {
            activeWidget()->terminalView()->pasteSelection();
        }
    });

    auto *selectAll = collection->addAction(KStandardActions::SelectAll, this, [this] {
        if (activeWidget()) {
            activeWidget()->terminalView()->selectAll();
        }
    });

    KActionCollection::setDefaultShortcuts(selectAll, {});

    auto *find = collection->addAction(KStandardActions::Find, this, [this] {
        if (activeWidget()) {
            activeWidget()->showFind();
        }
    });

    KActionCollection::setDefaultShortcut(find, QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F));

    collection->addAction(KStandardActions::FindNext, this, [this] {
        if (activeWidget()) {
            activeWidget()->findNext();
        }
    });

    collection->addAction(KStandardActions::FindPrev, this, [this] {
        if (activeWidget()) {
            activeWidget()->findNext(false);
        }
    });

    auto *saveAs = collection->addAction(KStandardActions::SaveAs, this, [this] {
        saveOutput();
    });

    saveAs->setText(i18ndc("kwi", "@action", "Save Output As…"));
    KActionCollection::setDefaultShortcut(saveAs, QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S));

    add("clear-history", i18nd("kwi", "Clear Scrollback"), "edit-clear-history", {}, [this] {
        if (activeSession()) {
            activeSession()->clearScrollback();
        }
    });

    add("clear-history-and-reset", i18ndc("kwi", "@action", "Reset Terminal"), "edit-clear-history", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_K), [this] {
        if (activeSession()) {
            activeSession()->resetTerminal();
        }
    });

    collection->addAction(KStandardActions::ZoomIn, this, [this] {
        if (activeWidget()) {
            activeWidget()->terminalView()->zoomIn();
        }
    });

    collection->addAction(KStandardActions::ZoomOut, this, [this] {
        if (activeWidget()) {
            activeWidget()->terminalView()->zoomOut();
        }
    });

    add("zoom_reset", i18nd("kwi", "Reset Zoom"), "zoom-original", QKeySequence(Qt::CTRL | Qt::Key_0), [this] {
        if (activeWidget()) {
            activeWidget()->terminalView()->resetZoom();
        }
    });

    add("open-browser", i18ndc("kwi", "@action", "Open Current Directory"), "folder-open", {}, [this] {
        m_desktop->openCurrentDirectory(activeSession());
    });

    auto *readOnly = add("view-readonly", i18ndc("kwi", "@action", "Read-Only Terminal"), "object-locked", {}, [this](bool enabled) {
        if (activeSession()) {
            activeSession()->setReadOnly(enabled);
        }
    });

    readOnly->setCheckable(true);
    auto *activity = add("monitor-activity", i18nd("kwi", "Monitor for Activity"), "notifications", {}, [this](bool enabled) {
        setMonitorActivityEnabled(enabled);
    });

    activity->setCheckable(true);
    auto *silence = add("monitor-silence", i18nd("kwi", "Monitor for Silence"), "notifications", {}, [this](bool enabled) {
        setMonitorSilenceEnabled(enabled);
    });

    silence->setCheckable(true);
    add("new-tab", i18nd("kwi", "New Tab"), "tab-new", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_T), [this] {
        newTab();
    });

    add("rename-session", i18nd("kwi", "Rename Tab…"), "edit-rename", QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_S), [this] {
        renameSession(activeSession());
    });

    add("next-tab", i18nd("kwi", "Next Tab"), "go-next", QKeySequence(Qt::CTRL | Qt::Key_PageDown), [this] {
        if (m_tabs->count()) {
            m_tabs->setCurrentIndex((m_tabs->currentIndex() + 1) % m_tabs->count());
        }
    });

    add("previous-tab", i18nd("kwi", "Previous Tab"), "go-previous", QKeySequence(Qt::CTRL | Qt::Key_PageUp), [this] {
        if (m_tabs->count()) {
            m_tabs->setCurrentIndex((m_tabs->currentIndex() + m_tabs->count() - 1) % m_tabs->count());
        }
    });

    add("close-session", i18nd("kwi", "Close Tab"), "tab-close", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_W), [this] {
        m_sessions->closeSession(m_sessions->currentIndex());
    });

    add("options_configure", i18nd("kwi", "Configure Kwi…"), "configure", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Comma), [this] {
        showSettings();
    });

    // Keep KonsolePart's Profile action IDs for host and saved-shortcut compatibility.
    add("manage-profiles", i18nd("kwi", "Manage Presets…"), "configure", {}, [this] {
        showManageProfilesDialog(widget()->window());
    });

    add("edit-current-profile", i18nd("kwi", "Edit Current Preset…"), "document-edit", {}, [this] {
        showEditCurrentProfileDialog(widget()->window());
    });

    const auto actions = collection->actions();

    for (auto *action : actions) {
        action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    }

    // Give host KShortcutsDialog saves a separate config group to avoid host action IDs.
    collection->setConfigGroup(QStringLiteral("KwiTerminalShortcuts"));
    collection->readSettings();
}

void Part::updateActions()
{
    if (m_destroying) {
        return;
    }

    auto *collection = m_terminalClient->actionCollection();
    auto *session = activeSession();

    const auto actions = collection->actions();

    for (auto *action : actions) {
        action->setEnabled(session != nullptr && activeWidget() != nullptr);
    }

    collection->action(QStringLiteral("edit_copy"))->setEnabled(activeWidget() && activeWidget()->terminalView()->hasSelection());

    if (!session || !activeWidget()) {
        return;
    }

    collection->action(QStringLiteral("view-readonly"))->setChecked(session->isReadOnly());
    collection->action(QStringLiteral("monitor-activity"))->setChecked(session->monitorActivity());
    collection->action(QStringLiteral("monitor-silence"))->setChecked(session->monitorSilence());

    const bool canPaste{session->isActive() && !session->isReadOnly()};
    collection->action(QStringLiteral("edit_paste"))->setEnabled(canPaste);
    collection->action(QStringLiteral("paste-selection"))->setEnabled(canPaste);
}

void Part::renameSession(TerminalSession *target)
{
    const QPointer<TerminalSession> session{target};

    if (!session) {
        return;
    }

    auto *dialog = new QInputDialog(widget()->window());
    dialog->setAttribute(Qt::WA_DeleteOnClose);

    connect(this, &QObject::destroyed, dialog, &QObject::deleteLater);
    dialog->setWindowTitle(i18nd("kwi", "Rename Tab"));
    dialog->setLabelText(i18nd("kwi", "Tab name (leave empty to use the automatic title):"));
    dialog->setTextValue(session->customTitle());

    connect(dialog, &QInputDialog::textValueSelected, this, [session](const QString &title) {
        if (session) {
            session->setCustomTitle(title);
        }
    });

    dialog->open();
}

void Part::tabContextMenu(QPoint position, TerminalSession *target)
{
    if (!target) {
        return;
    }

    const QPointer<TerminalSession> session{target};
    auto *menu = new QMenu(widget());
    menu->setObjectName(QStringLiteral("tabContextMenu"));
    menu->setAttribute(Qt::WA_DeleteOnClose);
    const auto *collection = m_terminalClient->actionCollection();
    const auto *rename = collection->action(QStringLiteral("rename-session"));
    menu->addAction(rename->icon(), rename->text(), this, [this, session] {
        renameSession(session);
    });

    const int index{m_sessions->indexOf(session)};
    auto *left = menu->addAction(QIcon::fromTheme(QStringLiteral("go-previous")), i18ndc("kwi", "@action", "Move Tab Left"), this, [this, session] {
        const int current{m_tabs->indexOf(m_pages.value(session))};
        m_tabs->tabBar()->moveTab(current, current - 1);
    });
    left->setEnabled(index > 0);
    auto *right = menu->addAction(QIcon::fromTheme(QStringLiteral("go-next")), i18ndc("kwi", "@action", "Move Tab Right"), this, [this, session] {
        const int current{m_tabs->indexOf(m_pages.value(session))};
        m_tabs->tabBar()->moveTab(current, current + 1);
    });
    right->setEnabled(index < m_sessions->count() - 1);
    menu->addSeparator();
    const auto *close = collection->action(QStringLiteral("close-session"));
    menu->addAction(close->icon(), close->text(), this, [this, session] {
        m_sessions->closeSession(m_sessions->indexOf(session));
    });
    menu->popup(position);
}

void Part::contextMenu(QPoint position, const QUrl &link)
{
    auto *menu = new QMenu(widget());
    menu->setObjectName(QStringLiteral("terminalContextMenu"));
    menu->setAttribute(Qt::WA_DeleteOnClose);
    const auto add = [this, menu](const char *id) {
        menu->addAction(m_terminalClient->actionCollection()->action(QString::fromLatin1(id)));
    };

    if (!link.isEmpty()) {
        menu->addAction(QIcon::fromTheme(QStringLiteral("document-open")), i18ndc("kwi", "@action", "Open Link"), m_desktop, [this, link] {
            m_desktop->openUrl(link);
        });

        menu->addAction(QIcon::fromTheme(QStringLiteral("edit-copy")), i18ndc("kwi", "@action", "Copy Link"), this, [this, link] {
            if (activeWidget()) {
                activeWidget()->terminalView()->copyLink(link);
            }
        });

        menu->addSeparator();
    }

    add("edit_copy");
    add("copy-output");
    add("edit_paste");
    add("edit_select_all");
    menu->addSeparator();
    add("edit_find");

    if (activeWidget() && webShortcutsEnabled()) {
        auto *webShortcuts = new KIO::KUriFilterSearchProviderActions{menu};
        webShortcuts->setSelectedText(activeWidget()->terminalView()->selectedText());
        webShortcuts->addWebShortcutsToMenu(menu);
    }

    menu->addSeparator();
    add("open-browser");
    menu->addSeparator();
    add("view-readonly");
    menu->addSeparator();
    add("options_configure");

    if (!m_additionalActions.isEmpty()) {
        menu->addSeparator();
    }

    for (const auto &action : std::as_const(m_additionalActions)) {
        if (action) {
            menu->addAction(action);
        }
    }

    menu->popup(position);
}

void Part::saveOutput()
{
    const QPointer<TerminalSession> session{activeSession()};
    auto *dialog = new QFileDialog(widget()->window(), i18nd("kwi", "Save Terminal Output"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);

    connect(this, &QObject::destroyed, dialog, &QObject::deleteLater);
    dialog->setAcceptMode(QFileDialog::AcceptSave);
    dialog->setNameFilter(i18nd("kwi", "Text Files (*.txt)"));
    dialog->setDefaultSuffix(QStringLiteral("txt"));

    connect(dialog, &QFileDialog::urlSelected, this, [this, session](const QUrl &url) {
        if (session) {
            m_desktop->saveOutput(url, session);
        }
    });

    dialog->open();
}

void Part::showCloseConfirmation(quint64 requestId, const QString &title)
{
    auto *dialog =
        new QMessageBox(QMessageBox::Warning, i18nd("kwi", "Close Running Command?"), QString{}, QMessageBox::Yes | QMessageBox::Cancel, widget()->window());
    dialog->setAttribute(Qt::WA_DeleteOnClose);

    connect(this, &QObject::destroyed, dialog, &QObject::deleteLater);
    dialog->setTextFormat(Qt::PlainText);
    const QFontMetrics metrics{dialog->fontMetrics()};
    const QString displayTitle{metrics.elidedText(title, Qt::ElideMiddle, metrics.averageCharWidth() * 60)};
    dialog->setText(i18nd("kwi", "Closing “%1” will stop its running command.", displayTitle));
    dialog->button(QMessageBox::Yes)->setText(i18nd("kwi", "Close Terminal"));
    dialog->setDefaultButton(QMessageBox::Cancel);
    auto *doNotAskAgain = new QCheckBox(i18ndc("kwi", "@option:check", "Do not ask again"), dialog);
    doNotAskAgain->setEnabled(!m_settings->isConfirmCloseRunningImmutable());
    dialog->setCheckBox(doNotAskAgain);
    const auto cancelled = connect(m_sessions, &SessionModel::closeConfirmationCancelled, dialog, [dialog, requestId](quint64 cancelledId) {
        if (requestId == cancelledId) {
            dialog->reject();
        }
    });

    connect(dialog, &QMessageBox::finished, this, [this, requestId, doNotAskAgain, cancelled](int result) {
        disconnect(cancelled);

        if (result == QMessageBox::Yes) {
            m_sessions->confirmClose(requestId, doNotAskAgain->isChecked());
        } else {
            m_sessions->cancelClose(requestId);
        }
    });

    dialog->open();
}

void Part::updateHostWindow()
{
    if (widget() && widget()->window()) {
        m_desktop->setWindow(widget()->window()->windowHandle());
    }
}

void Part::updateHostFocus()
{
    auto *page = activeWidget();
    m_desktop->setSessionFocused(!m_destroying && page && page->isVisible() && (page->hasFocus() || page->isAncestorOf(QApplication::focusWidget())));
}

bool Part::eventFilter(QObject *object, QEvent *event)
{
    if (!m_destroying && object != m_tabs && (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress)) {
        const QKeySequence pressed{static_cast<QKeyEvent *>(event)->keyCombination()};

        if (event->type() == QEvent::ShortcutOverride) {
            m_shortcutOrigin = object;
            m_negotiatedShortcut = pressed;
        } else if (m_shortcutOrigin && m_shortcutOrigin != object && m_negotiatedShortcut == pressed) {
            // Kate can create a tab during ShortcutOverride. Consume the following
            // key press so it neither reaches the new terminal nor opens another tab.
            m_shortcutOrigin.clear();
            event->accept();

            return true;
        }

        const auto actions = m_terminalClient->actionCollection()->actions();

        for (auto *action : actions) {
            if (!action->isEnabled() || !action->isVisible()) {
                continue;
            }

            const auto shortcuts = action->shortcuts();

            if (std::ranges::any_of(shortcuts, [&pressed](const QKeySequence &shortcut) {
                    return shortcut.count() == 1 && pressed.matches(shortcut) == QKeySequence::ExactMatch;
                })) {
                // Accept terminal actions before Qt reports ambiguity with host shortcuts.
                // Leave multi-chord sequences to Qt's shortcut engine.
                event->accept();

                if (event->type() == QEvent::KeyPress) {
                    m_shortcutOrigin.clear();
                    action->trigger();
                }

                return true;
            }
        }

        if (event->type() == QEvent::KeyPress) {
            m_shortcutOrigin.clear();
        }
    }

    if (!m_destroying && object == m_tabs && event->type() == QEvent::ParentChange) {
        // Attaching Quick can recreate the native window and reenter the host's
        // showEvent before its Part pointer is assigned. Defer until it returns.
        for (auto *page : std::as_const(m_pages)) {
            page->detach();
        }

        if (!m_embeddingPending) {
            m_embeddingPending = true;
            QTimer::singleShot(0, this, [this] {
                m_embeddingPending = false;

                if (m_destroying) {
                    return;
                }

                for (auto *page : std::as_const(m_pages)) {
                    page->attach();
                }

                updateHostWindow();
                updateHostFocus();
            });
        }
    }

    if (!m_destroying && object == m_tabs
        && (event->type() == QEvent::Show || event->type() == QEvent::Hide || event->type() == QEvent::ParentChange || event->type() == QEvent::WinIdChange)) {
        updateHostWindow();
        updateHostFocus();

        if (event->type() == QEvent::Show) {
            const QPointer<TerminalSession> session{activeSession()};
            QTimer::singleShot(0, this, [this, session] {
                if (session) {
                    startShell(session);
                }
            });
        }
    }

    return KParts::ReadOnlyPart::eventFilter(object, event);
}
}
