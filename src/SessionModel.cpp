// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "SessionModel.h"
#include "RunningCommandCheck.h"
#include "Session.h"
#include "TerminalSession.h"
#include "settings/LaunchSettings.h"
#include "settings/TerminalPresets.h"
#include "settings/TerminalSettings.h"

#include <QDir>
#include <QTimer>
#include <memory>
#include <utility>

namespace Kwi
{
SessionModel::SessionModel(TerminalSettings *settings, TerminalPresets *presets, QObject *parent)
    : QAbstractListModel{parent}
    , m_settings{settings}
    , m_presets{presets}
{
}

int SessionModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_sessions.size();
}

int SessionModel::count() const
{
    return m_sessions.size();
}

QVariant SessionModel::data(const QModelIndex &index, int role) const
{
    if (role != SessionRole || !index.isValid() || index.row() < 0 || index.row() >= count()) {
        return {};
    }

    return QVariant::fromValue(m_sessions.at(index.row()));
}

QHash<int, QByteArray> SessionModel::roleNames() const
{
    return {{SessionRole, "session"}};
}

int SessionModel::currentIndex() const
{
    return m_currentIndex;
}

void SessionModel::setCurrentIndex(int index)
{
    if (index < 0 || index >= count() || m_currentIndex == index) {
        return;
    }

    m_currentIndex = index;
    auto *session = currentSession();
    m_activationHistory.removeOne(session);
    m_activationHistory.prepend(session);

    Q_EMIT currentIndexChanged();
}

TerminalSession *SessionModel::currentSession() const
{
    return m_sessions.value(m_currentIndex);
}

int SessionModel::indexOf(TerminalSession *session) const
{
    return m_sessions.indexOf(session);
}

QVariantList SessionModel::entries() const
{
    QVariantList result;
    result.reserve(m_sessions.size());

    for (auto *session : m_sessions) {
        result.append(QVariantMap{{QStringLiteral("label"), session->title()}, {QStringLiteral("session"), QVariant::fromValue(session)}});
    }

    return result;
}

void SessionModel::newSession()
{
    const auto *current = currentSession();
    launchSession({.contextDirectory = current ? current->workingDirectory() : QDir::currentPath()},
                  current ? current->currentPresetName() : m_presets->defaultPresetName());
}

bool SessionModel::newSessionWithPreset(const QString &name)
{
    return launchSession({.contextDirectory = currentSession() ? currentSession()->workingDirectory() : QDir::currentPath()}, name);
}

bool SessionModel::launchSession(const LaunchRequest &request, const QString &presetName, bool hold, const QString &title)
{
    auto ownedSession = std::make_unique<TerminalSession>(m_settings, m_presets);

    if (!ownedSession->setCurrentPreset(presetName.isEmpty() ? m_presets->defaultPresetName() : presetName)) {
        return false;
    }

    // Capture the launch before observers can change presets or move the session.
    const TerminalLaunch launch{resolveLaunch(ownedSession->launchSettings(), request)};
    auto *session = ownedSession.release();
    session->setHoldOpen(hold);
    session->setCustomTitle(title);
    adoptSession(session);
    QTimer::singleShot(0, session, [session, launch] {
        session->start(launch);
    });

    return true;
}

void SessionModel::adoptSession(TerminalSession *session, int index)
{
    if (!session || m_sessions.contains(session)) {
        return;
    }

    session->setParent(this);
    const auto *current = currentSession();
    const int row{index < 0 ? m_currentIndex + 1 : qMin(index, count())};
    beginInsertRows({}, row, row);
    m_sessions.insert(row, session);
    m_currentIndex = m_sessions.indexOf(current);
    endInsertRows();

    connect(session, &TerminalSession::exited, this, [this, session] {
        if (!session->holdOpen() && session->state() != Session::State::Failed) {
            removeSession(session);
        }
    });

    Q_EMIT countChanged();

    setCurrentIndex(row);

    Q_EMIT sessionAdded(session);
}

void SessionModel::newWindow()
{
    const auto *current = currentSession();

    Q_EMIT newWindowRequested(current ? current->currentPresetName() : m_presets->defaultPresetName(),
                              current ? current->workingDirectory() : QDir::homePath());
}

void SessionModel::closeApplication(bool force)
{
    Q_EMIT quitRequested(force);
}

void SessionModel::detachSession(int index)
{
    if (index >= 0 && index < count()) {
        Q_EMIT detachRequested(m_sessions.at(index));
    }
}

TerminalSession *SessionModel::takeSession(int index)
{
    if (index < 0 || index >= count()) {
        return nullptr;
    }

    auto *session = m_sessions.at(index);
    eraseSession(session);
    session->setParent(nullptr);

    if (m_sessions.isEmpty() && m_closing.isEmpty()) {
        Q_EMIT allClosed();
    }

    return session;
}

void SessionModel::moveSession(int from, int to)
{
    if (from < 0 || from >= count() || to < 0 || to >= count() || from == to) {
        return;
    }

    const auto *current = currentSession();
    beginMoveRows({}, from, from, {}, to > from ? to + 1 : to);
    m_sessions.move(from, to);
    endMoveRows();
    m_currentIndex = m_sessions.indexOf(current);

    Q_EMIT currentIndexChanged();
}

