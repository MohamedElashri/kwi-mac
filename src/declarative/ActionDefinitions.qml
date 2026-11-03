// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQml
import org.kde.ki18n
import org.kde.kirigami.actioncollection as AC

QtObject {
    id: definitions
    readonly property bool macOS: Qt.platform.os === "osx"
    readonly property AC.ActionCollection tabs: AC.ActionCollection {
        name: "org.kde.kwi.tabs"
        text: KI18n.i18nc("@title:group", "Windows and Tabs")

        AC.ActionData { name: "new_window"; text: KI18n.i18nc("@action", "New Window"); icon.name: "window-new"; defaultShortcut: definitions.macOS ? "Ctrl+N" : "Ctrl+Shift+N" }
        AC.ActionData { name: "new_tab"; text: KI18n.i18nc("@action", "New Tab"); icon.name: "tab-new"; defaultShortcut: definitions.macOS ? "Ctrl+T" : "Ctrl+Shift+T" }
        AC.ActionData { name: "close_tab"; text: KI18n.i18nc("@action", "Close Tab"); icon.name: "tab-close"; defaultShortcut: definitions.macOS ? "Ctrl+W" : "Ctrl+Shift+W" }
        AC.ActionData { name: "rename_tab"; text: KI18n.i18nc("@action", "Rename Tab"); icon.name: "edit-rename"; defaultShortcut: "Ctrl+Alt+S" }
        AC.ActionData { name: "next_tab"; text: KI18n.i18nc("@action", "Next Tab"); icon.name: "go-next"; defaultShortcut: definitions.macOS ? "Meta+Tab" : "Ctrl+PgDown" }
        AC.ActionData { name: "previous_tab"; text: KI18n.i18nc("@action", "Previous Tab"); icon.name: "go-previous"; defaultShortcut: definitions.macOS ? "Meta+Shift+Tab" : "Ctrl+PgUp" }
        AC.ActionData { name: "last_used_tab"; text: KI18n.i18nc("@action", "Last Used Tab"); icon.name: "tab-duplicate"; defaultShortcut: definitions.macOS ? "" : "Ctrl+Tab" }
        AC.ActionData { name: "move_tab_left"; text: KI18n.i18nc("@action", "Move Tab Left"); icon.name: "go-previous"; defaultShortcut: "Ctrl+Alt+Left" }
        AC.ActionData { name: "move_tab_right"; text: KI18n.i18nc("@action", "Move Tab Right"); icon.name: "go-next"; defaultShortcut: "Ctrl+Alt+Right" }
        AC.ActionData { name: "detach_tab"; text: KI18n.i18nc("@action", "Detach Tab"); icon.name: "window-new" }
        AC.ActionData { name: "search_tabs"; text: KI18n.i18nc("@action", "Search Tabs"); icon.name: "tab-search"; defaultShortcut: "Ctrl+Shift+O" }
        AC.ActionData { name: "tab_1"; text: KI18n.i18nc("@action", "Switch to Tab %1", 1); icon.name: "tab"; defaultShortcut: definitions.macOS ? "Ctrl+1" : "Alt+1" }
        AC.ActionData { name: "tab_2"; text: KI18n.i18nc("@action", "Switch to Tab %1", 2); icon.name: "tab"; defaultShortcut: definitions.macOS ? "Ctrl+2" : "Alt+2" }
        AC.ActionData { name: "tab_3"; text: KI18n.i18nc("@action", "Switch to Tab %1", 3); icon.name: "tab"; defaultShortcut: definitions.macOS ? "Ctrl+3" : "Alt+3" }
        AC.ActionData { name: "tab_4"; text: KI18n.i18nc("@action", "Switch to Tab %1", 4); icon.name: "tab"; defaultShortcut: definitions.macOS ? "Ctrl+4" : "Alt+4" }
        AC.ActionData { name: "tab_5"; text: KI18n.i18nc("@action", "Switch to Tab %1", 5); icon.name: "tab"; defaultShortcut: definitions.macOS ? "Ctrl+5" : "Alt+5" }
        AC.ActionData { name: "tab_6"; text: KI18n.i18nc("@action", "Switch to Tab %1", 6); icon.name: "tab"; defaultShortcut: definitions.macOS ? "Ctrl+6" : "Alt+6" }
        AC.ActionData { name: "tab_7"; text: KI18n.i18nc("@action", "Switch to Tab %1", 7); icon.name: "tab"; defaultShortcut: definitions.macOS ? "Ctrl+7" : "Alt+7" }
        AC.ActionData { name: "tab_8"; text: KI18n.i18nc("@action", "Switch to Tab %1", 8); icon.name: "tab"; defaultShortcut: definitions.macOS ? "Ctrl+8" : "Alt+8" }
        AC.ActionData { name: "tab_9"; text: KI18n.i18nc("@action", "Switch to Tab %1", 9); icon.name: "tab"; defaultShortcut: definitions.macOS ? "Ctrl+9" : "Alt+9" }
    }

    readonly property AC.ActionCollection editing: AC.ActionCollection {
        name: "org.kde.kwi.output"
        text: KI18n.i18nc("@title:group", "Text and Output")

        AC.ActionData { name: "copy"; text: KI18n.i18nc("@action", "Copy"); icon.name: "edit-copy"; defaultShortcut: definitions.macOS ? "Ctrl+C" : "Ctrl+Shift+C" }
        AC.ActionData { name: "paste"; text: KI18n.i18nc("@action", "Paste"); icon.name: "edit-paste"; defaultShortcut: definitions.macOS ? "Ctrl+V" : "Ctrl+Shift+V" }
        AC.ActionData { name: "paste_selection"; text: KI18n.i18nc("@action", "Paste Selection"); icon.name: "edit-paste"; defaultShortcut: "Shift+Ins" }
        AC.ActionData { name: "select_all"; text: KI18n.i18nc("@action", "Select All"); icon.name: "edit-select-all"; defaultShortcut: definitions.macOS ? "Ctrl+A" : "" }
        AC.ActionData { name: "selection_mode"; text: KI18n.i18nc("@action", "Keyboard Selection Mode"); icon.name: "edit-select"; defaultShortcut: "Ctrl+Shift+D" }
        AC.ActionData { name: "save_output"; text: KI18n.i18nc("@action", "Save Output As…"); icon.name: "document-save-as"; defaultShortcut: "Ctrl+Shift+S" }
        AC.ActionData { name: "clear_history"; text: KI18n.i18nc("@action", "Clear Scrollback"); icon.name: "edit-clear-history" }
        AC.ActionData { name: "reset_terminal"; text: KI18n.i18nc("@action", "Reset Terminal"); icon.name: "view-refresh"; defaultShortcut: "Ctrl+Shift+K"}
        AC.ActionData { name: "find"; text: KI18n.i18nc("@action", "Find…"); icon.name: "edit-find"; defaultShortcut: definitions.macOS ? "Ctrl+F" : "Ctrl+Shift+F" }
        AC.StandardActionData { standardAction: AC.StandardActionData.FindNext }
        AC.StandardActionData { standardAction: AC.StandardActionData.FindPrev }
        AC.ActionData { name: "previous_prompt"; text: KI18n.i18nc("@action", "Previous Prompt"); icon.name: "go-up" }
        AC.ActionData { name: "next_prompt"; text: KI18n.i18nc("@action", "Next Prompt"); icon.name: "go-down" }
        AC.ActionData { name: "copy_command_output"; text: KI18n.i18nc("@action", "Copy Last Command Output"); icon.name: "edit-copy" }
        AC.ActionData { name: "open_directory"; text: KI18n.i18nc("@action", "Open Current Directory"); icon.name: "folder-open" }
        AC.ActionData { name: "open_link_chooser"; text: KI18n.i18nc("@action", "Open Link…"); icon.name: "insert-link" }
        AC.ActionData { name: "read_only"; text: KI18n.i18nc("@action", "Read-Only Terminal"); icon.name: "object-locked" }
        AC.ActionData { name: "monitor_activity"; text: KI18n.i18nc("@action", "Monitor for Activity"); icon.name: "notifications" }
        AC.ActionData { name: "monitor_silence"; text: KI18n.i18nc("@action", "Monitor for Silence"); icon.name: "notifications" }
    }

    readonly property AC.ActionCollection application: AC.ActionCollection {
        name: "org.kde.kwi.application"
        text: KI18n.i18nc("@title:group", "Application")

        AC.ActionData { name: "fullscreen"; text: KI18n.i18nc("@action", "Full Screen"); icon.name: "view-fullscreen"; defaultShortcut: definitions.macOS ? "Ctrl+Meta+F" : "Ctrl+Shift+F11" }
        AC.StandardActionData { standardAction: AC.StandardActionData.ZoomIn }
        AC.StandardActionData { standardAction: AC.StandardActionData.ZoomOut }
        AC.ActionData { name: "reset_zoom"; text: KI18n.i18nc("@action", "Reset Zoom"); icon.name: "zoom-original"; defaultShortcut: "Ctrl+0" }
        AC.ActionData { name: "settings"; text: KI18n.i18nc("@action", "Configure Kwi…"); icon.name: "settings-configure"; defaultShortcut: definitions.macOS ? "Ctrl+," : "Ctrl+Shift+," }
        AC.StandardActionData { standardAction: AC.StandardActionData.ConfigureNotifications }
        AC.ActionData { name: "menu"; text: KI18n.i18nc("@action", "Application Menu"); icon.name: "application-menu"; defaultShortcut: "Shift+F10" }
        AC.ActionData { name: "quit"; text: KI18n.i18nc("@action", "Quit"); icon.name: "window-close"; defaultShortcut: definitions.macOS ? "Ctrl+Q" : "Ctrl+Shift+Q" }
    }
}
