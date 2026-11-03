// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "DesktopIntegration.h"
#include "OutputSaveJob.h"
#include "SessionModel.h"
#include "TerminalSession.h"

#include <KIO/CommandLauncherJob>
#include <KIO/OpenUrlJob>
#include <KLocalizedString>
#include <KNotification>
#include <KSandbox>
#include <KWindowSystem>
#ifndef Q_OS_MACOS
#include <KWaylandExtras>
#endif
#include <QDir>
#include <QGuiApplication>
#include <QIcon>
#include <QProcess>
#include <QProcessEnvironment>
#include <QUrl>
#include <QWindow>

namespace Kwi
{
void DesktopIntegration::initializeApplicationIcon()
{
    const QString resourcePath{QStringLiteral(":/icons")};

    // Append the bundled icons so the desktop theme takes precedence.
    if (!QIcon::themeSearchPaths().contains(resourcePath)) {
        QIcon::setThemeSearchPaths(QIcon::themeSearchPaths() << resourcePath);
    }

    if (!QIcon::fallbackSearchPaths().contains(resourcePath)) {
        QIcon::setFallbackSearchPaths(QIcon::fallbackSearchPaths() << resourcePath);
    }

    QGuiApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral("org.kde.kwi")));
}

namespace
{
constexpr qsizetype maximumUrlLength{8192};
constexpr qsizetype maximumNotificationLength{8192};
constexpr qint64 notificationIntervalMs{2000};
}

DesktopIntegration::DesktopIntegration(SessionModel *sessions, Mode mode, QObject *parent)
    : QObject{parent}
    , m_sessions{sessions}
    , m_sessionFocused{mode == Mode::Standalone}
{
    connect(sessions, &SessionModel::sessionAdded, this, &DesktopIntegration::watchSession);

    for (int i{0}; i < sessions->count(); ++i) {
        watchSession(sessions->data(sessions->index(i), SessionModel::SessionRole).value<TerminalSession *>());
    }

    connect(sessions, &SessionModel::sessionRemoved, this, &DesktopIntegration::dismissNotification);
    connect(sessions, &SessionModel::sessionRemoved, this, [this](TerminalSession *session) {
        disconnect(session, nullptr, this, nullptr);
    });

    connect(sessions, &SessionModel::currentIndexChanged, this, &DesktopIntegration::dismissCurrentNotification);
}

void DesktopIntegration::watchSession(TerminalSession *session)
{
    connect(session, &TerminalSession::bell, this, [this, session] {
        notifyBell(session);
    });

    connect(session, &TerminalSession::notificationRequested, this, [this, session](const QString &title, const QString &body) {
        const QString text{title.isEmpty() ? body : title + QLatin1Char('\n') + body};
        notify(session, QStringLiteral("notification"), i18n("Terminal Notification"), text);
    });

    connect(session, &TerminalSession::activityDetected, this, [this, session] {
        notify(session, QStringLiteral("activity"), i18n("Terminal Activity"), i18n("New output in %1", session->title()));
    });

    connect(session, &TerminalSession::silenceDetected, this, [this, session] {
        notify(session,
               QStringLiteral("silence"),
               i18n("Terminal Silence"),
               i18np("No output in %2 for %1 second", "No output in %2 for %1 seconds", session->silenceSeconds(), session->title()));
    });
}

void DesktopIntegration::setWindow(QWindow *window)
{
    if (m_window) {
        disconnect(m_window, nullptr, this, nullptr);
    }

    m_window = window;

    if (window) {
        connect(window, &QWindow::activeChanged, this, &DesktopIntegration::dismissCurrentNotification);
    }
}

void DesktopIntegration::setSessionFocused(bool focused)
{
    if (m_sessionFocused != focused) {
        m_sessionFocused = focused;
        dismissCurrentNotification();
    }
}

void DesktopIntegration::toggleFullScreen()
{
    if (!m_window) {
        return;
    }

    // Preserve Maximized when entering full screen so Wayland restores it on exit.
    auto states = m_window->windowStates();
    states.setFlag(Qt::WindowFullScreen, !states.testFlag(Qt::WindowFullScreen));
    m_window->setWindowStates(states);
}

QString DesktopIntegration::errorString() const
{
    return m_error;
}

void DesktopIntegration::clearError()
{
    if (!m_error.isEmpty()) {
        m_error.clear();

        Q_EMIT errorChanged();
    }
}

void DesktopIntegration::setError(const QString &error)
{
    if (m_error == error) {
        return;
    }

    m_error = error;

    Q_EMIT errorChanged();
}

void DesktopIntegration::saveOutput(const QUrl &url, TerminalSession *session)
{
    if (!session || m_sessions->indexOf(session) < 0) {
        setError(i18n("This terminal is no longer available."));

        return;
    }

    if (!url.isLocalFile() || url.toLocalFile().isEmpty()) {
        setError(i18n("Choose a local file to save terminal output."));

        return;
    }

    auto *job = new OutputSaveJob{session->terminal(), url.toLocalFile(), this};
    clearError();

    connect(job, &KJob::result, this, [this](KJob *finished) {
        if (finished->error()) {
            setError(i18n("Could not save terminal output: %1", finished->errorString()));
        }
    });

    job->start();
}

