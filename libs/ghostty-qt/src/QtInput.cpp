// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "QtInput_p.h"
#include <QGuiApplication>
#include <QKeyEvent>
#ifdef Q_OS_MACOS
#include <Carbon/Carbon.h>
#elif defined(Q_OS_LINUX)
#include <linux/input-event-codes.h>
#endif
namespace GhosttyQt
{
namespace
{
constexpr GhosttyKey logicalKey(int key, Qt::KeyboardModifiers mods)
{
    if (mods.testFlag(Qt::KeypadModifier)) {
        if (key >= Qt::Key_0 && key <= Qt::Key_9) {
            return GhosttyKey(GHOSTTY_KEY_NUMPAD_0 + key - Qt::Key_0);
        }

        switch (key) {
        case Qt::Key_Enter:
        case Qt::Key_Return:
            return GHOSTTY_KEY_NUMPAD_ENTER;
        case Qt::Key_Plus:
            return GHOSTTY_KEY_NUMPAD_ADD;
        case Qt::Key_Minus:
            return GHOSTTY_KEY_NUMPAD_SUBTRACT;
        case Qt::Key_Asterisk:
            return GHOSTTY_KEY_NUMPAD_MULTIPLY;
        case Qt::Key_Slash:
            return GHOSTTY_KEY_NUMPAD_DIVIDE;
        case Qt::Key_Period:
        case Qt::Key_Comma:
            return GHOSTTY_KEY_NUMPAD_DECIMAL;
        case Qt::Key_Equal:
            return GHOSTTY_KEY_NUMPAD_EQUAL;
        case Qt::Key_Home:
            return GHOSTTY_KEY_NUMPAD_HOME;
        case Qt::Key_End:
            return GHOSTTY_KEY_NUMPAD_END;
        case Qt::Key_Insert:
            return GHOSTTY_KEY_NUMPAD_INSERT;
        case Qt::Key_Delete:
            return GHOSTTY_KEY_NUMPAD_DELETE;
        case Qt::Key_PageUp:
            return GHOSTTY_KEY_NUMPAD_PAGE_UP;
        case Qt::Key_PageDown:
            return GHOSTTY_KEY_NUMPAD_PAGE_DOWN;
        case Qt::Key_Up:
            return GHOSTTY_KEY_NUMPAD_UP;
        case Qt::Key_Down:
            return GHOSTTY_KEY_NUMPAD_DOWN;
        case Qt::Key_Left:
            return GHOSTTY_KEY_NUMPAD_LEFT;
        case Qt::Key_Right:
            return GHOSTTY_KEY_NUMPAD_RIGHT;
        case Qt::Key_Clear:
            return GHOSTTY_KEY_NUMPAD_BEGIN;
        default:
            break;
        }
    }

    if (key >= Qt::Key_A && key <= Qt::Key_Z) {
        return GhosttyKey(GHOSTTY_KEY_A + key - Qt::Key_A);
    }

    if (key >= Qt::Key_0 && key <= Qt::Key_9) {
        return GhosttyKey(GHOSTTY_KEY_DIGIT_0 + key - Qt::Key_0);
    }

    if (key >= Qt::Key_F1 && key <= Qt::Key_F25) {
        return GhosttyKey(GHOSTTY_KEY_F1 + key - Qt::Key_F1);
    }

    switch (key) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        return GHOSTTY_KEY_ENTER;
    case Qt::Key_Escape:
        return GHOSTTY_KEY_ESCAPE;
    case Qt::Key_Tab:
    case Qt::Key_Backtab:
        return GHOSTTY_KEY_TAB;
    case Qt::Key_Backspace:
        return GHOSTTY_KEY_BACKSPACE;
    case Qt::Key_Insert:
        return GHOSTTY_KEY_INSERT;
    case Qt::Key_Delete:
        return GHOSTTY_KEY_DELETE;
    case Qt::Key_Home:
        return GHOSTTY_KEY_HOME;
    case Qt::Key_End:
        return GHOSTTY_KEY_END;
    case Qt::Key_PageUp:
        return GHOSTTY_KEY_PAGE_UP;
    case Qt::Key_PageDown:
        return GHOSTTY_KEY_PAGE_DOWN;
    case Qt::Key_Up:
        return GHOSTTY_KEY_ARROW_UP;
    case Qt::Key_Down:
        return GHOSTTY_KEY_ARROW_DOWN;
    case Qt::Key_Left:
        return GHOSTTY_KEY_ARROW_LEFT;
    case Qt::Key_Right:
        return GHOSTTY_KEY_ARROW_RIGHT;
    case Qt::Key_Space:
        return GHOSTTY_KEY_SPACE;
    case Qt::Key_Minus:
        return GHOSTTY_KEY_MINUS;
    case Qt::Key_Equal:
    case Qt::Key_Plus:
        return GHOSTTY_KEY_EQUAL;
    case Qt::Key_BracketLeft:
        return GHOSTTY_KEY_BRACKET_LEFT;
    case Qt::Key_BracketRight:
        return GHOSTTY_KEY_BRACKET_RIGHT;
    case Qt::Key_Backslash:
        return GHOSTTY_KEY_BACKSLASH;
    case Qt::Key_Semicolon:
        return GHOSTTY_KEY_SEMICOLON;
    case Qt::Key_Apostrophe:
        return GHOSTTY_KEY_QUOTE;
    case Qt::Key_QuoteLeft:
        return GHOSTTY_KEY_BACKQUOTE;
    case Qt::Key_Comma:
        return GHOSTTY_KEY_COMMA;
    case Qt::Key_Period:
        return GHOSTTY_KEY_PERIOD;
    case Qt::Key_Slash:
        return GHOSTTY_KEY_SLASH;
    case Qt::Key_Shift:
        return GHOSTTY_KEY_SHIFT_LEFT;
    case Qt::Key_Control:
        return GHOSTTY_KEY_CONTROL_LEFT;
    case Qt::Key_Alt:
        return GHOSTTY_KEY_ALT_LEFT;
    case Qt::Key_AltGr:
        return GHOSTTY_KEY_ALT_RIGHT;
    case Qt::Key_Meta:
        return GHOSTTY_KEY_META_LEFT;
    case Qt::Key_CapsLock:
        return GHOSTTY_KEY_CAPS_LOCK;
    case Qt::Key_NumLock:
        return GHOSTTY_KEY_NUM_LOCK;
    case Qt::Key_Menu:
        return GHOSTTY_KEY_CONTEXT_MENU;
    case Qt::Key_Print:
        return GHOSTTY_KEY_PRINT_SCREEN;
    case Qt::Key_ScrollLock:
        return GHOSTTY_KEY_SCROLL_LOCK;
    case Qt::Key_Pause:
        return GHOSTTY_KEY_PAUSE;
    default:
        return GHOSTTY_KEY_UNIDENTIFIED;
    }
}

GhosttyKey physicalKey(const QKeyEvent &event)
{
    // TODO: Verify FreeBSD Wayland events and resolve FreeBSD X11 keys through XKB names.
    // Its X11 keycode tables can differ from Linux's evdev map.
#ifdef Q_OS_MACOS
    if (QGuiApplication::platformName() != QLatin1String("cocoa")) {
        return logicalKey(event.key(), event.modifiers());
    }

    // Cocoa flagsChanged events can arrive without native key metadata. Zero is
    // also the ANSI A keycode: never interpret a modifier transition as A.
    if (event.nativeVirtualKey() == 0 && event.key() >= Qt::Key_Shift && event.key() <= Qt::Key_ScrollLock) {
        if (event.key() == Qt::Key_Control || event.key() == Qt::Key_Meta) {
            const bool control{(event.key() == Qt::Key_Control) == QCoreApplication::testAttribute(Qt::AA_MacDontSwapCtrlAndMeta)};
            return control ? GHOSTTY_KEY_CONTROL_LEFT : GHOSTTY_KEY_META_LEFT;
        }
        return logicalKey(event.key(), event.modifiers());
    }

    // Cocoa keyCode is a physical position, independent of the active layout.
    switch (event.nativeVirtualKey()) {
    case kVK_ANSI_A:
        return GHOSTTY_KEY_A;
    case kVK_ANSI_B:
        return GHOSTTY_KEY_B;
    case kVK_ANSI_C:
        return GHOSTTY_KEY_C;
    case kVK_ANSI_D:
        return GHOSTTY_KEY_D;
    case kVK_ANSI_E:
        return GHOSTTY_KEY_E;
    case kVK_ANSI_F:
        return GHOSTTY_KEY_F;
    case kVK_ANSI_G:
        return GHOSTTY_KEY_G;
    case kVK_ANSI_H:
        return GHOSTTY_KEY_H;
    case kVK_ANSI_I:
        return GHOSTTY_KEY_I;
    case kVK_ANSI_J:
        return GHOSTTY_KEY_J;
    case kVK_ANSI_K:
        return GHOSTTY_KEY_K;
    case kVK_ANSI_L:
        return GHOSTTY_KEY_L;
    case kVK_ANSI_M:
        return GHOSTTY_KEY_M;
    case kVK_ANSI_N:
        return GHOSTTY_KEY_N;
    case kVK_ANSI_O:
        return GHOSTTY_KEY_O;
    case kVK_ANSI_P:
        return GHOSTTY_KEY_P;
    case kVK_ANSI_Q:
        return GHOSTTY_KEY_Q;
    case kVK_ANSI_R:
        return GHOSTTY_KEY_R;
    case kVK_ANSI_S:
        return GHOSTTY_KEY_S;
    case kVK_ANSI_T:
        return GHOSTTY_KEY_T;
    case kVK_ANSI_U:
        return GHOSTTY_KEY_U;
    case kVK_ANSI_V:
        return GHOSTTY_KEY_V;
    case kVK_ANSI_W:
        return GHOSTTY_KEY_W;
    case kVK_ANSI_X:
        return GHOSTTY_KEY_X;
    case kVK_ANSI_Y:
        return GHOSTTY_KEY_Y;
    case kVK_ANSI_Z:
        return GHOSTTY_KEY_Z;
    case kVK_ANSI_0:
        return GHOSTTY_KEY_DIGIT_0;
    case kVK_ANSI_1:
        return GHOSTTY_KEY_DIGIT_1;
    case kVK_ANSI_2:
        return GHOSTTY_KEY_DIGIT_2;
    case kVK_ANSI_3:
        return GHOSTTY_KEY_DIGIT_3;
    case kVK_ANSI_4:
        return GHOSTTY_KEY_DIGIT_4;
    case kVK_ANSI_5:
        return GHOSTTY_KEY_DIGIT_5;
    case kVK_ANSI_6:
        return GHOSTTY_KEY_DIGIT_6;
    case kVK_ANSI_7:
        return GHOSTTY_KEY_DIGIT_7;
    case kVK_ANSI_8:
        return GHOSTTY_KEY_DIGIT_8;
    case kVK_ANSI_9:
        return GHOSTTY_KEY_DIGIT_9;
    case kVK_F1:
        return GHOSTTY_KEY_F1;
    case kVK_F2:
        return GHOSTTY_KEY_F2;
    case kVK_F3:
        return GHOSTTY_KEY_F3;
    case kVK_F4:
        return GHOSTTY_KEY_F4;
    case kVK_F5:
        return GHOSTTY_KEY_F5;
    case kVK_F6:
        return GHOSTTY_KEY_F6;
    case kVK_F7:
        return GHOSTTY_KEY_F7;
    case kVK_F8:
        return GHOSTTY_KEY_F8;
    case kVK_F9:
        return GHOSTTY_KEY_F9;
    case kVK_F10:
        return GHOSTTY_KEY_F10;
    case kVK_F11:
        return GHOSTTY_KEY_F11;
    case kVK_F12:
        return GHOSTTY_KEY_F12;
    case kVK_F13:
        return GHOSTTY_KEY_F13;
    case kVK_F14:
        return GHOSTTY_KEY_F14;
    case kVK_F15:
        return GHOSTTY_KEY_F15;
    case kVK_F16:
        return GHOSTTY_KEY_F16;
    case kVK_F17:
        return GHOSTTY_KEY_F17;
    case kVK_F18:
        return GHOSTTY_KEY_F18;
    case kVK_F19:
        return GHOSTTY_KEY_F19;
    case kVK_F20:
        return GHOSTTY_KEY_F20;
    case kVK_ANSI_Keypad0:
        return GHOSTTY_KEY_NUMPAD_0;
    case kVK_ANSI_Keypad1:
        return GHOSTTY_KEY_NUMPAD_1;
    case kVK_ANSI_Keypad2:
        return GHOSTTY_KEY_NUMPAD_2;
    case kVK_ANSI_Keypad3:
        return GHOSTTY_KEY_NUMPAD_3;
    case kVK_ANSI_Keypad4:
        return GHOSTTY_KEY_NUMPAD_4;
    case kVK_ANSI_Keypad5:
        return GHOSTTY_KEY_NUMPAD_5;
    case kVK_ANSI_Keypad6:
        return GHOSTTY_KEY_NUMPAD_6;
    case kVK_ANSI_Keypad7:
        return GHOSTTY_KEY_NUMPAD_7;
    case kVK_ANSI_Keypad8:
        return GHOSTTY_KEY_NUMPAD_8;
    case kVK_ANSI_Keypad9:
        return GHOSTTY_KEY_NUMPAD_9;
    case kVK_ANSI_Equal:
        return GHOSTTY_KEY_EQUAL;
    case kVK_ANSI_Minus:
        return GHOSTTY_KEY_MINUS;
    case kVK_ANSI_RightBracket:
        return GHOSTTY_KEY_BRACKET_RIGHT;
    case kVK_ANSI_LeftBracket:
        return GHOSTTY_KEY_BRACKET_LEFT;
    case kVK_ANSI_Quote:
        return GHOSTTY_KEY_QUOTE;
    case kVK_ANSI_Semicolon:
        return GHOSTTY_KEY_SEMICOLON;
    case kVK_ANSI_Backslash:
        return GHOSTTY_KEY_BACKSLASH;
    case kVK_ANSI_Comma:
        return GHOSTTY_KEY_COMMA;
    case kVK_ANSI_Slash:
        return GHOSTTY_KEY_SLASH;
    case kVK_ANSI_Period:
        return GHOSTTY_KEY_PERIOD;
    case kVK_ANSI_Grave:
        return GHOSTTY_KEY_BACKQUOTE;
    case kVK_ISO_Section:
        return GHOSTTY_KEY_INTL_BACKSLASH;
    case kVK_JIS_Yen:
        return GHOSTTY_KEY_INTL_YEN;
    case kVK_JIS_Underscore:
        return GHOSTTY_KEY_INTL_RO;
    case kVK_JIS_KeypadComma:
        return GHOSTTY_KEY_NUMPAD_DECIMAL;
    case kVK_ANSI_KeypadDecimal:
        return GHOSTTY_KEY_NUMPAD_DECIMAL;
    case kVK_ANSI_KeypadMultiply:
        return GHOSTTY_KEY_NUMPAD_MULTIPLY;
    case kVK_ANSI_KeypadPlus:
        return GHOSTTY_KEY_NUMPAD_ADD;
    case kVK_ANSI_KeypadClear:
        return GHOSTTY_KEY_NUM_LOCK;
    case kVK_ANSI_KeypadDivide:
        return GHOSTTY_KEY_NUMPAD_DIVIDE;
    case kVK_ANSI_KeypadEnter:
        return GHOSTTY_KEY_NUMPAD_ENTER;
    case kVK_ANSI_KeypadMinus:
        return GHOSTTY_KEY_NUMPAD_SUBTRACT;
    case kVK_ANSI_KeypadEquals:
        return GHOSTTY_KEY_NUMPAD_EQUAL;
    case kVK_Return:
        return GHOSTTY_KEY_ENTER;
    case kVK_Tab:
        return GHOSTTY_KEY_TAB;
    case kVK_Space:
        return GHOSTTY_KEY_SPACE;
    case kVK_Delete:
        return GHOSTTY_KEY_BACKSPACE;
    case kVK_Escape:
        return GHOSTTY_KEY_ESCAPE;
    case kVK_Command:
        return GHOSTTY_KEY_META_LEFT;
    case kVK_RightCommand:
        return GHOSTTY_KEY_META_RIGHT;
    case kVK_Shift:
        return GHOSTTY_KEY_SHIFT_LEFT;
    case kVK_RightShift:
        return GHOSTTY_KEY_SHIFT_RIGHT;
    case kVK_CapsLock:
        return GHOSTTY_KEY_CAPS_LOCK;
    case kVK_Option:
        return GHOSTTY_KEY_ALT_LEFT;
    case kVK_RightOption:
        return GHOSTTY_KEY_ALT_RIGHT;
    case kVK_Control:
        return GHOSTTY_KEY_CONTROL_LEFT;
    case kVK_RightControl:
        return GHOSTTY_KEY_CONTROL_RIGHT;
    case kVK_Help:
        return GHOSTTY_KEY_INSERT;
    case kVK_Home:
        return GHOSTTY_KEY_HOME;
    case kVK_PageUp:
        return GHOSTTY_KEY_PAGE_UP;
    case kVK_ForwardDelete:
        return GHOSTTY_KEY_DELETE;
    case kVK_End:
        return GHOSTTY_KEY_END;
    case kVK_PageDown:
        return GHOSTTY_KEY_PAGE_DOWN;
    case kVK_LeftArrow:
        return GHOSTTY_KEY_ARROW_LEFT;
    case kVK_RightArrow:
        return GHOSTTY_KEY_ARROW_RIGHT;
    case kVK_DownArrow:
        return GHOSTTY_KEY_ARROW_DOWN;
    case kVK_UpArrow:
        return GHOSTTY_KEY_ARROW_UP;
    case kVK_ContextualMenu:
        return GHOSTTY_KEY_CONTEXT_MENU;
    default:
        return logicalKey(event.key(), event.modifiers());
    }
#elif defined(Q_OS_LINUX)
    // Qt Wayland adds 8 to evdev keycodes; Qt XCB passes XKB keycodes through.
    // Linux's evdev XKB map uses the same offset. Keep synthetic events and
    // other platform plugins on the logical fallback.
    static const bool usesEvdev =
        QGuiApplication::platformName() == QLatin1String("xcb") || QGuiApplication::platformName().startsWith(QLatin1String("wayland"));

    if (!usesEvdev || event.nativeScanCode() < 8 || event.modifiers().testFlag(Qt::KeypadModifier)) {
        return logicalKey(event.key(), event.modifiers());
    }

    switch (event.nativeScanCode() - 8) {
    case KEY_A:
        return GHOSTTY_KEY_A;
    case KEY_B:
        return GHOSTTY_KEY_B;
    case KEY_C:
        return GHOSTTY_KEY_C;
    case KEY_D:
        return GHOSTTY_KEY_D;
    case KEY_E:
        return GHOSTTY_KEY_E;
    case KEY_F:
        return GHOSTTY_KEY_F;
    case KEY_G:
        return GHOSTTY_KEY_G;
    case KEY_H:
        return GHOSTTY_KEY_H;
    case KEY_I:
        return GHOSTTY_KEY_I;
    case KEY_J:
        return GHOSTTY_KEY_J;
    case KEY_K:
        return GHOSTTY_KEY_K;
    case KEY_L:
        return GHOSTTY_KEY_L;
    case KEY_M:
        return GHOSTTY_KEY_M;
    case KEY_N:
        return GHOSTTY_KEY_N;
    case KEY_O:
        return GHOSTTY_KEY_O;
    case KEY_P:
        return GHOSTTY_KEY_P;
    case KEY_Q:
        return GHOSTTY_KEY_Q;
    case KEY_R:
        return GHOSTTY_KEY_R;
    case KEY_S:
        return GHOSTTY_KEY_S;
    case KEY_T:
        return GHOSTTY_KEY_T;
    case KEY_U:
        return GHOSTTY_KEY_U;
    case KEY_V:
        return GHOSTTY_KEY_V;
    case KEY_W:
        return GHOSTTY_KEY_W;
    case KEY_X:
        return GHOSTTY_KEY_X;
    case KEY_Y:
        return GHOSTTY_KEY_Y;
    case KEY_Z:
        return GHOSTTY_KEY_Z;
    case KEY_0:
        return GHOSTTY_KEY_DIGIT_0;
    case KEY_1:
        return GHOSTTY_KEY_DIGIT_1;
    case KEY_2:
        return GHOSTTY_KEY_DIGIT_2;
    case KEY_3:
        return GHOSTTY_KEY_DIGIT_3;
    case KEY_4:
        return GHOSTTY_KEY_DIGIT_4;
    case KEY_5:
        return GHOSTTY_KEY_DIGIT_5;
    case KEY_6:
        return GHOSTTY_KEY_DIGIT_6;
    case KEY_7:
        return GHOSTTY_KEY_DIGIT_7;
    case KEY_8:
        return GHOSTTY_KEY_DIGIT_8;
    case KEY_9:
        return GHOSTTY_KEY_DIGIT_9;
    case KEY_MINUS:
        return GHOSTTY_KEY_MINUS;
    case KEY_EQUAL:
        return GHOSTTY_KEY_EQUAL;
    case KEY_LEFTBRACE:
        return GHOSTTY_KEY_BRACKET_LEFT;
    case KEY_RIGHTBRACE:
        return GHOSTTY_KEY_BRACKET_RIGHT;
    case KEY_BACKSLASH:
        return GHOSTTY_KEY_BACKSLASH;
    case KEY_SEMICOLON:
        return GHOSTTY_KEY_SEMICOLON;
    case KEY_APOSTROPHE:
        return GHOSTTY_KEY_QUOTE;
    case KEY_GRAVE:
        return GHOSTTY_KEY_BACKQUOTE;
    case KEY_COMMA:
        return GHOSTTY_KEY_COMMA;
    case KEY_DOT:
        return GHOSTTY_KEY_PERIOD;
    case KEY_SLASH:
        return GHOSTTY_KEY_SLASH;
    case KEY_102ND:
        return GHOSTTY_KEY_INTL_BACKSLASH;
    case KEY_RO:
        return GHOSTTY_KEY_INTL_RO;
    case KEY_YEN:
        return GHOSTTY_KEY_INTL_YEN;
    case KEY_LEFTSHIFT:
        return GHOSTTY_KEY_SHIFT_LEFT;
    case KEY_RIGHTSHIFT:
        return GHOSTTY_KEY_SHIFT_RIGHT;
    case KEY_LEFTCTRL:
        return GHOSTTY_KEY_CONTROL_LEFT;
    case KEY_RIGHTCTRL:
        return GHOSTTY_KEY_CONTROL_RIGHT;
    case KEY_LEFTALT:
        return GHOSTTY_KEY_ALT_LEFT;
    case KEY_RIGHTALT:
        return GHOSTTY_KEY_ALT_RIGHT;
    case KEY_LEFTMETA:
        return GHOSTTY_KEY_META_LEFT;
    case KEY_RIGHTMETA:
        return GHOSTTY_KEY_META_RIGHT;
    default:
        return logicalKey(event.key(), event.modifiers());
    }
#else
    return logicalKey(event.key(), event.modifiers());
#endif
}
}

