// SPDX-FileCopyrightText: 2026 Kwi contributors
// SPDX-License-Identifier: CC0-1.0
#include "MacGuiTestSupport.h"
#import <AppKit/AppKit.h>
#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTest>
#include <QWindow>

void activateMacTestApplication()
{
    // A CLI-launched GUI test may lose app activation after its last window
    // closes. QWindow::requestActivate only makes a window key. Claim the
    // foreground explicitly for the owned test app before each GUI case.
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    [NSApp activateIgnoringOtherApps:YES];
}

void recordMacTestProcess()
{
    const auto path = qEnvironmentVariable("KWI_GUI_TEST_PID_FILE");
    if (path.isEmpty())
        return;
    QFile file{path};
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        qFatal("Cannot create the owned GUI test PID marker");
    file.write(QByteArray::number(QCoreApplication::applicationPid()));
}

bool activateMacTestWindow(QWindow *window, int timeout)
{
    if (!QTest::qWaitForWindowExposed(window, timeout))
        return false;
    const QString activationPath = qEnvironmentVariable("KWI_GUI_TEST_ACTIVATION_FILE");
    if (!activationPath.isEmpty()) {
        static qint64 sequence{0};
        const qint64 token = ++sequence;
        const qint64 pid = QCoreApplication::applicationPid();
        QSaveFile request{activationPath};
        if (!request.open(QIODevice::WriteOnly))
            return false;
        request.write(QJsonDocument(QJsonObject{{QStringLiteral("pid"), pid}, {QStringLiteral("sequence"), token}}).toJson(QJsonDocument::Compact));
        if (!request.commit())
            return false;
        QElapsedTimer elapsed;
        elapsed.start();
        bool acknowledged = false;
        while (elapsed.elapsed() < timeout) {
            QFile ack{activationPath + QStringLiteral(".ack")};
            if (ack.open(QIODevice::ReadOnly)) {
                const auto data = QJsonDocument::fromJson(ack.readAll()).object();
                acknowledged = data.value(QStringLiteral("pid")).toInteger() == pid && data.value(QStringLiteral("sequence")).toInteger() == token
                    && data.value(QStringLiteral("success")).toBool();
                if (acknowledged)
                    break;
            }
            QTest::qWait(10);
        }
        if (!acknowledged)
            return false;
    }
    activateMacTestApplication();
    window->raise();
    window->requestActivate();
    if (qEnvironmentVariableIsSet("KWI_GUI_FOCUS_DIAGNOSTIC"))
        timeout = 60000;
    const bool active = QTest::qWaitForWindowActive(window, timeout);
    if (!active) {
        qWarning() << "Native focus state" << macTestFocusState(window);
        qWarning() << "GUI focus failure" << "application state" << QGuiApplication::applicationState() << "target" << window << "focus"
                   << QGuiApplication::focusWindow() << "modal" << QGuiApplication::modalWindow();
        NSLog(@"Native test activation: active=%d policy=%ld key=%@ main=%@", NSApp.active, long(NSApp.activationPolicy), NSApp.keyWindow, NSApp.mainWindow);
        for (auto *candidate : QGuiApplication::allWindows())
            qWarning() << "Owned window" << candidate << candidate->title() << candidate->isVisible() << candidate->isActive() << candidate->flags();
    }
    return active;
}

void postMacTestKey(QWindow *window,
                    unsigned short keyCode,
                    Qt::KeyboardModifiers modifiers,
                    const QString &text,
                    const QString &unmodifiedText,
                    bool release,
                    bool capsLock)
{
    NSView *view = reinterpret_cast<NSView *>(window->winId());
    NSEventModifierFlags flags = capsLock ? NSEventModifierFlagCapsLock : 0;
    if (modifiers.testFlag(Qt::ControlModifier))
        flags |= NSEventModifierFlagCommand;
    if (modifiers.testFlag(Qt::MetaModifier))
        flags |= NSEventModifierFlagControl;
    if (modifiers.testFlag(Qt::AltModifier))
        flags |= NSEventModifierFlagOption;
    if (modifiers.testFlag(Qt::ShiftModifier))
        flags |= NSEventModifierFlagShift;
    NSEvent *event = [NSEvent keyEventWithType:release ? NSEventTypeKeyUp : NSEventTypeKeyDown
                                      location:NSZeroPoint
                                 modifierFlags:flags
                                     timestamp:NSProcessInfo.processInfo.systemUptime
                                  windowNumber:view.window.windowNumber
                                       context:nil
                                    characters:text.toNSString()
                   charactersIgnoringModifiers:unmodifiedText.toNSString()
                                     isARepeat:NO
                                       keyCode:keyCode];
    [NSApp postEvent:event atStart:NO];
}

