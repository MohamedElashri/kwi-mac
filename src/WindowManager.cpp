// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "WindowManager.h"
#include "DesktopIntegration.h"
#include "Diagnostics_p.h"
#include "RunningCommandCheck.h"
#include "Session.h"
#include "SessionModel.h"
#include "TerminalSession.h"
#include "settings/PresetEditor.h"
#include "settings/TerminalPaletteModel.h"
#include "settings/TerminalPresets.h"
#include "settings/TerminalSettings.h"
#include "ui/AppShortcuts.h"

#include <KLocalizedQmlContext>
#include <KLocalizedString>
#include <KSharedConfig>
#include <KWindowEffects>
#include <KWindowStateSaver>
#include <KWindowSystem>
#include <QAccessible>
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDebug>
#include <QDir>
#include <QEvent>
#include <QEventLoopLocker>
#include <QGuiApplication>
#include <QMetaObject>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QScreen>
#include <QSize>
#include <QSizeF>
#include <QTimer>
#include <QUuid>
#include <QVariant>
#include <QtMath>
#include <algorithm>
#include <utility>
#ifdef Q_OS_MACOS
#include "ui/MacWindowEffects.h"
#include <dlfcn.h>
#endif

namespace Kwi
{
class WindowManager::WindowContext final : public QObject
{
public:
    WindowContext(TerminalSettings *settings, TerminalPresets *presets, QObject *parent)
        : QObject{parent}
        , sharedSettings{settings}
        , sessions{new SessionModel{settings, presets, this}}
        , desktop{new DesktopIntegration{sessions, DesktopIntegration::Mode::Standalone, this}}
        , engine{new QQmlApplicationEngine{this}}
    {
        connect(settings, &TerminalSettings::backgroundBlurChanged, this, &WindowContext::updateBlur);
        connect(settings, &TerminalSettings::backgroundOpacityChanged, this, &WindowContext::updateBlur);
    }

    ~WindowContext() override
    {
        // Destroy QML bindings and render resources before their session owners.
        delete engine;
    }

    void updateBlur()
    {
        if (!window) {
            return;
        }

#ifdef Q_OS_MACOS
        setMacBackgroundBlur(window, sharedSettings->backgroundBlur() && sharedSettings->backgroundOpacity() < 1);
#else
        KWindowEffects::enableBlurBehind(window, sharedSettings->backgroundBlur() && sharedSettings->backgroundOpacity() < 1);
#endif
    }

