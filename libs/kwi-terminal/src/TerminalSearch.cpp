// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "TerminalSearch.h"
#include "Session.h"
#include "SessionEvents_p.h"
#include <KLocalizedString>

namespace Kwi
{
TerminalSearch::TerminalSearch(QObject *parent)
    : QObject{parent}
{
    m_stepTimer.setSingleShot(true);
    connect(&m_stepTimer, &QTimer::timeout, this, &TerminalSearch::advance);
}

TerminalSearch::~TerminalSearch() = default;

void TerminalSearch::setSession(Session *session)
{
    reset();
    disconnect(m_changedConnection);
    m_session = session;

    if (session) {
        m_changedConnection = connect(session->events(), &SessionEvents::historyChanged, this, &TerminalSearch::requestFeed);
    }

    setStatus({}, false);
}

void TerminalSearch::find(const QString &query, bool forward)
{
    if (query.isEmpty()) {
        clear();

        return;
    }

    if (!m_session || !m_session->terminal()) {
        return;
    }

    if (!m_search) {
        m_search = m_session->terminal()->createSearch();
    }

    if (!m_search || !m_search->setNeedle(query)) {
        fail();

        return;
    }

    m_navigation = forward;

    if (!m_stepTimer.isActive()) {
        m_stepTimer.start();
    }

    setStatus(m_status, true, m_matchCount);
}

void TerminalSearch::clear()
{
    reset();
    setStatus({}, false);
    Q_EMIT matchesChanged();
}

void TerminalSearch::reset()
{
    m_stepTimer.stop();
    m_search.reset();
    m_navigation.reset();
    m_feedPending = false;
}

QString TerminalSearch::status() const
{
    return m_status;
}

bool TerminalSearch::isBusy() const
{
    return m_busy;
}

qsizetype TerminalSearch::matchCount() const
{
    return m_matchCount;
}

QRegion TerminalSearch::viewportMatches(bool viewportChanged)
{
    if (!m_search)
        return {};

    // Refresh viewport matches on scroll as well as on output changes.
    if (m_feedPending || viewportChanged || m_search->results().status == GhosttyQt::Search::Status::FeedRequired) {
        m_feedPending = false;

        if (m_search->feed() == GhosttyQt::Search::Status::Error) {
            fail();

            return {};
        }
    }

    const auto matches = m_search->viewportMatches();

    if (!matches) {
        fail();

        return {};
    }

    return *matches;
}

void TerminalSearch::setStatus(const QString &statusText, bool busy, qsizetype matches)
{
    if (m_status == statusText && m_busy == busy && m_matchCount == matches) {
        return;
    }

    m_status = statusText;
    m_busy = busy;
    m_matchCount = matches;

    Q_EMIT statusChanged();
}

void TerminalSearch::requestFeed()
{
    if (!m_search) {
        return;
    }

    m_feedPending = true;

    if (!m_stepTimer.isActive()) {
        m_stepTimer.start();
    }
}

void TerminalSearch::advance()
{
    if (!m_session || !m_session->terminal()) {
        clear();

        return;
    }

    using Status = GhosttyQt::Search::Status;
    auto searchStatus = m_search->results().status;

    if (m_feedPending || searchStatus == Status::FeedRequired) {
        m_feedPending = false;
        searchStatus = m_search->feed();
    }

    if (searchStatus == Status::Running) {
        searchStatus = m_search->tick();
    }

    if (searchStatus == Status::Error) {
        fail();

        return;
    }

    auto results = m_search->results();
    bool applied{false};

    if (m_navigation && (results.totalMatches > 0 || searchStatus == Status::Complete)) {
        if (results.totalMatches > 0 && !m_search->select(*m_navigation)) {
            fail();

            return;
        }

        applied = m_search->applySelection();

        if (!applied) {
            fail();

            return;
        }

        m_navigation.reset();
        results = m_search->results();
        searchStatus = results.status;
    }

    if (searchStatus == Status::Error) {
        fail();

        return;
    }

    const bool busy{searchStatus != Status::Complete};
    const bool awaitingResults{busy && results.totalMatches == 0};
    QString text;

    if (awaitingResults) {
        text = m_status;
    } else if (results.totalMatches == 0) {
        text = i18n("No matches");
    } else if (results.selectedIndex) {
        text = i18n("%1 of %2", *results.selectedIndex + 1, results.totalMatches);
    } else {
        text = i18np("%1 match", "%1 matches", results.totalMatches);
    }

    if (busy && !m_stepTimer.isActive()) {
        m_stepTimer.start();
    }

    setStatus(text, busy, awaitingResults ? m_matchCount : results.totalMatches);

    if (applied) {
        Q_EMIT matchApplied();
    }

    Q_EMIT matchesChanged();
}

void TerminalSearch::fail()
{
    reset();
    setStatus(i18n("Could not search terminal history"), false);
    Q_EMIT matchesChanged();
}
}