void setMacTestPreedit(QWindow *window, const QString &text)
{
    id<NSTextInputClient> view = reinterpret_cast<id<NSTextInputClient>>(window->winId());
    [[(NSView *)view inputContext] activate];
    [view setMarkedText:text.toNSString() selectedRange:NSMakeRange(text.size(), 0) replacementRange:NSMakeRange(NSNotFound, 0)];
}

void commitMacTestText(QWindow *window, const QString &text)
{
    id<NSTextInputClient> view = reinterpret_cast<id<NSTextInputClient>>(window->winId());
    [view insertText:text.toNSString() replacementRange:NSMakeRange(NSNotFound, 0)];
}

QJsonObject macTestFocusState(QWindow *window)
{
    NSView *view = reinterpret_cast<NSView *>(window->winId());
    NSRunningApplication *front = NSWorkspace.sharedWorkspace.frontmostApplication;
    return {{QStringLiteral("native_app_active"), bool(NSApp.active)},
            {QStringLiteral("native_window_minimized"), bool(view.window.miniaturized)},
            {QStringLiteral("native_window_key"), bool(view.window.keyWindow)},
            {QStringLiteral("native_key_window_matches"), NSApp.keyWindow == view.window},
            {QStringLiteral("frontmost_bundle_id"), QString::fromNSString(front.bundleIdentifier)},
            {QStringLiteral("frontmost_pid"), int(front.processIdentifier)}};
}

QJsonObject macTestAccessibility(QWindow *window)
{
    NSView *view = reinterpret_cast<NSView *>(window->winId());
    QJsonArray entries;
    int remaining = 256;
    const auto visit = [&](auto &&self, id<NSAccessibility> node, int depth) -> void {
        if (!node || depth > 12 || --remaining < 0)
            return;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        // AppKit window proxies and Qt bridge elements may implement the
        // attribute protocol rather than the modern property protocol.
        const auto attribute = [&](NSString *name) -> id {
            return [node respondsToSelector:@selector(accessibilityAttributeValue:)] ? [(id)node accessibilityAttributeValue:name] : nil;
        };
        NSString *role = [node respondsToSelector:@selector(accessibilityRole)] ? node.accessibilityRole : attribute(NSAccessibilityRoleAttribute);
        NSString *label = [node respondsToSelector:@selector(accessibilityLabel)] ? node.accessibilityLabel : attribute(NSAccessibilityDescriptionAttribute);
        id value = [node respondsToSelector:@selector(accessibilityValue)] ? node.accessibilityValue : attribute(NSAccessibilityValueAttribute);
        entries.append(QJsonObject{{QStringLiteral("role"), QString::fromNSString(role)},
                                   {QStringLiteral("label"), QString::fromNSString(label)},
                                   {QStringLiteral("value"), [value isKindOfClass:NSString.class] ? QString::fromNSString(value) : QString{}}});
        NSArray *children =
            [node respondsToSelector:@selector(accessibilityChildren)] ? node.accessibilityChildren : attribute(NSAccessibilityChildrenAttribute);
#pragma clang diagnostic pop
        for (id child in children)
            self(self, child, depth + 1);
    };
    visit(visit, view.window, 0);
    return {{QStringLiteral("entries"), entries},
            {QStringLiteral("bounded_nodes"), 256},
            {QStringLiteral("voiceover_running"), NSWorkspace.sharedWorkspace.voiceOverEnabled}};
}