Qt::KeyboardModifiers terminalModifiers(Qt::KeyboardModifiers mods)
{
#ifdef Q_OS_MACOS
    // Respect embedding hosts: never change a process-global Qt input attribute.
    if (!QCoreApplication::testAttribute(Qt::AA_MacDontSwapCtrlAndMeta)) {
        const bool command{mods.testFlag(Qt::ControlModifier)};
        const bool control{mods.testFlag(Qt::MetaModifier)};
        mods.setFlag(Qt::ControlModifier, control);
        mods.setFlag(Qt::MetaModifier, command);
    }
#endif
    return mods;
}

namespace
{
GhosttyMods encodeModifiers(Qt::KeyboardModifiers mods)
{
    return (mods.testFlag(Qt::ShiftModifier) ? GHOSTTY_MODS_SHIFT : 0) | (mods.testFlag(Qt::ControlModifier) ? GHOSTTY_MODS_CTRL : 0)
        | (mods.testFlag(Qt::AltModifier) ? GHOSTTY_MODS_ALT : 0) | (mods.testFlag(Qt::MetaModifier) ? GHOSTTY_MODS_SUPER : 0);
}

}

GhosttyMods ghosttyModifiers(Qt::KeyboardModifiers mods)
{
    return encodeModifiers(terminalModifiers(mods));
}