    TerminalSettings *const sharedSettings;
    SessionModel *sessions;
    DesktopIntegration *desktop;
    QQmlApplicationEngine *engine;
    QPointer<QQuickWindow> window{};
    // Keep asynchronous process cleanup alive after the window disappears.
    QEventLoopLocker quitLock{};
    bool disposeAfterDrag{false};
};

WindowManager::WindowManager(TerminalSettings *settings, TerminalPresets *presets, AppShortcuts *shortcuts, TerminalPaletteModel *palettes, QObject *parent)
    : QObject{parent}
    , m_settings{settings}
    , m_presets{presets}
    , m_shortcuts{shortcuts}
    , m_palettes{palettes}
{
#ifdef Q_OS_MACOS
    QCoreApplication::instance()->installEventFilter(this);
#endif
}

bool WindowManager::eventFilter(QObject *watched, QEvent *event)
{
#ifdef Q_OS_MACOS
    if (watched == QCoreApplication::instance()) {
        if (event->type() == QEvent::Quit && !m_windows.isEmpty()) {
            if (!m_quitting) {
                requestQuit(m_windows.constLast(), false);
            }
            return true;
        }
        if (event->type() == QEvent::ApplicationStateChange && QGuiApplication::applicationState() == Qt::ApplicationActive && m_windows.isEmpty()
            && !m_quitting) {
            LaunchOptions options;
            options.launch.contextDirectory = QDir::homePath();
            launch(options);
        }
    }
#endif
    return QObject::eventFilter(watched, event);
}

WindowManager::~WindowManager()
{
    clearTabDrag();

    // Dispose children while members accessed by destroyed callbacks still exist.
    while (!m_windows.isEmpty()) {
        delete m_windows.takeLast();
    }
}

int WindowManager::count() const
{
    return m_windows.size();
}

SessionModel *WindowManager::sessions(const QQuickWindow *window) const
{
    const auto found = std::ranges::find(m_windows, window, &WindowContext::window);

    return found == m_windows.cend() ? nullptr : (*found)->sessions;
}

WindowManager::WindowContext *WindowManager::contextForSessions(const SessionModel *sessionModel) const
{
    const auto found = std::ranges::find(m_windows, sessionModel, &WindowContext::sessions);

    return found == m_windows.cend() ? nullptr : *found;
}

TerminalSession *WindowManager::draggedSession() const
{
    return m_draggedSession;
}

QString WindowManager::beginTabDrag(SessionModel *source, TerminalSession *session)
{
    auto *context = contextForSessions(source);

    if (!m_dragToken.isEmpty() || !context || !session || source->indexOf(session) < 0 || !context->window || !context->window->isVisible()) {
        return {};
    }

    m_dragToken = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_dragSource = source;
    m_draggedSession = session;

    Q_EMIT draggedSessionChanged();

    return m_dragToken;
}

bool WindowManager::acceptsTabDrag(const QString &token) const
{
    return !m_dragFinishing && !m_dropRecorded && !token.isEmpty() && token == m_dragToken && m_dragSource && m_draggedSession
        && contextForSessions(m_dragSource) && m_dragSource->indexOf(m_draggedSession) >= 0;
}

bool WindowManager::dropTab(const QString &token, SessionModel *destination, int insertionIndex)
{
    auto *context = contextForSessions(destination);

    if (!acceptsTabDrag(token) || !context || !context->window || !context->window->isVisible() || destination->count() == 0 || insertionIndex < 0
        || insertionIndex > destination->count()) {
        return false;
    }

    m_dropDestination = destination;
    m_dropAtEnd = insertionIndex == destination->count();
    m_dropBeforeSession = m_dropAtEnd ? nullptr : destination->data(destination->index(insertionIndex), SessionModel::SessionRole).value<TerminalSession *>();
    m_dropRecorded = true;

    return true;
}

void WindowManager::finishTabDrag(const QString &token, int action)
{
    if (token.isEmpty() || token != m_dragToken || m_dragFinishing) {
        return;
    }

    m_dragFinishing = true;

    // Defer teardown until Qt Quick finishes handling the drag event.
    QTimer::singleShot(0, this, [this, token, action] {
        if (token != m_dragToken) {
            return;
        }

        if (action == Qt::MoveAction && m_dropRecorded) {
            completeTabDrop(token);
        } else {
            clearTabDrag();
        }
    });
}

void WindowManager::completeTabDrop(const QString &token)
{
    if (token != m_dragToken) {
        return;
    }

    const QPointer<SessionModel> source{m_dragSource};
    const QPointer<SessionModel> destination{m_dropDestination};
    const QPointer<TerminalSession> session{m_draggedSession};
    const QPointer<TerminalSession> before{m_dropBeforeSession};
    const bool atEnd{m_dropAtEnd};
    clearTabDrag();
    auto *destinationContext = contextForSessions(destination);

    if (!source || !destination || !session || !contextForSessions(source) || !destinationContext || !destinationContext->window
        || !destinationContext->window->isVisible() || destination->count() == 0 || source->indexOf(session) < 0
        || (!atEnd && (!before || destination->indexOf(before) < 0))) {
        return;
    }

    const int from{source->indexOf(session)};
    int to{atEnd ? destination->count() : destination->indexOf(before)};

    if (source == destination) {
        if (to > from) {
            --to;
        }

        source->moveSession(from, to);
    } else {
        destination->adoptSession(source->takeSession(from), to);
    }

    destination->setCurrentIndex(destination->indexOf(session));
    activate(destinationContext);
    QMetaObject::invokeMethod(destinationContext->window, "focusTerminal", Qt::QueuedConnection);
}

void WindowManager::clearTabDrag()
{
    const bool hadDrag{!m_dragToken.isEmpty()};
    const QPointer<WindowContext> sourceContext{contextForSessions(m_dragSource)};
    m_dragSource.clear();
    m_draggedSession.clear();
    m_dropDestination.clear();
    m_dropBeforeSession.clear();
    m_dropAtEnd = false;
    m_dropRecorded = false;
    m_dragFinishing = false;
    m_dragToken.clear();

    if (hadDrag) {
        Q_EMIT draggedSessionChanged();
    }

    if (sourceContext && sourceContext->disposeAfterDrag) {
        disposeWindow(sourceContext);
    }
}

void WindowManager::disposeWindow(WindowContext *context)
{
    if (context->window && context->window->isVisible()) {
        return;
    }

    if (!m_dragToken.isEmpty() && context->sessions == m_dragSource) {
        context->disposeAfterDrag = true;
    } else {
        context->deleteLater();
    }
}

WindowManager::WindowContext *WindowManager::createWindow()
{
    traceStage(QStringLiteral("before-window-context"));
    auto *context = new WindowContext{m_settings, m_presets, this};
    traceStage(QStringLiteral("after-window-context"));
    KLocalization::setupLocalizedContext(context->engine);
    context->engine->setInitialProperties({
        {QStringLiteral("appSettings"), QVariant::fromValue(m_settings)},
        {QStringLiteral("terminalPresets"), QVariant::fromValue(m_presets)},
        {QStringLiteral("presetEditor"), QVariant::fromValue(new PresetEditor{m_presets, context})},
        {QStringLiteral("appShortcuts"), QVariant::fromValue(m_shortcuts)},
        {QStringLiteral("terminalPalettes"), QVariant::fromValue(m_palettes)},
        {QStringLiteral("sessionModel"), QVariant::fromValue(context->sessions)},
        {QStringLiteral("desktopIntegration"), QVariant::fromValue(context->desktop)},
        {QStringLiteral("tabDragController"), QVariant::fromValue(this)},
    });

    context->engine->loadFromModule(QStringLiteral("org.kde.kwi"), QStringLiteral("Main"));
    traceStage(QStringLiteral("after-qml-load"));

    if (context->engine->rootObjects().isEmpty()) {
        delete context;

        return nullptr;
    }

    context->window = qobject_cast<QQuickWindow *>(context->engine->rootObjects().constFirst());

    if (!context->window) {
        delete context;

        return nullptr;
    }

#ifdef Q_OS_MACOS
    configureMacPresentation(context->window);
    const QPointer<QQuickWindow> surface{context->window};
    connect(
        context->window,
        &QQuickWindow::sceneGraphInitialized,
        context,
        [surface] {
            // Platform layers may be recreated with the scene graph. Keep
            // AppKit access on the GUI thread and guard early teardown.
            if (surface)
                configureMacPresentation(surface);
        },
        Qt::QueuedConnection);
#endif

    // HACK: Union assumes Quick tab bars are immutable (KDE bug 527467).
    // Look up Union's attached object at runtime to avoid a build dependency.
    // Remove when Qt/Kirigami supports mutable tabs across styles.
    if (auto *tabBar = context->window->findChild<QObject *>(QStringLiteral("terminalTabBar"))) {
        for (auto *object : tabBar->children()) {
            if (!object->inherits("Union::Quick::QuickElement")) {
                continue;
            }

            QObject *hint{};

            if (QMetaObject::invokeMethod(object, "hint", Qt::DirectConnection, Q_RETURN_ARG(QObject *, hint), Q_ARG(QString, QStringLiteral("immutable")))
                && hint) {
                hint->setProperty("when", false);
            }

            break;
        }
    }

    context->desktop->setWindow(context->window);
    context->updateBlur();
    if (traceEnabled()) {
        connect(
            context->window,
            &QQuickWindow::sceneGraphInitialized,
            context->window,
            [window = context->window] {
                trace(QStringLiteral("graphics"), {{QStringLiteral("api"), int(window->rendererInterface()->graphicsApi())}});
            },
            Qt::DirectConnection);
        const auto logWindow = [window = context->window] {
            trace(QStringLiteral("window"),
                  {{QStringLiteral("width"), window->width()},
                   {QStringLiteral("height"), window->height()},
                   {QStringLiteral("dpr"), window->devicePixelRatio()},
                   {QStringLiteral("refresh_hz"), window->screen() ? window->screen()->refreshRate() : 0},
                   {QStringLiteral("accessibility_active"), QAccessible::isActive()}});
        };
        logWindow();
        connect(context->window, &QWindow::widthChanged, context, logWindow);
        connect(context->window, &QWindow::heightChanged, context, logWindow);
        connect(context->window, &QWindow::screenChanged, context, logWindow);
        connect(
            context->window,
            &QQuickWindow::frameSwapped,
            context->window,
            [] {
                QJsonObject fields;
#ifdef Q_OS_MACOS
                // Optional test instrumentation supplies the current drawable
                // ID. Keep it out of normal builds' execution path and avoid
                // replacing Qt's message handler just to correlate frames.
                using DrawableSequence = quint64 (*)();
                static auto sequence = reinterpret_cast<DrawableSequence>(dlsym(RTLD_DEFAULT, "kwi_current_drawable_sequence"));
                if (sequence)
                    fields.insert(QStringLiteral("drawable_sequence"), qint64(sequence()));
#endif
                trace(QStringLiteral("frame-swapped"), std::move(fields));
            },
            Qt::DirectConnection);
    }

    connect(context->window, &QQuickWindow::sceneGraphError, context, [](QQuickWindow::SceneGraphError, const QString &message) {
        qCritical().noquote() << i18n("Could not initialize terminal graphics: %1", message);
        QCoreApplication::exit(1);
    });

    connect(context->window, &QWindow::activeChanged, this, [this, context] {
        if (context->window && context->window->isActive()) {
            m_activeWindow = context->window;
            updateProgress();
        }
    });

    connect(context->sessions, &SessionModel::allClosed, context, [this, context] {
        disposeWindow(context);
    });
    connect(context->sessions, &SessionModel::countChanged, context, [this, context] {
        // A window can already be hidden when its final tab closes. In that
        // order no further visibleChanged signal arrives to dispose it.
        if (context->sessions->count() == 0 && (!context->window || !context->window->isVisible())) {
            disposeWindow(context);
        }
    });

    connect(context->sessions, &SessionModel::currentIndexChanged, this, &WindowManager::updateProgress);
    connect(context->sessions, &SessionModel::sessionAdded, context, [this, context](TerminalSession *session) {
        connect(session, &TerminalSession::progressChanged, context, [this] {
            updateProgress();
        });
    });
    connect(context->sessions, &SessionModel::sessionRemoved, context, [context](TerminalSession *session) {
        disconnect(session, nullptr, context, nullptr);
    });

    connect(context->sessions, &SessionModel::newWindowRequested, this, [this](const QString &presetName, const QString &contextDirectory) {
        LaunchOptions options{};
        options.presetName = presetName;
        options.launch.contextDirectory = contextDirectory;
        launch(options);
    });

    connect(context->sessions, &SessionModel::detachRequested, this, [this, context](TerminalSession *session) {
        detach(context, session);
    });

    connect(context->sessions, &SessionModel::quitRequested, this, [this, context](bool force) {
        requestQuit(context, force);
    });

    connect(context->sessions, &SessionModel::closeConfirmationRequested, context->window, [this, context] {
        activate(context);
    });

    connect(context->sessions, &SessionModel::closeRequestCancelled, this, [this, context] {
        cancelQuit(context->sessions);
    });

    connect(context, &QObject::destroyed, this, [this, context] {
        m_windows.removeAll(context);
        updateProgress();
#ifdef Q_OS_MACOS
        if (m_quitting && m_windows.isEmpty()) {
            QCoreApplication::quit();
        }
#endif
    });

    m_windows.append(context);

    return context;
}

void WindowManager::initializeWindow(WindowContext *context)
{
    if (!m_settings->useCustomWindowSize()) {
        // Keep the established state file so the Zirq rename preserves window geometry.
        new KWindowStateSaver{context->window, KConfigGroup{KSharedConfig::openStateConfig(QStringLiteral("kwistaterc")), QStringLiteral("MainWindow")}};

        return;
    }

    const auto resizeToGrid = [context] {
        if (context->window->windowState() != Qt::WindowNoState) {
            return;
        }

        QSizeF requested;
        QMetaObject::invokeMethod(context->window, "initialWindowSize", Q_RETURN_ARG(QSizeF, requested));
        const QSize size{qCeil(requested.width()), qCeil(requested.height())};
        context->window->resize(size.expandedTo(context->window->minimumSize()).boundedTo(context->window->screen()->availableGeometry().size()));
    };
    resizeToGrid();

    // We get the initial surface scale inbetween show() and delivering the
    // first frame, so we need to derive the initial grid size at that time
    // to take scaling into account.
    const auto scaleConnection = connect(context->window, &QQuickWindow::devicePixelRatioChanged, context, resizeToGrid);
    connect(
        context->window,
        &QQuickWindow::afterAnimating,
        context,
        [scaleConnection] {
            disconnect(scaleConnection);
        },
        Qt::SingleShotConnection);
}

void WindowManager::updateProgress()
{
#ifndef Q_OS_MACOS
    // Report the last active window, falling back to a surviving window after it closes.
    const auto *model = sessions(m_activeWindow);

    if (!model || model->count() == 0) {
        model = nullptr;

        for (auto candidate = m_windows.crbegin(); candidate != m_windows.crend(); ++candidate) {
            // Assign the model directly without find_if's end check and dereference.
            // cppcheck-suppress useStlAlgorithm
            if ((*candidate)->sessions->count() > 0) {
                model = (*candidate)->sessions;
                break;
            }
        }
    }

    const auto *session = model ? model->currentSession() : nullptr;
    const auto state = session ? session->progressState() : Session::ProgressState::None;
    const int percent = session ? session->progressPercent() : -1;
    // The Unity launcher protocol cannot display indeterminate progress.
    const bool visible =
        session && !session->isFinished() && state != Session::ProgressState::None && state != Session::ProgressState::Indeterminate && percent >= 0;
    QVariantMap properties{{QStringLiteral("progress-visible"), visible}};

    if (visible) {
        properties.insert(QStringLiteral("progress"), percent / 100.0);
    }

    if (m_lastProgress == properties) {
        return;
    }

    m_lastProgress = properties;
    QString launcherId{QStringLiteral("application://") + QGuiApplication::desktopFileName()};

    if (!launcherId.endsWith(QStringLiteral(".desktop"))) {
        launcherId += QStringLiteral(".desktop");
    }

    auto message =
        QDBusMessage::createSignal(QStringLiteral("/org/kde/kwi/UnityLauncher"), QStringLiteral("com.canonical.Unity.LauncherEntry"), QStringLiteral("Update"));
    message.setArguments({launcherId, properties});
    QDBusConnection::sessionBus().send(message);
#endif
}

QList<QPointer<TerminalSession>> WindowManager::sessionSnapshot() const
{
    QList<QPointer<TerminalSession>> snapshot{};

    for (const auto *context : m_windows) {
        // Flatten session lists with Qt's bulk append; transform maps individual entries.
        // cppcheck-suppress useStlAlgorithm
        snapshot.append(context->sessions->sessionSnapshot());
    }

    return snapshot;
}

void WindowManager::requestQuit(WindowContext *context, bool force)
{
    // Cancellation signals must finish before advancing the quit generation.
    for (auto *candidate : std::as_const(m_windows)) {
        candidate->sessions->cancelClose();
    }

    const auto serial = ++m_quitRequestSerial;

    if (!force && m_settings->confirmCloseRunning()) {
        m_quitRequester = context->sessions;
        const auto snapshot = sessionSnapshot();
        const QPointer<WindowContext> requester{context};
        checkRunningCommands(snapshot, this, [this, snapshot, requester, serial](bool running) {
            if (!requester || serial != m_quitRequestSerial) {
                return;
            }

            if (snapshot != sessionSnapshot()) {
                requestQuit(requester, false);
            } else if (m_settings->confirmCloseRunning() && running) {
                requester->sessions->requestCloseConfirmation(SessionModel::CloseScope::Application);
            } else {
                requestQuit(requester, true);
            }
        });

        return;
    }

    m_quitting = true;
    for (auto *candidate : std::as_const(m_windows)) {
        candidate->sessions->closeAll(true);
    }
}

void WindowManager::cancelQuit(const SessionModel *source)
{
    ++m_quitRequestSerial;
    const auto requester = std::exchange(m_quitRequester, {});

    if (requester && requester != source) {
        requester->cancelClose();
    }
}

void WindowManager::activate(WindowContext *context, bool fullscreen)
{
    auto states = context->window->windowStates() & ~Qt::WindowMinimized;

    if (fullscreen) {
        states |= Qt::WindowFullScreen;
    }

    context->window->setWindowStates(states);
    context->window->setVisible(true);
    m_activeWindow = context->window;
    updateProgress();
#ifdef Q_OS_MACOS
    context->window->raise();
    context->window->requestActivate();
#else
    KWindowSystem::updateStartupId(context->window);
    KWindowSystem::activateWindow(context->window);
#endif
}

QQuickWindow *WindowManager::launch(const LaunchOptions &options)
{
    if (!options.presetName.isEmpty() && !m_presets->preset(options.presetName)) {
        qWarning().noquote() << i18n("The terminal preset %1 is not available.", options.presetName);

        return nullptr;
    }

    WindowContext *context = nullptr;

    if (options.newTab && !m_windows.isEmpty()) {
        for (auto *candidate : std::as_const(m_windows)) {
            // Assign the context directly without find_if's end check and dereference.
            // cppcheck-suppress useStlAlgorithm
            if (candidate->window == m_activeWindow && candidate->sessions->count() > 0) {
                context = candidate;
                break;
            }
        }

        if (!context) {
            // A drag may be keeping an already closed window context alive.
            for (auto candidate = m_windows.crbegin(); candidate != m_windows.crend(); ++candidate) {
                // Assign the context directly without find_if's end check and dereference.
                // cppcheck-suppress useStlAlgorithm
                if ((*candidate)->sessions->count() > 0) {
                    context = *candidate;
                    break;
                }
            }
        }
    }

    const bool newWindow{!context};

    if (newWindow) {
        context = createWindow();
    }

    if (!context) {
        return nullptr;
    }

    LaunchRequest request{options.launch};
    QString presetName{options.presetName};

    if (options.newTab && context->sessions->currentSession()) {
        const auto *current = context->sessions->currentSession();

        if (request.contextDirectory.isEmpty()) {
            request.contextDirectory = current->workingDirectory();
        }

        if (presetName.isEmpty()) {
            presetName = current->currentPresetName();
        }
    }

    if (!context->sessions->launchSession(request, presetName, options.hold, options.title)) {
        return nullptr;
    }

    if (newWindow) {
        initializeWindow(context);
    }

    activate(context, options.fullscreen);

    return context->window;
}

void WindowManager::detach(WindowContext *source, TerminalSession *session)
{
    const int index{source->sessions->indexOf(session)};

    if (index < 0) {
        return;
    }

    auto *destination = createWindow();

    if (!destination) {
        return;
    }

    destination->sessions->adoptSession(source->sessions->takeSession(index));
    initializeWindow(destination);
    activate(destination);
}
}
