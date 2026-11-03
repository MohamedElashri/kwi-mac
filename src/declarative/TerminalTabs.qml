// SPDX-FileCopyrightText: 2026 Kwi contributors
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.ki18n
import org.kde.kirigami as Kirigami
import org.kde.kwi

RowLayout {
    id: root

    required property SessionModel sessions
    property WindowManager dragController: null
    property int position: Controls.TabBar.Header
    property bool showStatusIcons: true
    readonly property int count: tabBar.count
    readonly property int currentIndex: sessions.currentIndex
    readonly property ListView view: tabBar.contentItem as ListView
    readonly property bool overflowing: count * Kirigami.Units.gridUnit * 8 > width

    // Clip the selected tab's frame opening to the scrolled viewport.
    readonly property real viewportLeft: tabBar.x + view.x
    readonly property real selectedTabLeft: viewportLeft + Math.max(0, Math.min(view.width, tabBar.currentItem ? tabBar.currentItem.x - view.contentX : 0))
    readonly property real selectedTabRight: viewportLeft + Math.max(0, Math.min(view.width, tabBar.currentItem ? tabBar.currentItem.x + tabBar.currentItem.width - view.contentX : 0))

    // originX remains the content's left edge in right-to-left layouts.
    readonly property real occupiedLeft: view.originX - view.contentX
    readonly property real occupiedRight: occupiedLeft + view.contentWidth

    signal activated(int index)
    signal contextRequested(TerminalSession session)
    signal renameRequested(int index)
    signal newTabRequested

    objectName: "terminalTabs"
    spacing: 0

    // The drag must survive removal of its source tab delegate.
    Item {
        id: dragHelper

        visible: false

        property string token: ""
        property var preview: null

        Drag.source: dragHelper
        Drag.dragType: Drag.Automatic
        Drag.supportedActions: Qt.MoveAction
        Drag.proposedAction: Qt.MoveAction
        Drag.mimeData: ({ "application/x-kwi-terminal-tab": token })

        Drag.onDragFinished: action => {
            root.dragController.finishTabDrag(token, action);
            token = "";
            dragHelper.Drag.imageSource = "";
            preview = null;
        }

        function start(tab: Item, hotSpot: point): void {
            tab.grabToImage(result => {
                if (!tab || !tab.dragGestureActive || !root.dragController)
                    return;

                const token = root.dragController.beginTabDrag(root.sessions, tab.session);

                if (token.length === 0)
                    return;

                dragHelper.token = token;
                dragHelper.preview = result;
                dragHelper.Drag.imageSource = result.url;
                dragHelper.Drag.hotSpot = hotSpot;
                dragHelper.Drag.active = true;
            });
        }
    }

    function scrollTabs(distance: real): void {
        view.contentX = Math.max(view.originX, Math.min(view.originX + Math.max(0, view.contentWidth - view.width), view.contentX + distance));
    }

    Controls.ToolButton {
        objectName: "previousTabsButton"
        Kirigami.MnemonicData.enabled: false
        visible: root.overflowing
        enabled: !root.view.atXBeginning
        icon.name: "arrow-left"
        text: KI18n.i18nc("@action:button", "Scroll Tabs Left")
        display: Controls.AbstractButton.IconOnly

        onClicked: root.scrollTabs(-root.view.width / 2)

        Controls.ToolTip.text: text
        Controls.ToolTip.visible: hovered
        Controls.ToolTip.delay: Kirigami.Units.toolTipDelay
    }

    Controls.TabBar {
        id: tabBar

        objectName: "terminalTabBar"
        // The enclosing toolbar owns the surface behind tabs and buttons.
        background: null
        Layout.fillWidth: true
        Layout.fillHeight: true
        clip: true
        position: root.position
        currentIndex: root.sessions.currentIndex
        contentItem.objectName: "terminalTabList"

        Binding {
            root.view.snapMode: ListView.NoSnap
        }

        Binding {
            when: Qt.platform.os === "osx"
            root.view.spacing: 0
        }

        // Guard the desktop style's height calculation while the Repeater is empty.
        contentItem.implicitHeight: tabBar.count > 0 && tabBar.itemAt(0) ? tabBar.itemAt(0).implicitHeight : 0

        // Fill the toolbar height without making implicitHeight depend on actual height.
        implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset, contentItem.implicitHeight + topPadding + bottomPadding)
        contentHeight: availableHeight

        readonly property real tabWidth: Math.max(Kirigami.Units.gridUnit * 8, Math.min(Kirigami.Units.gridUnit * 14, width / Math.max(1, count)))

        function revealCurrent(): void {
            setCurrentIndex(root.sessions.currentIndex);

            if (currentIndex >= 0)
                contentItem.positionViewAtIndex(currentIndex, ListView.Contain);
        }

        function navigateTo(index: int): void {
            root.activated(index);
            const button = itemAt(index);

            if (button)
                button.forceActiveFocus(Qt.TabFocusReason);
        }

        // Ignore intermediate container indexes during model changes.
        onCurrentIndexChanged: Qt.callLater(revealCurrent)
        onCountChanged: Qt.callLater(revealCurrent)
        onWidthChanged: Qt.callLater(revealCurrent)

        Connections {
            target: root.sessions

            function onCurrentIndexChanged(): void {
                tabBar.revealCurrent();
            }
        }

        function navigateKey(key: int): bool {
            switch (key) {
            case Qt.Key_Left:
                navigateTo(Math.max(0, root.currentIndex - 1));
                break;
            case Qt.Key_Right:
                navigateTo(Math.min(root.count - 1, root.currentIndex + 1));
                break;
            case Qt.Key_Home:
                navigateTo(0);
                break;
            case Qt.Key_End:
                navigateTo(root.count - 1);
                break;
            default:
                return false;
            }

            return true;
        }

        Keys.onPressed: event => event.accepted = navigateKey(event.key)

        Repeater {
            model: root.sessions

            delegate: TerminalTabButton {
                id: tabButton

                width: tabBar.tabWidth
                height: tabBar.availableHeight
                tabPosition: root.position
                showStatusIcons: root.showStatusIcons
                draggable: root.dragController !== null
                dragging: root.dragController !== null && root.dragController.draggedSession === session

                onClicked: root.activated(index)
                onCloseRequested: root.sessions.closeSession(index)
                onContextRequested: root.contextRequested(session)
                onRenameRequested: root.renameRequested(index)
                onDragRequested: hotSpot => dragHelper.start(tabButton, hotSpot)
                onNavigationRequested: key => tabBar.navigateKey(key)
            }
        }

        WheelHandler {
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad

            onWheel: event => {
                const delta = event.pixelDelta.x || event.pixelDelta.y || event.angleDelta.x / 2 || event.angleDelta.y / 2;
                root.scrollTabs(-delta);
            }
        }

        MouseArea {
            parent: root.view
            width: Math.max(0, Math.min(root.view.width, root.occupiedLeft))
            height: root.view.height
            acceptedButtons: Qt.LeftButton

            onDoubleClicked: root.newTabRequested()
        }

        DropArea {
            id: tabDrop

            objectName: "terminalTabDropArea"

            // An explicit parent keeps this overlay out of the TabBar's content model.
            parent: root.view
            anchors.fill: parent
            z: 2
            keys: ["application/x-kwi-terminal-tab"]

            readonly property int insertionIndex: insertionAt(drag.x)

            function insertionAt(position: real): int {
                for (let index = 0; index < root.count; ++index) {
                    const tab = tabBar.itemAt(index);
                    const midpoint = tab.x + tab.width / 2 - root.view.contentX;

                    if (tabBar.mirrored ? position > midpoint : position < midpoint)
                        return index;
                }

                return root.count;
            }

            readonly property real insertionX: {
                const tab = tabBar.itemAt(Math.min(insertionIndex, root.count - 1));

                if (!tab)
                    return 0;

                const trailing = insertionIndex === root.count;

                return tab.x - root.view.contentX + ((trailing !== tabBar.mirrored) ? tab.width : 0);
            }

            readonly property int scrollDirection: !containsDrag || !root.overflowing ? 0 : drag.x < Kirigami.Units.gridUnit ? -1 : drag.x > width - Kirigami.Units.gridUnit ? 1 : 0

            onEntered: event => {
                event.accepted = root.dragController !== null && root.dragController.acceptsTabDrag(event.getDataAsString("application/x-kwi-terminal-tab"));
            }

            onDropped: event => {
                event.accepted = false;

                if (root.dragController !== null && root.dragController.dropTab(event.getDataAsString("application/x-kwi-terminal-tab"), root.sessions, insertionAt(event.x)))
                    event.accept(Qt.MoveAction);
            }

            Rectangle {
                objectName: "terminalTabDropIndicator"
                visible: tabDrop.containsDrag
                x: Math.max(0, Math.min(tabDrop.width - width, tabDrop.insertionX - width / 2))
                y: Kirigami.Units.smallSpacing
                width: Kirigami.Units.smallSpacing / 2
                height: Math.max(0, parent.height - 2 * y)
                color: Kirigami.Theme.highlightColor
            }

            Timer {
                interval: 60
                repeat: true
                running: tabDrop.scrollDirection !== 0

                onTriggered: root.scrollTabs(tabDrop.scrollDirection * Kirigami.Units.gridUnit)
            }
        }

        MouseArea {
            parent: root.view
            x: Math.max(0, Math.min(root.view.width, root.occupiedRight))
            width: root.view.width - x
            height: root.view.height
            acceptedButtons: Qt.LeftButton

            onDoubleClicked: root.newTabRequested()
        }
    }

    Controls.ToolButton {
        objectName: "nextTabsButton"
        Kirigami.MnemonicData.enabled: false
        visible: root.overflowing
        enabled: !root.view.atXEnd
        icon.name: "arrow-right"
        text: KI18n.i18nc("@action:button", "Scroll Tabs Right")
        display: Controls.AbstractButton.IconOnly

        onClicked: root.scrollTabs(root.view.width / 2)

        Controls.ToolTip.text: text
        Controls.ToolTip.visible: hovered
        Controls.ToolTip.delay: Kirigami.Units.toolTipDelay
    }
}
