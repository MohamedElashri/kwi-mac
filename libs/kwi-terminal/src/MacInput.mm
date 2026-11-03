// SPDX-FileCopyrightText: 2026 Kwi contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "MacInput_p.h"
#import <AppKit/AppKit.h>
#include <QWindow>

bool Kwi::nativeOptionKeyEvent(QWindow *window)
{
    NSEvent *event = NSApp.currentEvent;
    if (!window || !event || (event.type != NSEventTypeKeyDown && event.type != NSEventTypeKeyUp))
        return false;
    NSView *view = reinterpret_cast<NSView *>(window->winId());
    return event.window == view.window && (event.modifierFlags & NSEventModifierFlagOption);
}
