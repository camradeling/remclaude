import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15

Item {
    id: screen
    signal done()

    readonly property color bg:          "#1e1e2e"
    readonly property color surface:     "#2a2a3e"
    readonly property color textPrimary: "#e0e0e0"
    readonly property color textDim:     "#888899"
    readonly property color accent:      "#7b9ac4"
    readonly property color danger:      "#5a3a3a"

    property string pendingDelete: ""

    Rectangle { anchors.fill: parent; color: bg }

    ColumnLayout {
        anchors { fill: parent; margins: 20 }
        spacing: 14

        RowLayout {
            Layout.fillWidth: true
            Label {
                text: "Sessions"
                font.pixelSize: 20
                font.bold: true
                color: textPrimary
                Layout.fillWidth: true
            }
            ToolButton {
                enabled: remote.connected && !remote.busy
                onClicked: remote.refreshSessions()
                contentItem: Label {
                    text: "Refresh"; font.pixelSize: 13
                    color: parent.enabled ? textPrimary : textDim
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle { color: "transparent" }
            }
        }

        Rectangle { Layout.fillWidth: true; height: 1; color: surface }

        // ── New session ──────────────────────────────────────────────────────
        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            TextField {
                id: newName
                Layout.fillWidth: true
                placeholderText: "New session name"
                enabled: remote.connected
                color: textPrimary
                placeholderTextColor: textDim
                font.pixelSize: 14
                background: Rectangle {
                    radius: 6
                    color: surface
                    border.color: newName.activeFocus ? accent : "#44445a"
                    border.width: 1
                }
                Keys.onReturnPressed: createBtn.clicked()
            }

            Button {
                id: createBtn
                enabled: remote.connected && !remote.busy && (newName.text + newName.preeditText).trim() !== ""
                onClicked: {
                    if (!enabled) return
                    Qt.inputMethod.commit()   // include Android pre-edit text
                    remote.createSession(newName.text.trim())
                    newName.clear()
                    screen.done()
                }
                contentItem: Label {
                    text: "Create"; color: textPrimary; font.pixelSize: 14
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle { radius: 8; color: createBtn.enabled ? accent : "#3a3a4e" }
            }
        }

        // ── List ─────────────────────────────────────────────────────────────
        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 6
            model: remote.sessions
            ScrollBar.vertical: ScrollBar {}

            delegate: Rectangle {
                width: list.width
                height: 56
                radius: 8
                readonly property bool isCurrent: modelData.name === remote.currentSession
                color: isCurrent ? "#34345a" : surface
                border.color: isCurrent ? accent : "transparent"
                border.width: 1

                MouseArea {
                    anchors.fill: parent
                    onClicked: {
                        remote.selectSession(modelData.name)
                        screen.done()
                    }
                }

                RowLayout {
                    anchors { fill: parent; leftMargin: 12; rightMargin: 4 }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Label {
                            text: modelData.name
                            color: textPrimary
                            font.pixelSize: 15
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                        Label {
                            text: "last used: " + (modelData.last_used || "—")
                            color: textDim
                            font.pixelSize: 11
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                    }
                    ToolButton {
                        enabled: !remote.busy
                        onClicked: {
                            screen.pendingDelete = modelData.name
                            confirmDelete.open()
                        }
                        implicitWidth: 44
                        contentItem: Item { Icon { anchors.centerIn: parent; kind: "close"; color: textDim } }
                        background: Rectangle { color: "transparent" }
                    }
                }
            }

            Label {
                anchors.centerIn: parent
                visible: list.count === 0
                text: remote.connected ? "No sessions yet" : "Not connected"
                color: textDim
                font.pixelSize: 14
            }
        }
    }

    Dialog {
        id: confirmDelete
        anchors.centerIn: parent
        width: Math.min(parent.width - 40, 340)
        modal: true
        standardButtons: Dialog.Yes | Dialog.Cancel
        contentItem: Label {
            wrapMode: Text.WordWrap
            textFormat: Text.StyledText
            text: "<b>Delete session?</b><br><br>“" + screen.pendingDelete +
                  "” and its Claude transcript will be deleted on the server."
        }
        onAccepted: remote.deleteSession(screen.pendingDelete)
    }
}