void DesktopIntegration::openCurrentDirectory(TerminalSession *session)
{
    if (!session || m_sessions->indexOf(session) < 0) {
        setError(i18n("This terminal is no longer available."));

        return;
    }

    const QString directory{session->workingDirectory()};

    if (directory.isEmpty() || (!KSandbox::isFlatpak() && !QDir(directory).exists())) {
        setError(i18n("The terminal's current folder is no longer available."));

        return;
    }

    // Keep this a directory launch even if the path is replaced before opening.
    const QUrl url{QUrl::fromLocalFile(directory.endsWith(QLatin1Char('/')) ? directory : directory + QLatin1Char('/'))};
    openUrl(url);
}

void DesktopIntegration::launchHostCommand(const QString &program, const QStringList &arguments)
{
    const auto launch = [this, program, arguments](const QString &token) {
        QProcess process{};
        process.setProgram(program);
        process.setArguments(arguments);

        if (!token.isEmpty()) {
            auto environment = QProcessEnvironment::systemEnvironment();
            environment.insert(QStringLiteral("XDG_ACTIVATION_TOKEN"), token);
            process.setProcessEnvironment(environment);
        }

        const auto context = KSandbox::makeHostContext(process);
        auto *job = new KIO::CommandLauncherJob{context.program, context.arguments, this};
        job->setStartupId(token.toUtf8());
        job->setUiDelegate(nullptr);
        clearError();

        connect(job, &KJob::result, this, [this](KJob *finished) {
            if (finished->error()) {
                setError(i18n("Could not launch the desktop application: %1", finished->errorString()));
            }
        });

        job->start();
    };

#ifndef Q_OS_MACOS
    if (m_window && KWindowSystem::isPlatformWayland()) {
        KWaylandExtras::xdgActivationToken(m_window, {}).then(this, launch);
    } else {
        launch({});
    }
#else
    launch({});
#endif
}

void DesktopIntegration::openUrl(const QUrl &url)
{
    if (!url.isValid() || url.scheme().isEmpty() || url.toEncoded().size() > maximumUrlLength) {
        setError(i18n("The link is invalid or too long."));
        return;
    }

    if (KSandbox::isFlatpak() && url.isLocalFile()) {
        // Host paths may be inaccessible to the file portal inside the sandbox.
        // kde-open uses the host's handler without executing the file.
        launchHostCommand(QStringLiteral("kde-open"), {url.toString(QUrl::FullyEncoded)});

        return;
    }

    auto *job = new KIO::OpenUrlJob{url, this};
    job->setRunExecutables(false);
    job->setUiDelegate(nullptr);
    clearError();

    connect(job, &KJob::result, this, [this](KJob *finished) {
        if (finished->error()) {
            setError(i18n("Could not open the link: %1", finished->errorString()));
        }
    });

    job->start();
}

void DesktopIntegration::notifyBell(TerminalSession *session)
{
    notify(session, QStringLiteral("bell"), i18n("Terminal Bell"), session->title());
}

void DesktopIntegration::notify(TerminalSession *session, const QString &event, const QString &title, const QString &text)
{
    if (!m_window || m_sessions->indexOf(session) < 0 || (m_sessionFocused && m_window->isActive() && m_sessions->currentSession() == session)) {
        return;
    }

    session->setAttention(true);
    const bool monitoring{event == QStringLiteral("activity") || event == QStringLiteral("silence")};

    if (auto previous = m_notifications.value(session)) {
        if (!monitoring || previous->eventId() == event) {
            return;
        }

        dismissNotification(session);
    }

    if (!monitoring && m_lastNotification.isValid() && m_lastNotification.elapsed() < notificationIntervalMs) {
        return;
    }

    auto *notification = new KNotification{event, KNotification::CloseOnTimeout, this};
    notification->setComponentName(QStringLiteral("kwi"));
    notification->setIconName(QStringLiteral("org.kde.kwi"));
    notification->setTitle(title);
    notification->setText(text.left(maximumNotificationLength).toHtmlEscaped());
    notification->setUrgency(KNotification::NormalUrgency);
    const QPointer<TerminalSession> owner{session};

    connect(notification->addDefaultAction(i18n("Show Terminal")), &KNotificationAction::activated, this, [this, owner, notification] {
        if (owner) {
            activateSession(owner, notification->xdgActivationToken());
        }
    });

    m_notifications.insert(session, notification);
    m_lastNotification.start();
    notification->sendEvent();
}

void DesktopIntegration::dismissNotification(TerminalSession *session)
{
    if (const auto notification = m_notifications.take(session)) {
        notification->close();
    }
}

void DesktopIntegration::dismissCurrentNotification()
{
    if (m_sessionFocused && m_window && m_window->isActive()) {
        if (auto *session = m_sessions->currentSession()) {
            session->setAttention(false);
        }

        dismissNotification(m_sessions->currentSession());
    }
}

void DesktopIntegration::activateSession(TerminalSession *session, const QString &activationToken)
{
    const int index{m_sessions->indexOf(session)};

    if (!m_window || index < 0) {
        return;
    }

    m_sessions->setCurrentIndex(index);
    m_window->setWindowStates(m_window->windowStates() & ~Qt::WindowMinimized);
    m_window->setVisible(true);

#ifdef Q_OS_MACOS
    Q_UNUSED(activationToken)
    m_window->raise();
    m_window->requestActivate();
#else
    if (!activationToken.isEmpty()) {
        KWindowSystem::setCurrentXdgActivationToken(activationToken);
    }

    KWindowSystem::activateWindow(m_window);
#endif

    Q_EMIT sessionActivationRequested(session);
}
}
