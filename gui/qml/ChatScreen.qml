import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15

ApplicationWindow {
    id: root
    visible: true
    width: 720
    height: 900
    title: "remclaude"

    // ── Palette (from confucius) ─────────────────────────────────────────────
    readonly property color bg:         "#1e1e2e"
    readonly property color surface:    "#2a2a3e"
    readonly property color userBubble: "#3a5a8a"
    readonly property color aiBubble:   "#2e4a3e"
    readonly property color textPrimary:"#e0e0e0"
    readonly property color textDim:    "#888899"
    readonly property color accent:     "#7b9ac4"

    readonly property bool canSend: remote.connected && remote.currentSession !== "" && !remote.busy

    color: root.bg

    // Avoid Connections targeting a C++ context property (noisy Qt 6.2 warning).
    Component.onCompleted: {
        remote.errorOccurred.connect(function(msg) { toast.show(msg) })
        if (remote.host === "" || remote.user === "")
            settingsDrawer.open()
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ── Header ───────────────────────────────────────────────────────────
        Rectangle {
            Layout.fillWidth: true
            height: 52
            color: root.surface

            RowLayout {
                anchors { fill: parent; leftMargin: 8; rightMargin: 8 }
                spacing: 8

                HeaderButton { kind: "menu"; onClicked: sessionsDrawer.open() }

                Rectangle {
                    width: 10; height: 10; radius: 5
                    color: remote.connected ? "#7abf7a" : "#b05050"
                }

                Label {
                    text: remote.currentSession !== "" ? remote.currentSession : "remclaude"
                    font.pixelSize: 18
                    font.bold: true
                    color: root.textPrimary
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }

                Label {
                    visible: remote.busy
                    text: "Claude is working…"
                    color: root.textDim
                    font.pixelSize: 13
                }

                HeaderButton {
                    visible: remote.busy
                    kind: "stop"
                    onClicked: remote.cancel()
                }

                HeaderButton { kind: "more"; onClicked: settingsDrawer.open() }
            }
        }

        // ── Message list ─────────────────────────────────────────────────────
        ListView {
            id: messageList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: remote.messages
            spacing: 8
            leftMargin: 12
            rightMargin: 12
            topMargin: 12
            bottomMargin: 12

            ScrollBar.vertical: ScrollBar {}

            onCountChanged: Qt.callLater(function() { messageList.positionViewAtEnd() })

            delegate: Item {
                width: messageList.width - messageList.leftMargin - messageList.rightMargin
                height: bubble.implicitHeight + 4

                readonly property bool isUser: modelData.role === "user"

                Rectangle {
                    id: bubble
                    width: Math.min(bubbleText.implicitWidth + 24, parent.width * 0.86)
                    implicitHeight: bubbleText.implicitHeight + 18
                    anchors.right: isUser ? parent.right : undefined
                    anchors.left:  isUser ? undefined     : parent.left
                    radius: 12
                    color: isUser ? root.userBubble : root.aiBubble

                    TextEdit {
                        id: bubbleText
                        anchors {
                            left: parent.left; right: parent.right
                            top: parent.top; bottom: parent.bottom
                            margins: 9; leftMargin: 12; rightMargin: 12
                        }
                        text: modelData.text
                        textFormat: isUser ? TextEdit.PlainText : TextEdit.MarkdownText
                        readOnly: true
                        selectByMouse: true
                        selectionColor: root.accent
                        color: root.textPrimary
                        wrapMode: TextEdit.Wrap
                        font.pixelSize: 14
                    }
                }
            }

            Label {
                anchors.centerIn: parent
                width: parent.width - 48
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                visible: remote.messages.length === 0
                text: !remote.connected
                      ? (remote.busy ? "Connecting…" : "Not connected — open Settings (top right)")
                      : remote.currentSession === ""
                        ? "Open the menu (top left) to pick or create a session"
                        : "Send a prompt to Claude…"
                color: root.textDim
                font.pixelSize: 14
            }
        }

        // ── Cost line ────────────────────────────────────────────────────────
        Label {
            visible: remote.lastCost > 0
            Layout.fillWidth: true
            Layout.leftMargin: 12
            Layout.bottomMargin: 2
            text: "last prompt: $" + remote.lastCost.toFixed(4)
            color: root.textDim
            font.pixelSize: 11
        }

        Rectangle { Layout.fillWidth: true; height: 1; color: root.surface }

        // ── Input row ────────────────────────────────────────────────────────
        Rectangle {
            Layout.fillWidth: true
            height: 56
            color: root.surface

            RowLayout {
                anchors { fill: parent; margins: 8 }
                spacing: 8

                TextField {
                    id: inputField
                    Layout.fillWidth: true
                    placeholderText: root.canSend ? "Prompt for Claude…"
                                   : remote.busy  ? "Waiting for Claude…"
                                   : "Connect and pick a session first"
                    enabled: root.canSend
                    color: root.textPrimary
                    placeholderTextColor: root.textDim
                    font.pixelSize: 14
                    background: Rectangle {
                        radius: 8
                        color: root.bg
                        border.color: inputField.activeFocus ? root.accent : "#44445a"
                        border.width: 1
                    }

                    Keys.onReturnPressed: sendBtn.clicked()

                    // Let the compositor see bare modifier presses (layout switching).
                    Keys.onPressed: function(event) {
                        const modKeys = [
                            Qt.Key_Shift, Qt.Key_Alt, Qt.Key_Control, Qt.Key_Meta,
                            Qt.Key_AltGr, Qt.Key_Super_L, Qt.Key_Super_R,
                            Qt.Key_Mode_switch
                        ]
                        if (modKeys.indexOf(event.key) >= 0)
                            event.accepted = false
                    }
                }

                Button {
                    id: sendBtn
                    text: "Send"
                    enabled: root.canSend && (inputField.text + inputField.preeditText).trim() !== ""
                    onClicked: {
                        if (!enabled) return
                        Qt.inputMethod.commit()   // include Android pre-edit text
                        remote.sendMessage(inputField.text.trim())
                        inputField.clear()
                    }
                    contentItem: Label {
                        text: "Send"
                        color: root.textPrimary
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        font.pixelSize: 14
                    }
                    background: Rectangle {
                        radius: 8
                        color: sendBtn.enabled ? root.accent : "#3a3a4e"
                    }
                }
            }
        }
    }

    component HeaderButton: ToolButton {
        property string kind: "menu"
        implicitWidth: 44
        contentItem: Item { Icon { anchors.centerIn: parent; kind: parent.parent.kind; color: root.textPrimary } }
        background: Rectangle { color: "transparent" }
    }

    // ── Sessions drawer (left) ───────────────────────────────────────────────
    Drawer {
        id: sessionsDrawer
        width: Math.min(400, root.width * 0.85)
        height: root.height
        edge: Qt.LeftEdge
        onOpened: if (remote.connected) remote.refreshSessions()

        SessionsScreen {
            anchors.fill: parent
            onDone: sessionsDrawer.close()
        }
    }

    // ── Settings drawer (right) ──────────────────────────────────────────────
    Drawer {
        id: settingsDrawer
        width: Math.min(440, root.width * 0.92)
        height: root.height
        edge: Qt.RightEdge

        SettingsScreen {
            anchors.fill: parent
            onDone: settingsDrawer.close()
        }
    }

    // ── Toast ────────────────────────────────────────────────────────────────
    Rectangle {
        id: toast
        anchors { bottom: parent.bottom; horizontalCenter: parent.horizontalCenter; bottomMargin: 80 }
        width: toastLabel.width + 32
        height: toastLabel.implicitHeight + 16
        radius: 8
        color: "#b03030"
        visible: false
        z: 10

        Label {
            id: toastLabel
            anchors.centerIn: parent
            text: ""
            color: "white"
            font.pixelSize: 13
            wrapMode: Text.WordWrap
            width: Math.min(implicitWidth, root.width - 64)
        }

        MouseArea { anchors.fill: parent; onClicked: toast.visible = false }

        function show(msg) {
            toastLabel.text = msg
            visible = true
            hideTimer.restart()
        }

        Timer {
            id: hideTimer
            interval: 6000
            onTriggered: toast.visible = false
        }
    }
}