KeyInput keyInput(const QKeyEvent &event, bool optionAsAlt)
{
    KeyInput result;
    result.key = physicalKey(event);
    result.action = event.type() == QEvent::KeyRelease ? GHOSTTY_KEY_ACTION_RELEASE
        : event.isAutoRepeat()                         ? GHOSTTY_KEY_ACTION_REPEAT
                                                       : GHOSTTY_KEY_ACTION_PRESS;
    auto modifiers = terminalModifiers(event.modifiers());

    if (event.key() == Qt::Key_Backtab)
        modifiers |= Qt::ShiftModifier;

    // Qt already applies the keyboard layout and AltGr to the event text.
    if (modifiers.testFlag(Qt::GroupSwitchModifier))
        modifiers &= ~(Qt::ControlModifier | Qt::AltModifier);

    result.modifiers = encodeModifiers(modifiers);
    result.text = event.text().toUtf8();

    if (event.key() >= 0x20 && event.key() <= 0x10ffff)
        result.codepoint = QChar::toLower(char32_t(event.key()));
    else if (!event.text().isEmpty())
        result.codepoint = event.text().toUcs4().first();
#ifdef Q_OS_MACOS
    // Cocoa supplies the layout-dependent key from charactersIgnoringModifiers
    // even for Option dead keys. Use that key, not the Option-composed text,
    // when the focused view explicitly reserves Option for terminal Alt.
    if (optionAsAlt && modifiers.testFlag(Qt::AltModifier) && event.key() >= 0x20 && event.key() <= 0x10ffff) {
        char32_t character{char32_t(event.key())};
        const bool capsLock{bool(event.nativeModifiers() & (1UL << 16))};
        if (modifiers.testFlag(Qt::ShiftModifier) == capsLock)
            character = QChar::toLower(character);
        result.text = QString::fromUcs4(&character, 1).toUtf8();
    }
#else
    Q_UNUSED(optionAsAlt)
#endif
    const bool printable{!result.text.isEmpty() && QString::fromUtf8(result.text).front().isPrint()};

    if (printable && !modifiers.testFlag(Qt::ControlModifier))
        result.consumedModifiers = encodeModifiers(modifiers & Qt::ShiftModifier);
#ifdef Q_OS_MACOS
    // Option produces composed text by default. Do not prefix that text with ESC.
    // Terminal Alt deliberately keeps that modifier unconsumed.
    if (!optionAsAlt && printable && !modifiers.testFlag(Qt::ControlModifier) && !modifiers.testFlag(Qt::MetaModifier))
        result.consumedModifiers |= encodeModifiers(modifiers & Qt::AltModifier);
#endif

    // Ghostty needs printable key text rather than Qt's C0 text for Ctrl+letter.
    if (modifiers.testFlag(Qt::ControlModifier) && result.text.size() == 1 && quint8(result.text.at(0)) < 0x20 && result.codepoint >= 0x20
        && result.codepoint <= 0x10ffff) {
        char32_t printableCodepoint{result.codepoint};

        if (modifiers.testFlag(Qt::ShiftModifier) && printableCodepoint >= U'a' && printableCodepoint <= U'z')
            printableCodepoint -= U'a' - U'A';

        result.text = QString::fromUcs4(&printableCodepoint, 1).toUtf8();
    }

    return result;
}
}
