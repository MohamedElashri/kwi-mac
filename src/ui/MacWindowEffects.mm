// SPDX-FileCopyrightText: 2026 Kwi contributors
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
#include "MacWindowEffects.h"
#import <AppKit/AppKit.h>
#include <QWindow>
#import <QuartzCore/CAMetalLayer.h>
#include <qpa/qplatformwindow_p.h>

void Kwi::configureMacPresentation(QWindow *window)
{
    if (!window)
        return;
    auto *cocoa = window->nativeInterface<QNativeInterface::Private::QCocoaWindow>();
    CALayer *layer = cocoa ? cocoa->contentLayer() : nil;
    if ([layer isKindOfClass:CAMetalLayer.class]) {
        // Bound presentation queueing while retaining Qt's Metal renderer and
        // display synchronization. The Darwin reader yields useful batches
        // promptly so waiting for a drawable does not throttle PTY intake.
        CAMetalLayer *metal = static_cast<CAMetalLayer *>(layer);
        metal.maximumDrawableCount = 2;
    }
}

void Kwi::setMacBackgroundBlur(QWindow *window, bool enabled)
{
    if (!window)
        return;
    NSView *content = reinterpret_cast<NSView *>(window->winId());
    NSView *container = content.superview;
    if (!container)
        return;
    NSString *identifier = @"org.kde.kwi.background-blur";
    NSVisualEffectView *effect = nil;
    for (NSView *candidate in container.subviews) {
        if ([candidate isKindOfClass:NSVisualEffectView.class] && [candidate.identifier isEqualToString:identifier]) {
            effect = (NSVisualEffectView *)candidate;
            break;
        }
    }
    if (!enabled) {
        [effect removeFromSuperview];
        return;
    }
    if (!effect) {
        effect = [[NSVisualEffectView alloc] initWithFrame:content.frame];
        effect.identifier = identifier;
        effect.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        effect.blendingMode = NSVisualEffectBlendingModeBehindWindow;
        effect.material = NSVisualEffectMaterialUnderWindowBackground;
        effect.state = NSVisualEffectStateFollowsWindowActiveState;
        // Keep Qt's content view and Metal layer intact. AppKit composites the
        // backdrop below the existing transparent content, including on resize.
        [container addSubview:effect positioned:NSWindowBelow relativeTo:content];
        [effect release];
    }
    effect.frame = content.frame;
}
