// SPDX-FileCopyrightText: 2026 Kwi contributors
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.ki18n
import org.kde.kirigami as Kirigami
import org.kde.kirigamiaddons.formcard as FormCard

FormCard.AboutPage {
    id: root

    // The upstream library delegates eagerly lay out every license, including
    // hidden dialogs. Keep one shared viewer and populate it only on request.
    showLibraries: false

    FormCard.FormHeader {
        title: KI18n.i18nd("kirigami-addons6", "Libraries in use")
        actions: Kirigami.Action {
            text: KI18n.i18ndc("kirigami-addons6", "@action:button", "Copy to Clipboard")
            icon.name: "edit-copy-symbolic"
            onTriggered: {
                FormCard.AboutComponent.copyToClipboard();
                root.Controls.ApplicationWindow.window.showPassiveNotification(KI18n.i18ndc("kirigami-addons6", "@info", "System information copied to clipboard."), "short");
            }
        }
    }

    FormCard.FormCard {
        Repeater {
            model: FormCard.AboutComponent.components

            FormCard.AbstractFormDelegate {
                id: library
                required property var modelData
                Layout.fillWidth: true
                background: null
                contentItem: RowLayout {
                    ColumnLayout {
                        Layout.fillWidth: true
                        Controls.Label {
                            Layout.fillWidth: true
                            text: library.modelData.name + " " + library.modelData.version
                            wrapMode: Text.WordWrap
                        }
                        Controls.Label {
                            Layout.fillWidth: true
                            text: library.modelData.description
                            color: Kirigami.Theme.disabledTextColor
                            font: Kirigami.Theme.smallFont
                            wrapMode: Text.WordWrap
                            visible: text.length > 0
                        }
                    }
                    Controls.ToolButton {
                        objectName: "libraryLicenseButton_" + library.modelData.name
                        visible: library.modelData.licenses.key !== 0
                        text: library.modelData.licenses.name
                        display: Controls.AbstractButton.IconOnly
                        icon.name: "license-symbolic"
                        Controls.ToolTip.text: text
                        Controls.ToolTip.visible: hovered
                        onClicked: {
                            libraryLicense.title = library.modelData.licenses.name;
                            libraryLicense.licenseText = library.modelData.licenses.text;
                            libraryLicense.open();
                        }
                    }
                    Controls.ToolButton {
                        visible: (library.modelData.webAddress ?? "").length > 0
                        text: library.modelData.webAddress ?? ""
                        display: Controls.AbstractButton.IconOnly
                        icon.name: "globe-symbolic"
                        Controls.ToolTip.text: text
                        Controls.ToolTip.visible: hovered
                        onClicked: Qt.openUrlExternally(library.modelData.webAddress)
                    }
                }
            }
        }
    }

    Controls.Dialog {
        id: libraryLicense

        objectName: "libraryLicenseDialog"
        property string licenseText: ""
        parent: root.Controls.Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(Kirigami.Units.gridUnit * 30, parent.width - Kirigami.Units.largeSpacing * 2)
        height: Math.min(Kirigami.Units.gridUnit * 24, parent.height - Kirigami.Units.largeSpacing * 2)
        modal: true
        standardButtons: Controls.Dialog.Close
        onClosed: licenseText = ""

        contentItem: Controls.ScrollView {
            id: licenseScroll
            contentWidth: availableWidth
            Controls.TextArea {
                objectName: "libraryLicenseText"
                width: licenseScroll.availableWidth
                text: libraryLicense.licenseText
                textFormat: TextEdit.PlainText
                readOnly: true
                selectByMouse: true
                wrapMode: TextEdit.Wrap
            }
        }
    }
}
