// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "TerminalSettings.h"
#include "SettingsConstraints_p.h"
#include "TerminalColors.h"
#include "TitleFormat_p.h"

#include <KLocalizedString>
#include <KSharedConfig>
#include <QEvent>
#include <QFont>
#include <QFontDatabase>
#include <QFontInfo>
#include <QGuiApplication>

#include <algorithm>
#include <cmath>
#include <utility>

#ifdef Q_OS_MACOS
#include <CoreText/CoreText.h>
#include <memory>
#include <type_traits>
#endif

namespace Kwi
{
namespace
{
#ifdef Q_OS_MACOS
template<typename T>
auto adoptCF(T value)
{
    return std::unique_ptr<std::remove_pointer_t<T>, decltype(&CFRelease)>{value, &CFRelease};
}

QStringList nativeFixedWidthFonts()
{
    // isFixedPitch() populates every queried Qt family, loading font files and
    // language metadata. Ask Core Text for monospaced descriptors instead.
    const int monospaced{kCTFontTraitMonoSpace};
    const auto number = adoptCF(CFNumberCreate(nullptr, kCFNumberIntType, &monospaced));
    const void *traitKeys[]{kCTFontSymbolicTrait};
    const void *traitValues[]{number.get()};
    const auto traits = adoptCF(CFDictionaryCreate(nullptr, traitKeys, traitValues, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
    const void *keys[]{kCTFontTraitsAttribute};
    const void *values[]{traits.get()};
    const auto attributes = adoptCF(CFDictionaryCreate(nullptr, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
    const auto query = adoptCF(CTFontDescriptorCreateWithAttributes(attributes.get()));
    const auto required = adoptCF(CFSetCreate(nullptr, keys, 1, &kCFTypeSetCallBacks));
    const auto matches = adoptCF(CTFontDescriptorCreateMatchingFontDescriptors(query.get(), required.get()));
    QStringList fonts;
    if (matches) {
        for (CFIndex n{0}; n < CFArrayGetCount(matches.get()); ++n) {
            const auto descriptor = static_cast<CTFontDescriptorRef>(CFArrayGetValueAtIndex(matches.get(), n));
            const auto family = adoptCF(static_cast<CFStringRef>(CTFontDescriptorCopyAttribute(descriptor, kCTFontFamilyNameAttribute)));
            if (family) {
                const auto name = QString::fromCFString(family.get());
                if (!name.isEmpty())
                    fonts.append(name);
            }
        }
    }
    fonts.removeDuplicates();
    fonts.sort(Qt::CaseInsensitive);
    return fonts;
}
#endif

bool fixedWidthFamily(const QString &family)
{
    return !family.isEmpty() && QFontDatabase::hasFamily(family) && QFontDatabase::isFixedPitch(family);
}
}

TerminalSettings::TerminalSettings(QObject *parent)
    : TerminalPreferences{KSharedConfig::openConfig(QStringLiteral("kwiterminalrc"), KConfig::NoGlobals), parent}
{
    qGuiApp->installEventFilter(this);
    connect(qGuiApp, &QGuiApplication::fontDatabaseChanged, this, &TerminalSettings::refreshFixedWidthFonts);

    const auto changeSignals = {&TerminalPreferences::useSystemFontChanged,
                                &TerminalPreferences::fontFamilyChanged,
                                &TerminalPreferences::fontPointSizeChanged,
                                &TerminalPreferences::colorSchemeChanged,
                                &TerminalPreferences::backgroundOpacityChanged,
                                &TerminalPreferences::backgroundBlurChanged,
                                &TerminalPreferences::tabBarAtBottomChanged,
                                &TerminalPreferences::showTabStatusIconsChanged,
                                &TerminalPreferences::scrollbackLinesChanged,
                                &TerminalPreferences::silenceSecondsChanged,
                                &TerminalPreferences::titleFormatChanged,
                                &TerminalPreferences::remoteTitleFormatChanged,
                                &TerminalPreferences::windowTitleFromApplicationChanged,
                                &TerminalPreferences::cursorStyleChanged,
                                &TerminalPreferences::cursorBlinkingChanged,
                                &TerminalPreferences::confirmCloseRunningChanged,
                                &TerminalPreferences::confirmMultilinePasteChanged,
                                &TerminalPreferences::copyOnSelectChanged,
                                &TerminalPreferences::optionAsAltChanged,
                                &TerminalPreferences::requireCtrlForLinksChanged};

    for (const auto signal : changeSignals) {
        // Every entry is a typed signal; Clazy cannot resolve the loop variable.
        connect(this, signal, this, &TerminalSettings::defaultsStateChanged); // clazy:exclude=connect-non-signal
    }

    connect(this, &TerminalPreferences::useSystemFontChanged, this, &TerminalPreferences::fontFamilyChanged);
    connect(this, &TerminalPreferences::useSystemFontChanged, this, &TerminalPreferences::fontPointSizeChanged);
    connect(this, &TerminalSettings::fixedWidthFontsChanged, this, &TerminalPreferences::fontFamilyChanged);

    read();
    m_watcher = KConfigWatcher::create(sharedConfig());
    connect(m_watcher.data(), &KConfigWatcher::configChanged, this, [this] {
        read();
        Q_EMIT defaultsStateChanged();
    });
}

void TerminalSettings::usrRead()
{
    if (!std::isfinite(TerminalPreferences::fontPointSize())) {
        TerminalPreferences::setFontPointSize(defaultFontPointSizeValue());
    }

    if (!std::isfinite(TerminalPreferences::backgroundOpacity())) {
        TerminalPreferences::setBackgroundOpacity(defaultBackgroundOpacityValue());
    }

    if (!titleFormatError(TerminalPreferences::titleFormat()).isEmpty()) {
        TerminalPreferences::setTitleFormat(defaultTitleFormatValue());
    }

    if (!titleFormatError(TerminalPreferences::remoteTitleFormat()).isEmpty()) {
        TerminalPreferences::setRemoteTitleFormat(defaultRemoteTitleFormatValue());
    }

    if (!SettingsConstraints::isValidCursorStyle(TerminalPreferences::cursorStyle())) {
        TerminalPreferences::setCursorStyle(defaultCursorStyleValue());
    }

    if (TerminalPreferences::scrollbackLines() < -1) {
        TerminalPreferences::setScrollbackLines(0);
    }
}

QString TerminalSettings::fontFamily() const
{
    const QString family{TerminalPreferences::fontFamily()};

    // Validating one configured face must not enumerate/load every fixed font.
    return useSystemFont() || !fixedWidthFamily(family) ? QFontDatabase::systemFont(QFontDatabase::FixedFont).family() : family;
}

qreal TerminalSettings::fontPointSize() const
{
    const qreal size{TerminalPreferences::fontPointSize()};

    if (!useSystemFont() && std::isfinite(size) && size != 0) {
        return std::clamp(size, SettingsConstraints::MinimumFontPointSize, SettingsConstraints::MaximumFontPointSize);
    }

    const QFont systemFont{QFontDatabase::systemFont(QFontDatabase::FixedFont)};

    return systemFont.pointSizeF() > 0 ? systemFont.pointSizeF() : QFontInfo(systemFont).pointSizeF();
}

qreal TerminalSettings::backgroundOpacity() const
{
    const qreal value{TerminalPreferences::backgroundOpacity()};

    return std::isfinite(value) ? value : defaultBackgroundOpacityValue();
}

int TerminalSettings::scrollbackLines() const
{
    const int value{TerminalPreferences::scrollbackLines()};

    return value < -1 ? 0 : value;
}

QString TerminalSettings::titleFormat() const
{
    const QString format{TerminalPreferences::titleFormat()};

    return titleFormatError(format).isEmpty() ? format : defaultTitleFormatValue();
}

QString TerminalSettings::remoteTitleFormat() const
{
    const QString format{TerminalPreferences::remoteTitleFormat()};

    return titleFormatError(format).isEmpty() ? format : defaultRemoteTitleFormatValue();
}

QString TerminalSettings::cursorStyle() const
{
    const QString style{TerminalPreferences::cursorStyle()};

    return SettingsConstraints::isValidCursorStyle(style) ? style : defaultCursorStyleValue();
}

QString TerminalSettings::titleFormatError(const QString &value) const
{
    return formatTitle(value).error;
}

QString TerminalSettings::previewTitleFormat(const QString &value, bool remote) const
{
    const auto result = formatTitle(value,
                                    {i18nc("@info title format preview", "Shell"),
                                     QStringLiteral("/home/user/project"),
                                     QStringLiteral("user"),
                                     remote ? QStringLiteral("server.example.org") : QStringLiteral("host"),
                                     remote ? QStringLiteral("top") : QStringLiteral("bash"),
                                     QStringLiteral("/home/user")});

    return result.error.isEmpty() ? result.text : QString{};
}

void TerminalSettings::setTitleFormat(const QString &value)
{
    if (titleFormatError(value).isEmpty()) {
        TerminalPreferences::setTitleFormat(value);
    }
}

void TerminalSettings::setRemoteTitleFormat(const QString &value)
{
    if (titleFormatError(value).isEmpty()) {
        TerminalPreferences::setRemoteTitleFormat(value);
    }
}

QStringList TerminalSettings::fixedWidthFonts() const
{
    // The complete picker model is only needed when preferences are opened.
    if (!m_fixedWidthFontsValid) {
#ifdef Q_OS_MACOS
        QStringList fonts{nativeFixedWidthFonts()};
#else
        QStringList fonts;
        const auto families = QFontDatabase::families();
        for (const auto &family : families) {
            if (QFontDatabase::isFixedPitch(family))
                fonts.append(family);
        }
#endif
        const auto systemFamily = QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
        if (!fonts.contains(systemFamily))
            fonts.prepend(systemFamily);
        m_fixedWidthFonts = std::move(fonts);
        m_fixedWidthFontsValid = true;
    }
    return m_fixedWidthFonts;
}

void TerminalSettings::setUseSystemFont(bool value)
{
    if (value == useSystemFont() || isUseSystemFontImmutable()) {
        return;
    }

    if (!value) {
        // Save the system font on first opt-out, then preserve the user's font across toggles.
        if (!fixedWidthFamily(TerminalPreferences::fontFamily())) {
            TerminalPreferences::setFontFamily(fontFamily());
        }

        const qreal size{TerminalPreferences::fontPointSize()};

        if (!std::isfinite(size) || size == 0) {
            TerminalPreferences::setFontPointSize(
                std::clamp(fontPointSize(), SettingsConstraints::MinimumFontPointSize, SettingsConstraints::MaximumFontPointSize));
        }
    }

    TerminalPreferences::setUseSystemFont(value);
}

void TerminalSettings::setFontFamily(const QString &value)
{
    if (!fixedWidthFamily(value) || isFontFamilyImmutable() || (useSystemFont() && isUseSystemFontImmutable())) {
        return;
    }

    TerminalPreferences::setFontFamily(value);
    setUseSystemFont(false);
}

void TerminalSettings::setFontPointSize(qreal value)
{
    if (!std::isfinite(value) || isFontPointSizeImmutable() || (useSystemFont() && isUseSystemFontImmutable())) {
        return;
    }

    TerminalPreferences::setFontPointSize(std::clamp(value, SettingsConstraints::MinimumFontPointSize, SettingsConstraints::MaximumFontPointSize));
    setUseSystemFont(false);
}

void TerminalSettings::refreshFixedWidthFonts()
{
    m_fixedWidthFontsValid = false;
    Q_EMIT fixedWidthFontsChanged();
}

bool TerminalSettings::eventFilter(QObject *object, QEvent *event)
{
    if (object == qGuiApp && (event->type() == QEvent::ApplicationFontChange || event->type() == QEvent::ThemeChange)) {
        refreshFixedWidthFonts();

        Q_EMIT fontFamilyChanged();
        Q_EMIT fontPointSizeChanged();
    }

    return TerminalPreferences::eventFilter(object, event);
}

void TerminalSettings::setColorScheme(const QString &value)
{
    if (isValidTerminalPalette(value)) {
        TerminalPreferences::setColorScheme(value);
    }
}

void TerminalSettings::setScrollbackLines(int value)
{
    TerminalPreferences::setScrollbackLines(value < -1 ? 0 : value);
}

void TerminalSettings::setBackgroundOpacity(qreal value)
{
    if (std::isfinite(value)) {
        TerminalPreferences::setBackgroundOpacity(value);
    }
}

void TerminalSettings::setCursorStyle(const QString &value)
{
    if (SettingsConstraints::isValidCursorStyle(value)) {
        TerminalPreferences::setCursorStyle(value);
    }
}

void TerminalSettings::resetAppearanceDefaults()
{
    resetGroupDefaults(QStringLiteral("Appearance"));
}

void TerminalSettings::resetTerminalDefaults()
{
    resetGroupDefaults(QStringLiteral("Terminal"));
}

void TerminalSettings::resetBehaviorDefaults()
{
    resetGroupDefaults(QStringLiteral("Behavior"));
}

bool TerminalSettings::canResetAppearance() const
{
    return canResetGroup(QStringLiteral("Appearance"));
}

bool TerminalSettings::canResetTerminal() const
{
    return canResetGroup(QStringLiteral("Terminal"));
}

bool TerminalSettings::canResetBehavior() const
{
    return canResetGroup(QStringLiteral("Behavior"));
}

bool TerminalSettings::canResetGroup(const QString &name) const
{
    const auto preferences = items();

    return std::any_of(preferences.cbegin(), preferences.cend(), [&name](const KConfigSkeletonItem *item) {
        return item->group() == name && !item->isImmutable() && !item->isDefault();
    });
}

void TerminalSettings::resetGroupDefaults(const QString &name)
{
    const auto preferences = items();

    for (KConfigSkeletonItem *item : preferences) {
        if (item->group() == name && !item->isImmutable()) {
            item->setDefault();
        }
    }
}

}
