// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QAbstractListModel>
#include <QByteArray>
#include <QHash>
#include <QList>
#include <QPointer>
#include <QString>
#include <QVariant>
#include <QtQmlIntegration/qqmlintegration.h>
#include <optional>

Q_MOC_INCLUDE("TerminalSession.h")

namespace Kwi
{
struct LaunchRequest;
class TerminalSettings;
class TerminalPresets;
class TerminalSession;

class SessionModel final : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Session models are provided by the application.")
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(int currentIndex READ currentIndex WRITE setCurrentIndex NOTIFY currentIndexChanged)
    Q_PROPERTY(Kwi::TerminalSession *currentSession READ currentSession NOTIFY currentIndexChanged)

public:
    enum Roles {
        SessionRole = Qt::UserRole + 1
    };

    enum class CloseScope {
        Session,
        Window,
        Application
    };
    Q_ENUM(CloseScope)

    SessionModel(TerminalSettings *settings, TerminalPresets *presets, QObject *parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex &parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex &index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    [[nodiscard]] int count() const;
    [[nodiscard]] int currentIndex() const;
    void setCurrentIndex(int index);
    [[nodiscard]] TerminalSession *currentSession() const;
    [[nodiscard]] Q_INVOKABLE int indexOf(Kwi::TerminalSession *session) const;
    [[nodiscard]] Q_INVOKABLE QVariantList entries() const;
    [[nodiscard]] QList<QPointer<TerminalSession>> sessionSnapshot() const;

    bool launchSession(const LaunchRequest &request, const QString &presetName = {}, bool hold = false, const QString &title = {});
    void adoptSession(TerminalSession *session, int index = -1);
    [[nodiscard]] TerminalSession *takeSession(int index);

    Q_INVOKABLE void newSession();
    Q_INVOKABLE bool newSessionWithPreset(const QString &name);
    Q_INVOKABLE void newWindow();
    Q_INVOKABLE void closeApplication(bool force = false);
    Q_INVOKABLE void detachSession(int index);
    Q_INVOKABLE void moveSession(int from, int to);
    Q_INVOKABLE void activateLastUsed();

    Q_INVOKABLE void confirmClose(quint64 requestId, bool doNotAskAgain = false);
    Q_INVOKABLE void cancelClose(quint64 requestId = 0);
    Q_INVOKABLE void closeSession(int index);
    Q_INVOKABLE void closeAll(bool force = false);
    void requestCloseConfirmation(CloseScope scope, TerminalSession *session = nullptr);

Q_SIGNALS:
    void countChanged();
    void currentIndexChanged();
    void closeConfirmationRequested(quint64 requestId, Kwi::SessionModel::CloseScope scope, const QString &title);
    void closeConfirmationCancelled(quint64 requestId);
    void closeRequestCancelled();
    void allClosed();
    void sessionAdded(Kwi::TerminalSession *session);
    void sessionRemoved(Kwi::TerminalSession *session);
    void newWindowRequested(const QString &presetName, const QString &contextDirectory);
    void quitRequested(bool force);
    void detachRequested(Kwi::TerminalSession *session);

private:
    struct CloseRequest {
        quint64 id{0};
        CloseScope scope{CloseScope::Session};
        QPointer<TerminalSession> session{};
    };

    void eraseSession(TerminalSession *session);
    void removeSession(TerminalSession *session);

    TerminalSettings *m_settings;
    TerminalPresets *m_presets;
    QList<TerminalSession *> m_sessions{};
    QList<TerminalSession *> m_activationHistory{};
    QList<QPointer<TerminalSession>> m_closing{};
    std::optional<CloseRequest> m_pendingClose;
    quint64 m_closeRequestSerial{0};
    int m_currentIndex{-1};
};
}
