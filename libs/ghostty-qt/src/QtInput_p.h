// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QByteArray>
#include <Qt>
#include <cstdint>
#include <ghostty/vt.h>

class QKeyEvent;

namespace GhosttyQt
{
struct KeyInput {
    GhosttyKey key{GHOSTTY_KEY_UNIDENTIFIED};
    GhosttyKeyAction action{GHOSTTY_KEY_ACTION_PRESS};
    GhosttyMods modifiers{0};
    GhosttyMods consumedModifiers{0};
    std::uint32_t codepoint{0};
    QByteArray text;
};

[[nodiscard]] KeyInput keyInput(const QKeyEvent &event, bool optionAsAlt = false);
[[nodiscard]] Qt::KeyboardModifiers terminalModifiers(Qt::KeyboardModifiers modifiers);
[[nodiscard]] GhosttyMods ghosttyModifiers(Qt::KeyboardModifiers modifiers);
}
