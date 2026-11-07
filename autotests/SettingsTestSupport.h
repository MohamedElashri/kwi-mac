// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "settings/TerminalSettings.h"

inline void resetSettingsDefaults(Kwi::TerminalSettings &settings)
{
    settings.resetAppearanceDefaults();
    settings.resetTerminalDefaults();
    settings.resetBehaviorDefaults();
}
