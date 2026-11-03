// SPDX-FileCopyrightText: 2021 Felipe Kinoshita <kinofhek@gmail.com>
// SPDX-FileCopyrightText: 2023 Michael Lang <criticaltemp@protonmail.com>
// SPDX-FileCopyrightText: 2025 Devin Lin <devin@kde.org>
// SPDX-FileCopyrightText: 2026 Kwi contributors
// SPDX-License-Identifier: LGPL-2.0-or-later

import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import QtQuick.Templates as T
import org.kde.ki18n
import org.kde.kirigami as Kirigami
import org.kde.kwi
import org.kde.kwi.terminal as Terminal

Controls.TabButton {
    id: control

    required property int index
    required property TerminalSession session
    readonly property string sessionTitle: session ? session.title : ""
    property bool showStatusIcons: true
    property bool draggable: false
    property bool dragging: false
    property int tabPosition: Controls.TabBar.Header
    readonly property bool dragGestureActive: tabDrag.active
    readonly property string monitoringDescription: !session ? "" : session.monitorActivity && session.monitorSilence ? KI18n.i18nc("@info:tooltip", "Monitoring activity and silence") : session.monitorActivity ? KI18n.i18nc("@info:tooltip", "Monitoring activity") : session.monitorSilence ? KI18n.i18nc("@info:tooltip", "Monitoring silence") : ""
    readonly property bool hasProgress: session && !session.finished && session.progressState !== Terminal.Session.None
    readonly property string progressDescription: {
        if (!hasProgress)
            return "";

        switch (session.progressState) {
        case Terminal.Session.Normal:
            return session.progressPercent >= 0 ? KI18n.i18nc("@info:tooltip", "Progress: %1%", session.progressPercent) : KI18n.i18nc("@info:tooltip", "In progress");
        case Terminal.Session.Error:
            return session.progressPercent >= 0 ? KI18n.i18nc("@info:tooltip", "Error: %1%", session.progressPercent) : KI18n.i18nc("@info:tooltip", "Error");
        case Terminal.Session.Indeterminate:
            return KI18n.i18nc("@info:tooltip", "In progress");
        case Terminal.Session.Paused:
            return session.progressPercent >= 0 ? KI18n.i18nc("@info:tooltip", "Paused: %1%", session.progressPercent) : KI18n.i18nc("@info:tooltip", "Paused");
        default:
            return "";
        }
    }

    signal closeRequested
    signal contextRequested
    signal renameRequested
    signal dragRequested(point hotSpot, point scenePosition)
    signal dragMoved(point scenePosition)
    signal dragReleased(bool accepted)
    signal navigationRequested(int key)

    objectName: "terminalTab" + index

    // Avoid painting the title twice with qqc2-desktop-style.
    // HACK: Union's QWidget style skips tab frames with empty native text.
    // Keep one space until it paints frames without native text or icons.
    text: " "
    leftPadding: 0
    rightPadding: 0
    topPadding: Kirigami.Units.smallSpacing
    bottomPadding: Kirigami.Units.smallSpacing
    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset, implicitContentWidth + leftPadding + rightPadding)
    // Reserve progress space even when hidden to keep the tab height stable.
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset, implicitContentHeight + topPadding + bottomPadding, titleLabel.implicitHeight + 2 * (progress.height + progress.anchors.bottomMargin) + Kirigami.Units.smallSpacing)
    Accessible.role: Accessible.PageTab
    Accessible.name: session && session.attention ? KI18n.i18nc("@info:accessibility", "%1, needs attention", sessionTitle) : sessionTitle
    Accessible.description: progressDescription
    Accessible.selected: checked

    // The desktop style delegates to QMacStyle on macOS, whose segmented
    // buttons do not join the terminal frame. Keep the Linux desktop tab
    // silhouette instead, using the same live Kirigami palette.
    Component.onCompleted: {
        if (Qt.platform.os === "osx")
            background = desktopTabBackground.createObject(control);
    }

    Component {
        id: desktopTabBackground

        Rectangle {
            implicitWidth: Kirigami.Units.gridUnit * 8
            implicitHeight: Kirigami.Units.gridUnit * 2
            Kirigami.Theme.colorSet: Kirigami.Theme.Window
            Kirigami.Theme.inherit: false

            readonly property bool footer: control.tabPosition === Controls.TabBar.Footer
            readonly property real cornerRadius: Kirigami.Units.smallSpacing

            topLeftRadius: footer ? 0 : cornerRadius
            topRightRadius: footer ? 0 : cornerRadius
            bottomLeftRadius: footer ? cornerRadius : 0
            bottomRightRadius: footer ? cornerRadius : 0
            color: control.checked ? Kirigami.Theme.backgroundColor : Qt.tint(Kirigami.Theme.backgroundColor, Qt.alpha(control.hovered ? Kirigami.Theme.hoverColor : Kirigami.Theme.textColor, control.hovered ? 0.12 : 0.06))
            border.width: control.checked ? 1 : 0
            border.color: control.visualFocus ? Kirigami.Theme.focusColor : Qt.alpha(Kirigami.Theme.textColor, 0.25)

            // Open the selected tab toward the terminal, just like the
            // toolbar separator's selected-tab gap, including bottom tabs.
            Rectangle {
                visible: control.checked
                x: 1
                y: parent.footer ? 0 : parent.height - height
                width: parent.width - 2
                height: 1
                color: parent.color
            }
        }
    }

    Keys.onPressed: event => {
        if (event.key === Qt.Key_Left || event.key === Qt.Key_Right || event.key === Qt.Key_Home || event.key === Qt.Key_End) {
            control.navigationRequested(event.key);
            event.accepted = true;
        }
    }

    contentItem: RowLayout {
        spacing: Kirigami.Units.smallSpacing

        Kirigami.Icon {
            objectName: "terminalTabIcon" + control.index
            visible: control.showStatusIcons
            Layout.leftMargin: Kirigami.Units.smallSpacing * 2
            Layout.preferredWidth: Kirigami.Units.iconSizes.small
            Layout.preferredHeight: Kirigami.Units.iconSizes.small
            source: control.session ? control.session.iconName : "utilities-terminal"
        }

        Controls.Label {
            id: titleLabel

            objectName: "terminalTabLabel" + control.index
            Layout.fillWidth: true
            Layout.leftMargin: control.showStatusIcons ? 0 : Kirigami.Units.smallSpacing * 2
            text: control.sessionTitle
            font: control.font
            textFormat: Text.PlainText
            elide: Text.ElideRight
        }

        TabCloseButton {
            id: closeButton

            objectName: "closeTerminalTab" + control.index
            Kirigami.MnemonicData.enabled: false

            readonly property string closeLabel: KI18n.i18nc("@action:button", "Close %1", control.sessionTitle)

            Layout.alignment: Qt.AlignVCenter
            Layout.rightMargin: Kirigami.Units.smallSpacing * 2
            selected: control.checked
            // Match QTabBar by leaving keyboard access to the Close Tab action.
            focusPolicy: Qt.NoFocus

            // A terminal title's ampersands must not create Qt button shortcuts.
            text: closeLabel.replace(/&/g, "&&")
            Accessible.name: closeLabel
            onClicked: control.closeRequested()

            Controls.ToolTip.text: TextFormatting.plainTextToHtml(closeLabel)
            Controls.ToolTip.visible: hovered
            Controls.ToolTip.delay: Kirigami.Units.toolTipDelay
        }
    }

    T.ProgressBar {
        id: progress

        objectName: "terminalTabProgress" + control.index
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.leftMargin: Kirigami.Units.smallSpacing * 2
        anchors.rightMargin: Kirigami.Units.smallSpacing * 2
        anchors.bottomMargin: Kirigami.Units.smallSpacing
        height: Kirigami.Units.smallSpacing
        visible: control.hasProgress
        from: 0
        to: 100
        value: control.session ? Math.max(0, control.session.progressPercent) : 0
        indeterminate: control.hasProgress && (control.session.progressState === Terminal.Session.Indeterminate || (control.session.progressPercent < 0 && control.session.progressState !== Terminal.Session.Paused))
        Accessible.name: control.sessionTitle
        Accessible.description: control.progressDescription
        palette.highlight: control.session && control.session.progressState === Terminal.Session.Error ? control.Kirigami.Theme.negativeTextColor : control.session && control.session.progressState === Terminal.Session.Paused ? control.Kirigami.Theme.neutralTextColor : control.Kirigami.Theme.highlightColor

        background: Rectangle {
            color: progress.palette.highlight
            opacity: 0.3
        }

        contentItem: Item {
            id: progressContent

            clip: progress.indeterminate
            scale: progress.mirrored ? -1 : 1

            Rectangle {
                property real phase: 0

                x: progress.indeterminate ? (progressContent.width + width) * phase - width : 0
                width: progressContent.width * (progress.indeterminate ? 0.25 : progress.position)
                height: progressContent.height
                color: progress.palette.highlight

                NumberAnimation on phase {
                    from: 0
                    to: 1
                    duration: 1200
                    loops: Animation.Infinite
                    running: progress.visible && progress.indeterminate
                }
            }
        }
    }

    DragHandler {
        id: tabDrag

        target: null
        enabled: control.draggable && control.session !== null
        acceptedButtons: Qt.LeftButton
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad

        onActiveChanged: {
            if (active && !closeButton.contains(closeButton.mapFromItem(control, centroid.pressPosition)))
                control.dragRequested(centroid.pressPosition, centroid.scenePosition);
        }

        onCentroidChanged: {
            if (active)
                control.dragMoved(centroid.scenePosition);
        }

        onGrabChanged: transition => {
            if (transition === PointerDevice.UngrabExclusive)
                control.dragReleased(true);
        }

        onCanceled: control.dragReleased(false)
    }

    TapHandler {
        acceptedButtons: Qt.MiddleButton

        onTapped: control.closeRequested()
    }

    TapHandler {
        acceptedButtons: Qt.RightButton

        onTapped: control.contextRequested()
    }

    TapHandler {
        onDoubleTapped: control.renameRequested()
    }

    opacity: dragging ? 0.6 : 1

    Controls.ToolTip {
        objectName: "terminalTabToolTip" + control.index
        text: TextFormatting.plainTextToHtml(control.sessionTitle + (control.progressDescription.length > 0 ? "\n" + control.progressDescription : "") + (control.monitoringDescription.length > 0 ? "\n" + control.monitoringDescription : ""))
        visible: control.hovered && !closeButton.hovered && (titleLabel.truncated || control.progressDescription.length > 0 || control.monitoringDescription.length > 0)
        delay: Kirigami.Units.toolTipDelay
    }
}
