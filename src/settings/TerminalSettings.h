// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include "TerminalPreferences.h"

#include <KConfigWatcher>
#include <QString>
#include <QStringList>
#include <QtQmlIntegration/qqmlintegration.h>

class QEvent;

namespace Kwi
{

class TerminalSettings : public TerminalPreferences
{
    Q_OBJECT
    QML_NAMED_ELEMENT(AppSettings)
    QML_UNCREATABLE("Settings are shared by the application.")
    Q_PROPERTY(bool useSystemFont READ useSystemFont WRITE setUseSystemFont NOTIFY useSystemFontChanged)
    Q_PROPERTY(QString fontFamily READ fontFamily WRITE setFontFamily NOTIFY fontFamilyChanged)
    Q_PROPERTY(qreal fontPointSize READ fontPointSize WRITE setFontPointSize NOTIFY fontPointSizeChanged)
    Q_PROPERTY(QString colorScheme READ colorScheme WRITE setColorScheme NOTIFY colorSchemeChanged)
    Q_PROPERTY(qreal backgroundOpacity READ backgroundOpacity WRITE setBackgroundOpacity NOTIFY backgroundOpacityChanged)
    Q_PROPERTY(int scrollbackLines READ scrollbackLines WRITE setScrollbackLines NOTIFY scrollbackLinesChanged)
    Q_PROPERTY(QString titleFormat READ titleFormat WRITE setTitleFormat NOTIFY titleFormatChanged)
    Q_PROPERTY(QString remoteTitleFormat READ remoteTitleFormat WRITE setRemoteTitleFormat NOTIFY remoteTitleFormatChanged)
    Q_PROPERTY(QString cursorStyle READ cursorStyle WRITE setCursorStyle NOTIFY cursorStyleChanged)
    Q_PROPERTY(QStringList fixedWidthFonts READ fixedWidthFonts NOTIFY fixedWidthFontsChanged)
    Q_PROPERTY(bool canResetAppearance READ canResetAppearance NOTIFY defaultsStateChanged)
    Q_PROPERTY(bool canResetTerminal READ canResetTerminal NOTIFY defaultsStateChanged)
    Q_PROPERTY(bool canResetBehavior READ canResetBehavior NOTIFY defaultsStateChanged)

public:
    explicit TerminalSettings(QObject *parent = nullptr);

    [[nodiscard]] QString fontFamily() const;
    [[nodiscard]] qreal fontPointSize() const;
    [[nodiscard]] qreal backgroundOpacity() const;
    [[nodiscard]] int scrollbackLines() const;
    [[nodiscard]] QString titleFormat() const;
    [[nodiscard]] QString remoteTitleFormat() const;
    [[nodiscard]] QString cursorStyle() const;
    [[nodiscard]] QStringList fixedWidthFonts() const;
    [[nodiscard]] bool canResetAppearance() const;
    [[nodiscard]] bool canResetTerminal() const;
    [[nodiscard]] bool canResetBehavior() const;

    void setUseSystemFont(bool value);
    void setFontFamily(const QString &value);
    void setFontPointSize(qreal value);
    void setColorScheme(const QString &value);
    void setBackgroundOpacity(qreal value);
    void setTitleFormat(const QString &value);
    void setRemoteTitleFormat(const QString &value);
    void setCursorStyle(const QString &value);
    // -1 removes the line limit. Other negative values disable history.
    void setScrollbackLines(int value);

    Q_INVOKABLE void resetAppearanceDefaults();
    Q_INVOKABLE void resetTerminalDefaults();
    Q_INVOKABLE void resetBehaviorDefaults();

    [[nodiscard]] Q_INVOKABLE QString titleFormatError(const QString &value) const;
    [[nodiscard]] Q_INVOKABLE QString previewTitleFormat(const QString &value, bool remote = false) const;

Q_SIGNALS:
    void fixedWidthFontsChanged();
    void defaultsStateChanged();

protected:
    bool eventFilter(QObject *object, QEvent *event) override;
    void usrRead() override;

private:
    void refreshFixedWidthFonts();
    [[nodiscard]] bool canResetGroup(const QString &name) const;
    void resetGroupDefaults(const QString &name);

    KConfigWatcher::Ptr m_watcher{};
    mutable QStringList m_fixedWidthFonts{};
    mutable bool m_fixedWidthFontsValid{false};
};

}
