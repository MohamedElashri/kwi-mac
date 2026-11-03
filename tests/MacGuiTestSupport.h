// SPDX-FileCopyrightText: 2026 Kwi contributors
// SPDX-License-Identifier: CC0-1.0
#pragma once

void activateMacTestApplication();
void recordMacTestProcess();
class QWindow;
bool activateMacTestWindow(QWindow *window, int timeout = 10000);

#include <QJsonObject>
#include <QString>
#include <Qt>
// Dispatch only to this test application's own Cocoa window; no global event injection.
void postMacTestKey(QWindow *window,
                    unsigned short keyCode,
                    Qt::KeyboardModifiers modifiers,
                    const QString &text,
                    const QString &unmodifiedText,
                    bool release = false,
                    bool capsLock = false);
void setMacTestPreedit(QWindow *window, const QString &text);
void commitMacTestText(QWindow *window, const QString &text);
QJsonObject macTestAccessibility(QWindow *window);
QJsonObject macTestFocusState(QWindow *window);
bool macTestSetAccessibilityCaret(QWindow *window, const QString &text, int offset);
bool macTestHasBlur(QWindow *window);
bool macTestVoiceOverEnabled();

void watchMacTestFullScreen(QWindow *window);
bool macTestFullScreenSettled(QWindow *window, bool entered);
void hideMacTestApplication(bool hidden, bool activateOnUnhide = true);
bool macTestApplicationHidden();
