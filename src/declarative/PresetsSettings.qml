// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Dialogs as Dialogs
import QtQuick.Layouts
import org.kde.ki18n
import org.kde.kirigami as Kirigami
import org.kde.kirigami.forms as Forms
import org.kde.kwi

SettingsPage {
    id: root

    required property AppSettings appSettings
    required property PresetCatalog terminalPresets
    required property PresetEditor editor

    objectName: "presetsSettings"
    title: KI18n.i18nc("@title:window", "Presets")

    function performAction(action: string, name: string): void {
        switch (action) {
        case "select":
            root.editor.load(name);
            break;
        case "duplicate":
            root.editor.newCopy();
            openEditor();
            break;
        case "import":
            importDialog.open();
            break;
        case "remove":
            removeDialog.open();
            break;
        }
    }

    function requestAction(action: string, name: string): void {
        if (root.editor.dirty) {
            unsavedDialog.pendingAction = action;
            unsavedDialog.pendingName = name;
            unsavedDialog.open();
        } else {
            performAction(action, name);
        }
    }

    function select(name: string): void {
        if (name !== root.editor.selectedName)
            requestAction("select", name);
    }

    function openEditor(): void {
        presetName.forceActiveFocus(Qt.OtherFocusReason);
    }

    function newCopy(): void {
        requestAction("duplicate", "");
    }

    function editVariable(index: int): void {
        environmentDialog.entryIndex = index;
        environmentDialog.acceptedIndex = -1;
        const entry = index < 0 ? {name: "", value: ""} : root.editor.environmentEntries[index];
        variableName.text = entry.name;
        variableValue.text = entry.value;
        environmentDialog.open();
    }

    function focusVariable(index: int): void {
        const entry = environmentRows.itemAt(index) as EnvironmentRow;

        if (entry)
            entry.focusEditor();
        else
            addVariableButton.forceActiveFocus(Qt.OtherFocusReason);
    }

    header: Kirigami.InlineMessage {
        visible: text.length > 0
        text: TextFormatting.plainTextToHtml(root.editor.errorString || root.terminalPresets.errorString)
        type: Kirigami.MessageType.Error
    }

    footer: Controls.ToolBar {
        padding: 0

        contentItem: Controls.DialogButtonBox {
            id: presetButtons

            objectName: "presetEditorButtons"
            padding: Kirigami.Units.largeSpacing

            Controls.Button {
                objectName: "savePresetButton"
                text: KI18n.i18nc("@action:button", "Save")
                icon.name: "document-save"
                enabled: root.editor.dirty && root.editor.name.trim().length > 0
                Controls.DialogButtonBox.buttonRole: Controls.DialogButtonBox.AcceptRole

                onClicked: root.editor.save()
            }

            Controls.Button {
                objectName: "resetPresetButton"
                text: KI18n.i18nc("@action:button", "Reset")
                icon.name: "edit-undo"
                enabled: root.editor.dirty
                Controls.DialogButtonBox.buttonRole: Controls.DialogButtonBox.ResetRole

                onClicked: root.editor.discard()
            }
        }
    }

    Forms.Form {
        Forms.FormGroup {
            title: KI18n.i18nc("@title:group", "New Sessions")

            Forms.FormEntry {
                title: KI18n.i18nc("@label:listbox", "Default preset:")

                contentItem: Controls.ComboBox {
                    objectName: "defaultPresetSelector"
                    enabled: !root.appSettings.isDefaultPresetImmutable
                    Layout.fillWidth: true
                    textRole: "label"
                    valueRole: "name"
                    model: root.terminalPresets.entries
                    currentIndex: root.terminalPresets.names.indexOf(root.terminalPresets.defaultPresetName)

                    onActivated: index => root.terminalPresets.setDefaultPreset(model[index].name)
                }
            }
        }

        Forms.FormGroup {
            title: KI18n.i18nc("@title:group", "Manage Presets")

            Forms.FormEntry {
                title: KI18n.i18nc("@label:listbox", "Preset:")

                contentItem: RowLayout {
                    Kirigami.FormData.buddyFor: presetSelector
                    spacing: Kirigami.Units.smallSpacing

                    Controls.ComboBox {
                        id: presetSelector

                        objectName: "presetSelector"
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        textRole: "label"
                        valueRole: "name"
                        model: root.terminalPresets.entries
                        currentIndex: root.terminalPresets.names.indexOf(root.editor.selectedName)

                        onActivated: index => {
                            root.select(model[index].name);
                            currentIndex = Qt.binding(() => root.terminalPresets.names.indexOf(root.editor.selectedName));
                        }
                    }

                    Controls.Button {
                        objectName: "editPresetButton"
                        text: KI18n.i18nc("@action:button", "Edit Preset")
                        icon.name: "document-edit"
                        display: Controls.AbstractButton.IconOnly
                        Controls.ToolTip.text: text
                        Controls.ToolTip.visible: hovered
                        Controls.ToolTip.delay: Kirigami.Units.toolTipDelay

                        onClicked: root.openEditor()
                    }

                    Controls.ToolButton {
                        id: presetMenuButton

                        objectName: "presetActionsButton"
                        text: KI18n.i18nc("@action:button", "More Preset Actions")
                        icon.name: "overflow-menu"
                        display: Controls.AbstractButton.IconOnly
                        down: pressed || presetMenu.visible
                        Controls.ToolTip.text: text
                        Controls.ToolTip.visible: hovered && !presetMenu.visible
                        Controls.ToolTip.delay: Kirigami.Units.toolTipDelay

                        onClicked: presetMenu.open()

                        Controls.Menu {
                            id: presetMenu

                            objectName: "presetActionsMenu"
                            y: presetMenuButton.height

                            Controls.MenuItem {
                                text: KI18n.i18nc("@action:inmenu", "Duplicate…")
                                icon.name: "edit-copy"

                                onTriggered: root.newCopy()
                            }

                            Controls.MenuItem {
                                text: KI18n.i18nc("@action:inmenu", "Import…")
                                icon.name: "document-import"

                                onTriggered: root.requestAction("import", "")
                            }

                            Controls.MenuSeparator {}

                            Controls.MenuItem {
                                text: KI18n.i18nc("@action:inmenu", "Remove")
                                icon.name: "edit-delete"
                                enabled: root.editor.canRemove

                                onTriggered: root.requestAction("remove", "")
                            }
                        }
                    }
                }
            }

            Forms.FormEntry {
                title: KI18n.i18nc("@label:textbox", "Name:")

                contentItem: Controls.TextField {
                    id: presetName

                    objectName: "presetName"
                    Layout.fillWidth: true
                    maximumLength: 80
                    text: root.editor.name

                    onTextEdited: root.editor.name = text
                }
            }

            Forms.FormEntry {
                title: KI18n.i18nc("@label:textbox", "Command:")

                contentItem: Controls.TextField {
                    objectName: "presetCommand"
                    Layout.fillWidth: true
                    text: root.editor.command
                    placeholderText: KI18n.i18nc("@info:placeholder", "System shell")

                    onTextEdited: root.editor.command = text
                }
            }

            Forms.FormEntry {
                title: KI18n.i18nc("@label:listbox", "Starting folder:")

                contentItem: Controls.ComboBox {
                    objectName: "presetDirectoryMode"
                    Layout.fillWidth: true
                    model: [KI18n.i18nc("@item:inlistbox", "Current folder"), KI18n.i18nc("@item:inlistbox", "Home folder"), KI18n.i18nc("@item:inlistbox", "Custom folder")]
                    currentIndex: root.editor.directoryMode

                    onActivated: index => root.editor.directoryMode = index
                }
            }

            Forms.FormEntry {
                title: KI18n.i18nc("@label:textbox", "Folder:")
                visible: root.editor.directoryMode === 2

                contentItem: RowLayout {
                    Kirigami.FormData.buddyFor: presetDirectory

                    Controls.TextField {
                        id: presetDirectory

                        objectName: "presetDirectory"
                        Layout.fillWidth: true
                        text: root.editor.directory

                        onTextEdited: root.editor.directory = text
                    }

                    Controls.Button {
                        icon.name: "document-open-folder"
                        text: KI18n.i18nc("@action:button", "Choose…")
                        display: Controls.AbstractButton.IconOnly
                        Controls.ToolTip.text: KI18n.i18nc("@info:tooltip", "Choose starting folder")
                        Controls.ToolTip.visible: hovered
                        Controls.ToolTip.delay: Kirigami.Units.toolTipDelay

                        onClicked: folderDialog.open()
                    }
                }
            }

            Forms.FormEntry {
                title: KI18n.i18nc("@label", "Environment:")

                contentItem: ColumnLayout {
                    Controls.Label {
                        Layout.fillWidth: true
                        visible: root.editor.environmentEntries.length === 0
                        text: KI18n.i18nc("@info", "No additional variables.")
                        wrapMode: Text.Wrap
                    }

                    Repeater {
                        id: environmentRows

                        model: root.editor.environmentEntries

                        delegate: EnvironmentRow {}

                        component EnvironmentRow: RowLayout {
                            id: variableEntry

                            required property int index
                            required property var modelData

                            function focusEditor(): void {
                                editVariableButton.forceActiveFocus(Qt.OtherFocusReason);
                            }

                            Layout.fillWidth: true

                            Controls.Label {
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                Layout.preferredWidth: Kirigami.Units.gridUnit * 8
                                text: variableEntry.modelData.name
                                textFormat: Text.PlainText
                                font.bold: true
                                elide: Text.ElideRight
                            }

                            Controls.Label {
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                Layout.preferredWidth: Kirigami.Units.gridUnit * 12
                                text: variableEntry.modelData.value || KI18n.i18nc("@item environment variable value", "(empty)")
                                elide: Text.ElideRight
                                textFormat: Text.PlainText
                            }

                            Controls.ToolButton {
                                id: editVariableButton

                                objectName: "editEnvironmentVariable" + variableEntry.index
                                text: KI18n.i18nc("@action:button", "Edit %1", variableEntry.modelData.name)
                                icon.name: "document-edit"
                                display: Controls.AbstractButton.IconOnly
                                Controls.ToolTip.text: text
                                Controls.ToolTip.visible: hovered
                                Controls.ToolTip.delay: Kirigami.Units.toolTipDelay

                                onClicked: root.editVariable(variableEntry.index)
                            }

                            Controls.ToolButton {
                                objectName: "removeEnvironmentVariable" + variableEntry.index
                                text: KI18n.i18nc("@action:button", "Remove %1", variableEntry.modelData.name)
                                icon.name: "list-remove"
                                display: Controls.AbstractButton.IconOnly
                                Controls.ToolTip.text: text
                                Controls.ToolTip.visible: hovered
                                Controls.ToolTip.delay: Kirigami.Units.toolTipDelay

                                onClicked: {
                                    const row = variableEntry.index;
                                    root.editor.removeEnvironmentVariable(row);
                                    root.focusVariable(Math.min(row, environmentRows.count - 1));
                                }
                            }
                        }
                    }

                    Controls.Button {
                        id: addVariableButton

                        objectName: "addEnvironmentVariable"
                        text: KI18n.i18nc("@action:button", "Add Variable…")
                        icon.name: "list-add"

                        onClicked: root.editVariable(-1)
                    }
                }
            }
        }
    }

    Kirigami.PromptDialog {
        id: unsavedDialog

        objectName: "unsavedPresetDialog"
        parent: Controls.Overlay.overlay
        title: KI18n.i18nc("@title:window", "Unsaved Preset Changes")
        subtitle: KI18n.i18nc("@info", "Save changes before continuing?")
        dialogType: Kirigami.PromptDialog.Warning
        preferredWidth: Kirigami.Units.gridUnit * 24
        standardButtons: Controls.Dialog.Save | Controls.Dialog.Discard | Controls.Dialog.Cancel
        closePolicy: Controls.Popup.CloseOnEscape
        focus: true

        property string pendingAction
        property string pendingName

        onAccepted: {
            if (root.editor.save())
                root.performAction(pendingAction, pendingName);
        }

        onDiscarded: {
            close();
            root.editor.discard();
            root.performAction(pendingAction, pendingName);
        }

        onOpened: standardButton(Controls.Dialog.Cancel).forceActiveFocus(Qt.PopupFocusReason)
    }

    Kirigami.PromptDialog {
        id: environmentDialog

        objectName: "environmentVariableDialog"
        parent: Controls.Overlay.overlay
        title: entryIndex < 0 ? KI18n.i18nc("@title:window", "Add Environment Variable") : KI18n.i18nc("@title:window", "Edit Environment Variable")
        preferredWidth: Kirigami.Units.gridUnit * 24
        closePolicy: Controls.Popup.CloseOnEscape
        focus: true

        footer: PromptDialogFooter {
            onRejected: environmentDialog.reject()

            action: Kirigami.Action {
                objectName: "saveEnvironmentVariableAction"
                text: environmentDialog.entryIndex < 0 ? KI18n.i18nc("@action:button", "Add") : KI18n.i18nc("@action:button", "Save")
                icon.name: environmentDialog.entryIndex < 0 ? "list-add" : "document-save"
                enabled: environmentDialog.inputError.length === 0

                onTriggered: environmentDialog.saveVariable()
            }
        }

        property int entryIndex: -1
        property int acceptedIndex: -1
        readonly property string inputError: root.editor.environmentVariableError(entryIndex, variableName.text, variableValue.text)

        function saveVariable(): void {
            const row = entryIndex < 0 ? root.editor.environmentEntries.length : entryIndex;

            if (inputError.length === 0 && root.editor.setEnvironmentVariable(entryIndex, variableName.text, variableValue.text)) {
                acceptedIndex = row;
                accept();
            }
        }

        ColumnLayout {
            Kirigami.FormLayout {
                Layout.fillWidth: true

                Controls.TextField {
                    id: variableName

                    objectName: "environmentVariableName"
                    Kirigami.FormData.label: KI18n.i18nc("@label:textbox environment variable name", "Name:")
                    Layout.fillWidth: true
                    maximumLength: 65536

                    onAccepted: variableValue.forceActiveFocus(Qt.TabFocusReason)
                }

                Controls.TextField {
                    id: variableValue

                    objectName: "environmentVariableValue"
                    Kirigami.FormData.label: KI18n.i18nc("@label:textbox environment variable value", "Value:")
                    Layout.fillWidth: true
                    maximumLength: 65536

                    onAccepted: environmentDialog.saveVariable()
                }
            }

            Kirigami.InlineMessage {
                Layout.fillWidth: true
                visible: variableName.text.length > 0 && text.length > 0
                text: TextFormatting.plainTextToHtml(environmentDialog.inputError)
                type: Kirigami.MessageType.Error
            }
        }

        onOpened: variableName.forceActiveFocus(Qt.PopupFocusReason)
        onClosed: {
            if (acceptedIndex >= 0)
                root.focusVariable(acceptedIndex);
        }
    }

    Kirigami.PromptDialog {
        id: removeDialog

        objectName: "removePresetDialog"
        parent: Controls.Overlay.overlay
        title: KI18n.i18nc("@title:window", "Remove Preset")
        dialogType: Kirigami.PromptDialog.Warning
        preferredWidth: Kirigami.Units.gridUnit * 24
        closePolicy: Controls.Popup.CloseOnEscape
        focus: true

        footer: PromptDialogFooter {
            id: removeDialogFooter

            actionRole: Controls.DialogButtonBox.DestructiveRole
            onRejected: removeDialog.reject()

            action: Kirigami.Action {
                objectName: "removePresetAction"
                text: KI18n.i18nc("@action:button", "Remove")
                icon.name: "edit-delete"

                onTriggered: {
                    if (root.editor.removeCurrent())
                        removeDialog.accept();
                }
            }
        }

        ColumnLayout {
            Controls.Label {
                Layout.fillWidth: true
                text: root.editor.selectedName === root.terminalPresets.defaultPresetName
                    ? KI18n.i18nc("@info", "Remove preset “%1”? New sessions will use System Shell.", root.editor.selectedName)
                    : KI18n.i18nc("@info", "Remove preset “%1”?", root.editor.selectedName)
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
            }

            Kirigami.InlineMessage {
                Layout.fillWidth: true
                visible: text.length > 0
                text: TextFormatting.plainTextToHtml(root.editor.errorString)
                type: Kirigami.MessageType.Error
            }
        }

        onOpened: removeDialogFooter.focusCancel()
    }

    Dialogs.FileDialog {
        id: importDialog

        title: KI18n.i18nc("@title:window", "Import Terminal Preset")
        nameFilters: [KI18n.i18nc("@item:inlistbox", "Zirq Launch Presets (*.preset)")]

        onAccepted: root.terminalPresets.importPreset(selectedFile)
    }

    Dialogs.FolderDialog {
        id: folderDialog

        title: KI18n.i18nc("@title:window", "Choose Starting Folder")

        onAccepted: root.editor.setDirectoryUrl(selectedFolder)
    }
}