void SessionModel::activateLastUsed()
{
    setCurrentIndex(m_sessions.indexOf(m_activationHistory.value(1)));
}

void SessionModel::confirmClose(quint64 requestId, bool doNotAskAgain)
{
    if (!m_pendingClose || m_pendingClose->id != requestId) {
        return;
    }

    const CloseRequest request{*m_pendingClose};
    cancelClose();

    if (request.scope == CloseScope::Session && (!request.session || indexOf(request.session) < 0)) {
        return;
    }

    if (doNotAskAgain) {
        m_settings->setConfirmCloseRunning(false);
        m_settings->save();
    }

    switch (request.scope) {
    case CloseScope::Session:
        removeSession(request.session);
        break;
    case CloseScope::Window:
        closeAll(true);
        break;
    case CloseScope::Application:
        closeApplication(true);
        break;
    }
}

void SessionModel::cancelClose(quint64 requestId)
{
    if (requestId && (!m_pendingClose || m_pendingClose->id != requestId)) {
        return;
    }

    const auto pending = std::exchange(m_pendingClose, std::nullopt);
    ++m_closeRequestSerial;

    Q_EMIT closeRequestCancelled();

    if (pending) {
        Q_EMIT closeConfirmationCancelled(pending->id);
    }
}

void SessionModel::requestCloseConfirmation(CloseScope scope, TerminalSession *session)
{
    m_pendingClose = CloseRequest{m_closeRequestSerial, scope, session};

    Q_EMIT closeConfirmationRequested(m_pendingClose->id, scope, session ? session->title() : QString());
}

QList<QPointer<TerminalSession>> SessionModel::sessionSnapshot() const
{
    return {m_sessions.cbegin(), m_sessions.cend()};
}

void SessionModel::closeSession(int index)
{
    if (index < 0 || index >= count()) {
        return;
    }

    cancelClose();
    auto *session = m_sessions.at(index);

    if (m_settings->confirmCloseRunning()) {
        const auto serial = m_closeRequestSerial;
        const QPointer<TerminalSession> target{session};
        checkRunningCommands({target}, this, [this, target, serial](bool running) {
            if (serial != m_closeRequestSerial || !target || indexOf(target) < 0) {
                return;
            }

            if (m_settings->confirmCloseRunning() && running) {
                requestCloseConfirmation(CloseScope::Session, target);
            } else {
                removeSession(target);
            }
        });

        return;
    }

    removeSession(session);
}

void SessionModel::closeAll(bool force)
{
    cancelClose();

    if (!force && m_settings->confirmCloseRunning()) {
        const auto serial = m_closeRequestSerial;
        const auto snapshot = sessionSnapshot();
        checkRunningCommands(snapshot, this, [this, snapshot, serial](bool running) {
            if (serial != m_closeRequestSerial) {
                return;
            }

            if (snapshot != sessionSnapshot()) {
                closeAll();
            } else if (m_settings->confirmCloseRunning() && running) {
                requestCloseConfirmation(CloseScope::Window);
            } else {
                closeAll(true);
            }
        });

        return;
    }

    if (m_sessions.isEmpty()) {
        if (m_closing.isEmpty()) {
            Q_EMIT allClosed();
        }

        return;
    }

    while (!m_sessions.isEmpty()) {
        removeSession(m_sessions.constLast());
    }
}

void SessionModel::eraseSession(TerminalSession *session)
{
    const int row{indexOf(session)};

    if (row < 0) {
        return;
    }

    if (m_pendingClose && m_pendingClose->scope == CloseScope::Session && m_pendingClose->session == session) {
        cancelClose();
    }

    const auto *current = currentSession();
    beginRemoveRows({}, row, row);
    m_sessions.removeAt(row);
    m_activationHistory.removeOne(session);
    m_currentIndex = m_sessions.indexOf(current == session ? m_activationHistory.value(0) : current);
    endRemoveRows();
    disconnect(session, &TerminalSession::exited, this, nullptr);

    Q_EMIT sessionRemoved(session);
    Q_EMIT countChanged();
    Q_EMIT currentIndexChanged();
}

void SessionModel::removeSession(TerminalSession *session)
{
    if (!m_sessions.contains(session)) {
        return;
    }

    eraseSession(session);

    // Keep the process owner alive through asynchronous termination and reaping.
    if (session->isActive()) {
        m_closing.append(session);

        connect(session, &TerminalSession::exited, this, [this, session] {
            m_closing.removeAll(session);
            session->deleteLater();

            if (m_sessions.isEmpty() && m_closing.isEmpty()) {
                Q_EMIT allClosed();
            }
        });

        session->close();
    } else {
        session->close(); // Also cancels a launch queued before the first event-loop turn.
        session->deleteLater();

        if (m_sessions.isEmpty() && m_closing.isEmpty()) {
            Q_EMIT allClosed();
        }
    }
}
}
