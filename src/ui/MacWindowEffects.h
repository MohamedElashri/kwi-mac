// SPDX-FileCopyrightText: 2026 Kwi contributors
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
#pragma once
class QWindow;
namespace Kwi
{
void configureMacPresentation(QWindow *window);
void setMacBackgroundBlur(QWindow *window, bool enabled);
}
