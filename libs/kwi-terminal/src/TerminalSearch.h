// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "kwiterminal_export.h"
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QRegion>
#include <QString>
#include <QTimer>
#include <Search.h>
#include <memory>
#include <optional>

namespace Kwi
{
class Session;

class KWI_TERMINAL_EXPORT TerminalSearch final : public QObject
{
    Q_OBJECT

public:
    explicit TerminalSearch(QObject *parent = nullptr);
    ~TerminalSearch() override;

    void setSession(Session *session);
    void find(const QString &query, bool forward = true);
    void clear();

    [[nodiscard]] QString status() const;
    [[nodiscard]] bool isBusy() const;
    [[nodiscard]] qsizetype matchCount() const;
    [[nodiscard]] QRegion viewportMatches(bool viewportChanged);

Q_SIGNALS:
    void statusChanged();
    void matchApplied();
    void matchesChanged();

private:
    void requestFeed();
    void advance();
    void reset();
    void fail();
    void setStatus(const QString &statusText, bool busy, qsizetype matches = -1);

    QPointer<Session> m_session;
    QMetaObject::Connection m_changedConnection;
    std::unique_ptr<GhosttyQt::Search> m_search;
    QTimer m_stepTimer;
    std::optional<bool> m_navigation;
    QString m_status;
    qsizetype m_matchCount{-1};
    bool m_feedPending{false};
    bool m_busy{false};
};
}
