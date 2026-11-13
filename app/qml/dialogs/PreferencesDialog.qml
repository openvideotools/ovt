// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Preferences (Ctrl+,, chapter 11 §11.2.5, FR-GUI-023): the groups General, Decoding, Picture, Overlays, HDR, Metrics,
// Keyboard and Privacy, each setting bound to `Settings` (settings.json). The rows are data: a setting that no panel
// reads yet is still stored, so the file already carries the user's choice when the panel arrives. Changes apply
// immediately and are validated by the settings schema (a rejected value reverts the control). The language entry
// stores the choice; switching the UI language without restart needs the translation catalogues (FR-GUI-023).

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OvtStudio

Dialog {
    id: dialog

    objectName: "preferencesDialog"
    signal keymapRequested

    // {group, key, label, type: bool|int|choice|text, def, min, max, choices}
    readonly property var rows: [
        {"group": "general", "key": "theme", "label": qsTr("Theme"), "type": "choice", "def": "system", "choices": ["system", "light", "dark"]},
        {"group": "general", "key": "language", "label": qsTr("Language"), "type": "choice", "def": "en", "choices": ["en", "pseudo"]},
        {"group": "general", "key": "recent_files_count", "label": qsTr("Recent files"), "type": "int", "def": 10, "min": 0, "max": 100},
        {"group": "general", "key": "confirm_on_close", "label": qsTr("Confirm on close"), "type": "bool", "def": true},
        {"group": "decoding", "key": "prefer_hw", "label": qsTr("Prefer hardware decoding"), "type": "bool", "def": false},
        {"group": "decoding", "key": "threads", "label": qsTr("Decoder threads (0 = automatic)"), "type": "int", "def": 0, "min": 0, "max": 256},
        {"group": "decoding", "key": "auto_analyse_on_overlay_mode", "label": qsTr("Auto-analyse on overlay mode"), "type": "bool", "def": !Platform.isAndroid},
        {"group": "decoding", "key": "cache_size_mb", "label": qsTr("Cache size limit (MB)"), "type": "int", "def": 2048, "min": 0, "max": 1048576},
        {"group": "picture", "key": "default_zoom", "label": qsTr("Default zoom"), "type": "choice", "def": "fit", "choices": ["fit", "100", "200", "400"]},
        {"group": "picture", "key": "chroma_upscale", "label": qsTr("Chroma upscale"), "type": "choice", "def": "bilinear", "choices": ["nearest", "bilinear", "bicubic"]},
        {"group": "picture", "key": "downscale_filter", "label": qsTr("Downscale filter"), "type": "choice", "def": "bilinear", "choices": ["nearest", "bilinear", "lanczos"]},
        {"group": "picture", "key": "background_colour", "label": qsTr("Background colour"), "type": "text", "def": "#1b1f27"},
        {"group": "overlays", "key": "default_mode", "label": qsTr("Default overlay mode (1 to 12)"), "type": "int", "def": 1, "min": 1, "max": 12},
        {"group": "overlays", "key": "colour_blind_safe", "label": qsTr("Colour-blind-safe palette"), "type": "bool", "def": false},
        {"group": "hdr", "key": "tone_map", "label": qsTr("Default tone map"), "type": "choice", "def": "none", "choices": ["none", "reinhard", "bt2390", "hable"]},
        {"group": "hdr", "key": "clip_nits", "label": qsTr("Clip level (nits)"), "type": "int", "def": 1000, "min": 1, "max": 10000},
        {"group": "metrics", "key": "threads", "label": qsTr("Metric threads (0 = automatic)"), "type": "int", "def": 0, "min": 0, "max": 256},
        {"group": "privacy", "key": "corpora_download_consent", "label": qsTr("Allow downloading test corpora"), "type": "bool", "def": false}
    ]
    readonly property var groups: [
        {"id": "general", "label": qsTr("General")}, {"id": "decoding", "label": qsTr("Decoding")},
        {"id": "picture", "label": qsTr("Picture")}, {"id": "overlays", "label": qsTr("Overlays")},
        {"id": "hdr", "label": qsTr("HDR")}, {"id": "metrics", "label": qsTr("Metrics")},
        {"id": "keyboard", "label": qsTr("Keyboard")}, {"id": "privacy", "label": qsTr("Privacy")}
    ]
    property int currentGroup: 0

    title: qsTr("Preferences")
    modal: true
    anchors.centerIn: parent
    width: 640
    height: 420
    standardButtons: Dialog.Close

    RowLayout {
        anchors.fill: parent
        spacing: OvtStyle.spacing * 2

        ListView {
            id: groupList

            objectName: "preferencesGroups"
            Layout.preferredWidth: 140
            Layout.fillHeight: true
            model: dialog.groups
            currentIndex: dialog.currentGroup
            Accessible.name: qsTr("Preference groups")
            delegate: ItemDelegate {
                required property var modelData
                required property int index

                width: ListView.view.width
                text: modelData.label
                highlighted: index === dialog.currentGroup
                onClicked: dialog.currentGroup = index
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: OvtStyle.spacing

            Repeater {
                model: dialog.rows.filter(r => r.group === dialog.groups[dialog.currentGroup].id)

                delegate: RowLayout {
                    id: row

                    required property var modelData
                    readonly property string settingId: modelData.group + "." + modelData.key

                    Layout.fillWidth: true
                    spacing: OvtStyle.spacing * 2

                    Label {
                        text: row.modelData.label
                        Layout.preferredWidth: 260
                        wrapMode: Text.Wrap
                    }
                    Loader {
                        Layout.fillWidth: true
                        sourceComponent: row.modelData.type === "bool" ? boolEditor
                                         : row.modelData.type === "int" ? intEditor
                                         : row.modelData.type === "choice" ? choiceEditor : textEditor
                        property var setting: row.modelData
                    }
                }
            }

            Button {
                visible: dialog.groups[dialog.currentGroup].id === "keyboard"
                objectName: "preferencesKeymap"
                text: qsTr("Edit keyboard map…")
                onClicked: dialog.keymapRequested()
            }
            Label {
                visible: dialog.groups[dialog.currentGroup].id === "keyboard"
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: OvtStyle.textMuted
                text: qsTr("The map is saved to %1").arg(Keymap.path)
            }
            Item { Layout.fillHeight: true }
        }
    }

    // Editors: every one reads the stored value (Settings.value) and writes it back on change; a value the schema
    // rejects is shown reverted.
    Component {
        id: boolEditor

        Switch {
            objectName: "pref_" + setting.group + "_" + setting.key
            checked: (Settings.revision, Settings.value(setting.group, setting.key, setting.def)) === true
            Accessible.name: setting.label
            onToggled: if (!Settings.setValue(setting.group, setting.key, checked))
                checked = Qt.binding(() => (Settings.revision, Settings.value(setting.group, setting.key, setting.def)) === true)
        }
    }

    Component {
        id: intEditor

        SpinBox {
            objectName: "pref_" + setting.group + "_" + setting.key
            from: setting.min
            to: setting.max
            editable: true
            value: Number((Settings.revision, Settings.value(setting.group, setting.key, setting.def)))
            Accessible.name: setting.label
            onValueModified: Settings.setValue(setting.group, setting.key, value)
        }
    }

    Component {
        id: choiceEditor

        ComboBox {
            objectName: "pref_" + setting.group + "_" + setting.key
            model: setting.choices
            currentIndex: Math.max(0, setting.choices.indexOf(String((Settings.revision, Settings.value(setting.group, setting.key, setting.def)))))
            Accessible.name: setting.label
            onActivated: index => Settings.setValue(setting.group, setting.key, setting.choices[index])
        }
    }

    Component {
        id: textEditor

        TextField {
            objectName: "pref_" + setting.group + "_" + setting.key
            text: String((Settings.revision, Settings.value(setting.group, setting.key, setting.def)))
            Accessible.name: setting.label
            onEditingFinished: Settings.setValue(setting.group, setting.key, text)
        }
    }
}
