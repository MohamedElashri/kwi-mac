// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QStringView>
#include <QtTypes>

namespace Kwi::SettingsConstraints
{
#ifdef Q_OS_MACOS
// Qt's system fixed-font size is too small for a terminal on macOS. Keep the
// platform's fixed-width family while giving fresh/reset profiles a readable size.
inline constexpr bool DefaultUseSystemFont{false};
inline constexpr qreal DefaultFontPointSize{14.0};
#else
inline constexpr bool DefaultUseSystemFont{true};
inline constexpr qreal DefaultFontPointSize{0.0};
#endif
inline constexpr qreal MinimumFontPointSize{6.0};
inline constexpr qreal MaximumFontPointSize{48.0};
inline constexpr int MaximumScrollbackLines{100000};
inline constexpr int MinimumSilenceSeconds{1};
inline constexpr int MaximumSilenceSeconds{3600};

[[nodiscard]] inline bool isValidCursorStyle(QStringView style)
{
    return style == u"block" || style == u"beam" || style == u"underline";
}
}
