// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

import QtQuick
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami
import org.kde.ki18n
import org.kde.kwi.terminal

FocusScope {
    id: root

    property alias session: terminal.session
    readonly property alias view: terminal

    signal multilinePasteRequested(Session origin, string text)
    signal contextMenuRequested(point position)
    signal linkActivated(url url)

    KI18nContext {
        id: translations
        translationDomain: "kwi-terminal"
    }

    TerminalView {
        id: terminal

        objectName: "terminalView"
        anchors.fill: parent
        anchors.rightMargin: scrollbar.width
        focus: true
        Accessible.role: Accessible.EditableText
        Accessible.name: session ? translations.i18nc("@info:accessibility", "Terminal: %1", (session.applicationTitle || session.launchedProgramName)) : translations.i18nc("@info:accessibility", "Terminal")
        Accessible.description: translations.i18nc("@info:accessibility", "Terminal output and command input.")

        onMultilinePasteRequested: (origin, text) => root.multilinePasteRequested(origin, text)
        onContextMenuRequested: position => root.contextMenuRequested(position)
        onLinkActivated: url => root.linkActivated(url)
    }

    Rectangle {
        x: terminal.x + terminal.selectionCursorRectangle.x
        y: terminal.y + terminal.selectionCursorRectangle.y
        width: terminal.selectionCursorRectangle.width
        height: terminal.selectionCursorRectangle.height
        visible: terminal.keyboardSelectionMode
        color: "transparent"
        border.color: Kirigami.Theme.highlightColor
        border.width: 2
        Accessible.ignored: true
    }

    Rectangle {
        objectName: "visualBell"
        anchors.fill: terminal
        color: "transparent"
        border.color: Kirigami.Theme.highlightColor
        border.width: 2
        visible: bellPulse.running
        Accessible.ignored: true

        Timer {
            id: bellPulse

            interval: 150
        }

        Connections {
            target: root.session

            function onBell(): void {
                if (!bellPulse.running) {
                    terminal.Accessible.announce(translations.i18nc("@info:accessibility", "Terminal bell"), Accessible.Polite);
                }

                bellPulse.restart();
            }
        }
    }

    Rectangle {
        // Keep the scrollbar opaque even when its style paints a translucent background.
        anchors.fill: scrollbar
        color: scrollbar.Kirigami.Theme.backgroundColor
        Accessible.ignored: true
    }

    Controls.ScrollBar {
        id: scrollbar

        objectName: "terminalScrollBar"
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.right: parent.right
        orientation: Qt.Vertical

        Kirigami.Theme.inherit: false
        Kirigami.Theme.backgroundColor: terminal.colorScheme === "system" ? root.Kirigami.Theme.backgroundColor : terminal.backgroundColor
        Kirigami.Theme.textColor: terminal.colorScheme === "system" ? root.Kirigami.Theme.textColor : terminal.foregroundColor

        // HACK: The desktop style hides size == 1 even with AlwaysOn. One representable
        // step below 1 still rounds to zero scroll range. Remove when the style honors AlwaysOn.
        size: Math.min(terminal.scrollSize, 1 - Number.EPSILON / 2)
        stepSize: terminal.scrollStepSize
        policy: Controls.ScrollBar.AlwaysOn
        interactive: true

        Accessible.ignored: terminal.scrollSize === 1

        // Desktop styles also move the bar through arrow/page buttons and
        // middle clicks, without setting ScrollBar.pressed.
        onPositionChanged: terminal.scrollTo(position)

        Binding {
            target: scrollbar
            property: "position"
            value: terminal.scrollPosition
        }

        WheelHandler {
            target: null
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad

            onWheel: event => terminal.scrollHistory(event.pixelDelta, event.angleDelta, event.modifiers)
        }
    }
}
