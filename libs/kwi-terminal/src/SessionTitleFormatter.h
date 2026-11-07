// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "kwiterminal_export.h"
#include <QObject>
#include <QPointer>
#include <QString>
#include <QtQmlIntegration/qqmlintegration.h>
#include <optional>

Q_MOC_INCLUDE("Session.h")

namespace Kwi
{
class Session;

/*! \class Kwi::SessionTitleFormatter
 * \internal
 */

/*!
 * \qmltype SessionTitleFormatter
 * \inqmlmodule org.kde.kwi.terminal
 * \inherits QtObject
 * \brief Formats a session title from process information.
 *
 * Bind \l session to a \l Session and use \l text as a window or tab title. The text
 * updates when relevant session properties change. A custom \l format replaces
 * the default local and SSH formats.
 */
class KWI_TERMINAL_EXPORT SessionTitleFormatter : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    /*!
     * \qmlproperty Session SessionTitleFormatter::session
     * The session to describe. The default is null. The formatter does not own or
     * start the session.
     */
    Q_PROPERTY(Kwi::Session *session READ session WRITE setSession NOTIFY sessionChanged)
    /*!
     * \qmlproperty string SessionTitleFormatter::format
     * The title format. By default it is \c {{directory} : {title}}.
     * When Session::hasSshInfo is true, the default is
     * \c {{user}@{host} : {title}}. Assigning a string overrides both defaults.
     * Assign \c undefined to restore them.
     *
     * \table
     * \header \li Placeholder \li Value
     * \row \li \c {{title}} \li The application title, falling back to the command name when empty.
     * \row \li \c {{command}} \li The foreground command name, falling back to the launched program name.
     * \row \li \c {{directory}} \li The last component of the local working directory. The home directory becomes \c ~.
     * \row \li \c {{path}} \li The local working directory, with the home directory replaced by \c ~.
     * \row \li \c {{user}} \li The local user name, or the SSH destination's resolved user name.
     * \row \li \c {{host}} \li The local hostname, or the resolved SSH hostname with its port when it differs from 22.
     * \row \li \c {{shortHost}} \li The hostname up to the first dot, without a port. SSH IP addresses are kept whole.
     * \endtable
     *
     * For SSH sessions, a known remote command replaces the local command name.
     * The directory placeholders still refer to the local working directory.
     * Double opening or closing braces to insert them literally.
     *
     * Formats must contain non-whitespace text and be at most 256 UTF-16 code units
     * long. Unknown placeholders, unmatched braces, control characters and Unicode
     * noncharacters make the format invalid. See errorString for the reason.
     */
    Q_PROPERTY(QString format READ format WRITE setFormat RESET resetFormat NOTIFY formatChanged)
    /*!
     * \qmlproperty string SessionTitleFormatter::text
     * \readonly
     * The formatted title, or an empty string when \l session is null or \l format is
     * invalid. It is limited to 256 UTF-16 code units without splitting a surrogate
     * pair. Control characters and Unicode noncharacters in substituted values are
     * replaced with spaces.
     */
    Q_PROPERTY(QString text READ text NOTIFY textChanged)
    /*!
     * \qmlproperty string SessionTitleFormatter::errorString
     * \readonly
     * A translated explanation of an invalid \l format, or an empty string when the
     * format is valid.
     */
    Q_PROPERTY(QString errorString READ errorString NOTIFY errorChanged)

public:
    explicit SessionTitleFormatter(QObject *parent = nullptr);

    [[nodiscard]] Session *session() const;
    void setSession(Session *newSession);
    [[nodiscard]] QString format() const;
    void setFormat(const QString &newFormat);
    void resetFormat();
    [[nodiscard]] QString text() const;
    [[nodiscard]] QString errorString() const;

Q_SIGNALS:
    void sessionChanged();
    void formatChanged();
    void textChanged();
    void errorChanged();

private:
    void update();

    QPointer<Session> m_session;
    std::optional<QString> m_formatOverride;
    QString m_format;
    QString m_text;
    QString m_error;
};
}
