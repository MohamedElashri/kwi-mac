// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.ki18n
import org.kde.kirigami as Kirigami
import org.kde.kirigami.forms as Forms
import org.kde.kwi

SettingsPage {
    id: root

    required property AppSettings appSettings

    objectName: "terminalSettings"
    title: KI18n.i18nc("@title:window", "Terminal")
    defaultsVisible: true
    defaultsEnabled: root.appSettings.canResetTerminal || titleFormatField.text !== root.appSettings.titleFormat || remoteTitleFormatField.text !== root.appSettings.remoteTitleFormat

    onRestoreDefaultsRequested: {
        root.appSettings.resetTerminalDefaults();
        root.appSettings.save();
        titleFormatField.text = Qt.binding(() => root.appSettings.titleFormat);
        remoteTitleFormatField.text = Qt.binding(() => root.appSettings.remoteTitleFormat);
    }

    property int limitedScrollbackLines: root.appSettings.scrollbackLines > 0 ? root.appSettings.scrollbackLines : 10000

    function insertTitlePart(part: string): void {
        const field = titlePartMenu.remote ? remoteTitleFormatField : titleFormatField;
        const start = field.selectionStart;
        const end = field.selectionEnd;

        if (field.length - (end - start) + part.length > field.maximumLength)
            return;

        field.remove(start, end);
        field.insert(start, part);
        field.cursorPosition = start + part.length;

        if (root.appSettings.titleFormatError(field.text).length === 0) {
            if (titlePartMenu.remote)
                root.appSettings.remoteTitleFormat = field.text;
            else
                root.appSettings.titleFormat = field.text;

            root.appSettings.save();
        }

        field.forceActiveFocus(Qt.OtherFocusReason);
    }

    component TitlePartMenuItem: Controls.MenuItem {
        required property string part

        enabled: {
            const field = titlePartMenu.remote ? remoteTitleFormatField : titleFormatField;
            return field.enabled && field.length - (field.selectionEnd - field.selectionStart) + part.length <= field.maximumLength;
        }

        onTriggered: root.insertTitlePart(part)
    }

    Controls.Menu {
        id: titlePartMenu

        property bool remote: false

        objectName: "titleFormatMenu"

        TitlePartMenuItem {
            objectName: "titleFormatInsertTitle"
            text: KI18n.i18nc("@action:inmenu", "Application title")
            part: "{title}"
        }

        TitlePartMenuItem {
            objectName: "titleFormatInsertCommand"
            text: KI18n.i18nc("@action:inmenu", "Command")
            part: "{command}"
        }

        TitlePartMenuItem {
            objectName: "titleFormatInsertDirectory"
            text: KI18n.i18nc("@action:inmenu", "Directory name")
            part: "{directory}"
        }

        TitlePartMenuItem {
            objectName: "titleFormatInsertPath"
            text: KI18n.i18nc("@action:inmenu", "Directory path")
            part: "{path}"
        }

        Controls.MenuSeparator {}

        TitlePartMenuItem {
            objectName: "titleFormatInsertUser"
            text: KI18n.i18nc("@action:inmenu", "User name")
            part: "{user}"
        }

        TitlePartMenuItem {
            objectName: "titleFormatInsertHost"
            text: KI18n.i18nc("@action:inmenu", "Host name")
            part: "{host}"
        }

        TitlePartMenuItem {
            objectName: "titleFormatInsertShortHost"
            text: KI18n.i18nc("@action:inmenu", "Host name (short)")
            part: "{shortHost}"
        }

        Controls.MenuSeparator {}

        TitlePartMenuItem {
            objectName: "titleFormatInsertOpeningBrace"
            text: KI18n.i18nc("@action:inmenu", "Opening brace: {")
            part: "{{"
        }

        TitlePartMenuItem {
            objectName: "titleFormatInsertClosingBrace"
            text: KI18n.i18nc("@action:inmenu", "Closing brace: }")
            part: "}}"
        }
    }

    Forms.Form {
        Forms.FormGroup {
            title: KI18n.i18nc("@title:group", "Session Titles")

            Forms.FormEntry {
                title: KI18n.i18nc("@label:textbox", "Tab title format:")
                enabled: !root.appSettings.isTitleFormatImmutable
                subtitle: KI18n.i18nc("@info", "Renamed tabs keep their names.")

                contentItem: RowLayout {
                    Layout.fillWidth: true
                    Kirigami.FormData.buddyFor: titleFormatField
                    spacing: Kirigami.Units.smallSpacing

                    Controls.TextField {
                        id: titleFormatField

                        objectName: "titleFormatField"
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        text: root.appSettings.titleFormat
                        maximumLength: 256
                        selectByMouse: true
                        persistentSelection: true

                        onTextEdited: {
                            if (root.appSettings.titleFormatError(text).length === 0) {
                                root.appSettings.titleFormat = text;
                                root.appSettings.save();
                            }
                        }
                    }

                    Controls.Button {
                        id: insertButton

                        objectName: "titleFormatInsert"
                        text: KI18n.i18nc("@action:button", "Insert")
                        icon.name: "list-add"

                        onClicked: {
                            titlePartMenu.remote = false;
                            titlePartMenu.popup(insertButton, 0, insertButton.height);
                        }
                    }
                }
            }

            Forms.FormEntry {
                visible: titleError.text.length > 0
                fullWidth: true

                contentItem: Kirigami.InlineMessage {
                    id: titleError

                    objectName: "titleFormatErrorMessage"
                    visible: true
                    text: TextFormatting.plainTextToHtml(root.appSettings.titleFormatError(titleFormatField.text))
                    type: Kirigami.MessageType.Error
                }
            }

            Forms.FormEntry {
                title: KI18n.i18nc("@label", "Preview:")

                contentItem: Controls.Label {
                    objectName: "titleFormatPreview"
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    text: root.appSettings.previewTitleFormat(titleFormatField.text)
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                }
            }

            Forms.FormEntry {
                title: KI18n.i18nc("@label:textbox", "Remote tab title format:")
                enabled: !root.appSettings.isRemoteTitleFormatImmutable

                contentItem: RowLayout {
                    Layout.fillWidth: true
                    Kirigami.FormData.buddyFor: remoteTitleFormatField
                    spacing: Kirigami.Units.smallSpacing

                    Controls.TextField {
                        id: remoteTitleFormatField

                        objectName: "remoteTitleFormatField"
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        text: root.appSettings.remoteTitleFormat
                        maximumLength: 256
                        selectByMouse: true
                        persistentSelection: true

                        onTextEdited: {
                            if (root.appSettings.titleFormatError(text).length === 0) {
                                root.appSettings.remoteTitleFormat = text;
                                root.appSettings.save();
                            }
                        }
                    }

                    Controls.Button {
                        id: remoteInsertButton

                        objectName: "remoteTitleFormatInsert"
                        text: KI18n.i18nc("@action:button", "Insert")
                        icon.name: "list-add"

                        onClicked: {
                            titlePartMenu.remote = true;
                            titlePartMenu.popup(remoteInsertButton, 0, remoteInsertButton.height);
                        }
                    }
                }
            }

            Forms.FormEntry {
                visible: remoteTitleError.text.length > 0
                fullWidth: true

                contentItem: Kirigami.InlineMessage {
                    id: remoteTitleError

                    objectName: "remoteTitleFormatErrorMessage"
                    visible: true
                    text: TextFormatting.plainTextToHtml(root.appSettings.titleFormatError(remoteTitleFormatField.text))
                    type: Kirigami.MessageType.Error
                }
            }

            Forms.FormEntry {
                title: KI18n.i18nc("@label", "Preview:")

                contentItem: Controls.Label {
                    objectName: "remoteTitleFormatPreview"
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    text: root.appSettings.previewTitleFormat(remoteTitleFormatField.text, true)
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                }
            }

            Forms.FormEntry {
                contentItem: Controls.CheckBox {
                    objectName: "windowTitleFromApplication"
                    text: KI18n.i18nc("@option:check", "Use application title for the window")
                    enabled: !root.appSettings.isWindowTitleFromApplicationImmutable
                    checked: root.appSettings.windowTitleFromApplication

                    onToggled: {
                        root.appSettings.windowTitleFromApplication = checked;
                        root.appSettings.save();
                    }
                }
            }
        }

        Forms.FormGroup {
            title: KI18n.i18nc("@title:group", "Text direction")

            Forms.FormEntry {
                contentItem: Controls.CheckBox {
                    objectName: "bidirectionalText"
                    text: KI18n.i18nc("@option:check", "Display Arabic, Persian and Hebrew in reading order")
                    enabled: !root.appSettings.isBidirectionalTextImmutable
                    checked: root.appSettings.bidirectionalText
                    onToggled: {
                        root.appSettings.bidirectionalText = checked;
                        root.appSettings.save();
                    }
                }
            }
            Forms.FormEntry {
                contentItem: Controls.Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: KI18n.i18nc("@info", "Turn this off for applications that already arrange right-to-left text themselves.")
                }
            }
        }

        Forms.FormGroup {
            title: KI18n.i18nc("@title:group", "Scrollback")

            Forms.FormEntry {
                title: KI18n.i18nc("@label:listbox", "History:")
                enabled: !root.appSettings.isScrollbackLinesImmutable

                contentItem: RowLayout {
                    Kirigami.FormData.buddyFor: historyMode
                    spacing: Kirigami.Units.smallSpacing

                    Controls.ComboBox {
                        id: historyMode

                        objectName: "scrollbackMode"
                        model: [KI18n.i18nc("@item:inlistbox", "None"), KI18n.i18nc("@item:inlistbox", "Limited"), KI18n.i18nc("@item:inlistbox", "No line limit")]
                        currentIndex: root.appSettings.scrollbackLines === -1 ? 2 : (root.appSettings.scrollbackLines === 0 ? 0 : 1)

                        onActivated: index => {
                            if (root.appSettings.scrollbackLines > 0)
                                root.limitedScrollbackLines = root.appSettings.scrollbackLines;

                            root.appSettings.scrollbackLines = index === 2 ? -1 : (index === 0 ? 0 : root.limitedScrollbackLines);
                            root.appSettings.save();
                        }
                    }

                    Controls.SpinBox {
                        id: scrollbackLines

                        objectName: "scrollbackLines"
                        Accessible.name: KI18n.i18nc("@label:spinbox", "Retained lines")
                        enabled: historyMode.currentIndex === 1
                        editable: true
                        from: 1
                        to: 100000
                        stepSize: 1000
                        value: root.appSettings.scrollbackLines > 0 ? root.appSettings.scrollbackLines : root.limitedScrollbackLines

                        onValueModified: {
                            root.appSettings.scrollbackLines = value;
                            root.appSettings.save();
                        }
                    }

                    Controls.Label {
                        text: KI18n.i18ncp("@label unit after the retained-line count", "line", "lines", scrollbackLines.value)
                        enabled: historyMode.currentIndex === 1
                    }
                }
            }

            Controls.Label {
                objectName: "historyRetentionNotice"
                visible: root.appSettings.scrollbackLines === -1
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: KI18n.i18nc("@info", "History is memory limited. Older output may be discarded when the limit is reached, including during continuous output.")
            }
        }

        Forms.FormGroup {
            title: KI18n.i18nc("@title:group", "Monitoring")

            Forms.FormEntry {
                title: KI18n.i18nc("@label:spinbox", "Silence timeout (seconds):")

                contentItem: Controls.SpinBox {
                    objectName: "silenceInterval"
                    enabled: !root.appSettings.isSilenceSecondsImmutable
                    editable: true
                    from: 1
                    to: 3600
                    value: root.appSettings.silenceSeconds

                    onValueModified: {
                        root.appSettings.silenceSeconds = value;
                        root.appSettings.save();
                    }
                }
            }
        }

        Forms.FormGroup {
            title: KI18n.i18nc("@title:group", "Cursor")

            Forms.FormEntry {
                title: KI18n.i18nc("@label:listbox", "Shape:")

                contentItem: Controls.ComboBox {
                    objectName: "cursorStyleSetting"
                    enabled: !root.appSettings.isCursorStyleImmutable
                    textRole: "text"
                    valueRole: "value"
                    model: [
                        {
                            text: KI18n.i18nc("@item:inlistbox", "Block"),
                            value: "block"
                        },
                        {
                            text: KI18n.i18nc("@item:inlistbox", "Beam"),
                            value: "beam"
                        },
                        {
                            text: KI18n.i18nc("@item:inlistbox", "Underline"),
                            value: "underline"
                        }
                    ]
                    currentIndex: ["block", "beam", "underline"].indexOf(root.appSettings.cursorStyle)

                    onActivated: index => {
                        root.appSettings.cursorStyle = model[index].value;
                        root.appSettings.save();
                    }
                }
            }

            Forms.FormEntry {
                contentItem: Controls.CheckBox {
                    objectName: "cursorBlinkingSetting"
                    text: KI18n.i18nc("@option:check", "Blink cursor")
                    enabled: !root.appSettings.isCursorBlinkingImmutable
                    checked: root.appSettings.cursorBlinking

                    onToggled: {
                        root.appSettings.cursorBlinking = checked;
                        root.appSettings.save();
                    }
                }
            }
        }
    }
}
