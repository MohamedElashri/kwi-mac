// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kwi

FocusScope {
    id: root

    property TerminalSession session: null
    property string hostError
    property bool findVisible: false
    readonly property alias terminalView: pane.view

    function showFind(): void {
        findVisible = true;
        findBar.focusSearch();
    }

    function hideFind(): void {
        findVisible = false;
        findBar.cancelPendingSearch();
        pane.terminal.clearSearch();
        pane.terminal.forceActiveFocus();
    }

    function findNext(forward: bool): void {
        if (findVisible)
            findBar.search(forward);
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        TerminalSearchBar {
            id: findBar

            Layout.fillWidth: true
            visible: root.findVisible
            terminal: pane.terminal
            tabBarAtTop: root.session && !root.session.settings.tabBarAtBottom

            onCloseRequested: root.hideFind()
        }

        Kirigami.InlineMessage {
            objectName: "embeddedErrorMessage"
            Layout.fillWidth: true
            visible: text.length > 0
            type: Kirigami.MessageType.Error
            text: TextFormatting.plainTextToHtml(root.hostError || (root.session ? root.session.errorString : ""))
        }

        TerminalPane {
            id: pane

            Layout.fillWidth: true
            Layout.fillHeight: true
            applicationSession: root.session
        }
    }
}