bool macTestSetAccessibilityCaret(QWindow *window, const QString &text, int offset)
{
    NSView *view = reinterpret_cast<NSView *>(window->winId());
    int remaining = 256;
    const auto visit = [&](auto &&self, id node, int depth) -> bool {
        if (!node || depth > 12 || --remaining < 0)
            return false;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        const auto attribute = [&](NSString *name) -> id {
            return [node respondsToSelector:@selector(accessibilityAttributeValue:)] ? [node accessibilityAttributeValue:name] : nil;
        };
        NSString *role = [node respondsToSelector:@selector(accessibilityRole)] ? [node accessibilityRole] : attribute(NSAccessibilityRoleAttribute);
        id value = [node respondsToSelector:@selector(accessibilityValue)] ? [node accessibilityValue] : attribute(NSAccessibilityValueAttribute);
        if ([role isEqualToString:NSAccessibilityTextAreaRole] && [value isKindOfClass:NSString.class] && QString::fromNSString(value).contains(text)) {
            const NSRange range = NSMakeRange(offset, 0);
            if ([node respondsToSelector:@selector(accessibilitySetValue:forAttribute:)]) {
                [node accessibilitySetValue:[NSValue valueWithRange:range] forAttribute:NSAccessibilitySelectedTextRangeAttribute];
                return true;
            }
            if ([node respondsToSelector:@selector(setAccessibilitySelectedTextRange:)]) {
                [node setAccessibilitySelectedTextRange:range];
                return true;
            }
        }
        NSArray *children =
            [node respondsToSelector:@selector(accessibilityChildren)] ? [node accessibilityChildren] : attribute(NSAccessibilityChildrenAttribute);
#pragma clang diagnostic pop
        for (id child in children)
            if (self(self, child, depth + 1))
                return true;
        return false;
    };
    return visit(visit, view.window, 0);
}

bool macTestHasBlur(QWindow *window)
{
    NSView *content = reinterpret_cast<NSView *>(window->winId());
    for (NSView *candidate in content.superview.subviews)
        if ([candidate isKindOfClass:NSVisualEffectView.class] && [candidate.identifier isEqualToString:@"org.kde.kwi.background-blur"])
            return NSEqualRects(candidate.frame, content.frame);
    return false;
}

bool macTestVoiceOverEnabled()
{
    return NSWorkspace.sharedWorkspace.voiceOverEnabled;
}

namespace
{
QHash<NSInteger, int> fullScreenStates;
}

void watchMacTestFullScreen(QWindow *window)
{
    static const bool installed = [] {
        auto *center = NSNotificationCenter.defaultCenter;
        [center addObserverForName:NSWindowWillEnterFullScreenNotification
                            object:nil
                             queue:nil
                        usingBlock:^(NSNotification *note) { fullScreenStates[[(NSWindow *)note.object windowNumber]] = 2; }];
        [center addObserverForName:NSWindowDidEnterFullScreenNotification
                            object:nil
                             queue:nil
                        usingBlock:^(NSNotification *note) { fullScreenStates[[(NSWindow *)note.object windowNumber]] = 1; }];
        [center addObserverForName:NSWindowWillExitFullScreenNotification
                            object:nil
                             queue:nil
                        usingBlock:^(NSNotification *note) { fullScreenStates[[(NSWindow *)note.object windowNumber]] = 2; }];
        [center addObserverForName:NSWindowDidExitFullScreenNotification
                            object:nil
                             queue:nil
                        usingBlock:^(NSNotification *note) { fullScreenStates[[(NSWindow *)note.object windowNumber]] = 0; }];
        return true;
    }();
    Q_UNUSED(installed)
    NSView *view = reinterpret_cast<NSView *>(window->winId());
    fullScreenStates[view.window.windowNumber] = (view.window.styleMask & NSWindowStyleMaskFullScreen) ? 1 : 0;
}

bool macTestFullScreenSettled(QWindow *window, bool entered)
{
    NSView *view = reinterpret_cast<NSView *>(window->winId());
    return fullScreenStates.value(view.window.windowNumber, -1) == (entered ? 1 : 0);
}

void hideMacTestApplication(bool hidden, bool activateOnUnhide)
{
    if (hidden)
        [NSApp hide:nil];
    else if (activateOnUnhide) {
        [NSApp unhide:nil];
        activateMacTestApplication();
    } else
        [NSApp unhideWithoutActivation];
}

bool macTestApplicationHidden()
{
    return NSApp.hidden;
}
