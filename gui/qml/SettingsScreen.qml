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
    readonly property color ok:          "#7abf7a"
    readonly property color bad:         "#c07070"

    Rectangle { anchors.fill: parent; color: bg }

    // Re-check key/token presence while visible (files are copied in from outside).
    property int fileTick: 0
    Timer { interval: 2000; repeat: true; running: screen.visible; onTriggered: screen.fileTick++ }

    function applyFields() {
        // Android keyboards hold the word being typed as uncommitted pre-edit
        // text that TextField.text does not include yet; commit it first.
        Qt.inputMethod.commit()
        remote.host       = hostField.text
        remote.sshPort    = parseInt(sshPortField.text) || 22
        remote.user       = userField.text
        remote.keyPath    = keyField.text
        remote.remoteHost = remoteHostField.text || "127.0.0.1"
        remote.remotePort = parseInt(remotePortField.text) || 8765
        remote.tokenPath  = tokenField.text
    }

    component FieldLabel: Label {
        color: textDim
        font.pixelSize: 12
    }

    component Field: TextField {
        Layout.fillWidth: true
        color: textPrimary
        placeholderTextColor: textDim
        font.pixelSize: 13
        selectByMouse: true
        inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
        background: Rectangle {
            radius: 6
            color: surface
            border.color: parent.activeFocus ? accent : "#44445a"
            border.width: 1
        }
    }

    component ActionButton: Button {
        property color fill: accent
        Layout.fillWidth: true
        contentItem: Label {
            text: parent.text
            color: textPrimary
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            font.pixelSize: 14
        }
        background: Rectangle { radius: 8; color: parent.enabled ? parent.fill : "#3a3a4e" }
    }

    Flickable {
        anchors.fill: parent
        contentWidth: width
        contentHeight: col.height + 48
        clip: true
        ScrollIndicator.vertical: ScrollIndicator {}

        ColumnLayout {
            id: col
            x: 24; y: 24
            width: screen.width - 48
            spacing: 10

            Label {
                text: "Settings"
                font.pixelSize: 20
                font.bold: true
                color: textPrimary
            }

            Rectangle { Layout.fillWidth: true; height: 1; color: surface }

            // ── SSH ──────────────────────────────────────────────────────────
            Label { text: "SSH server"; color: textPrimary; font.pixelSize: 14; font.bold: true }

            FieldLabel { text: "Host / IP" }
            RowLayout {
                Layout.fillWidth: true
                Field { id: hostField; text: remote.host; placeholderText: "192.168.1.10" }
                Field {
                    id: sshPortField
                    Layout.fillWidth: false
                    Layout.preferredWidth: 80
                    text: remote.sshPort
                    placeholderText: "22"
                    inputMethodHints: Qt.ImhDigitsOnly
                }
            }

            FieldLabel { text: "Username" }
            Field { id: userField; text: remote.user; placeholderText: "username" }

            FieldLabel { text: "Private key (OpenSSH format, no passphrase)" }
            Field { id: keyField; text: remote.keyPath }
            Label {
                text: (fileTick, remote.fileExists(keyField.text)) ? "● key file found" : "● key file not found"
                color: (fileTick, remote.fileExists(keyField.text)) ? ok : bad
                font.pixelSize: 12
            }

            Rectangle { Layout.fillWidth: true; height: 1; color: surface }

            // ── remclaude ────────────────────────────────────────────────────
            Label { text: "remclaude server (through the tunnel)"; color: textPrimary; font.pixelSize: 14; font.bold: true }

            FieldLabel { text: "Address and port, as seen from the SSH host" }
            RowLayout {
                Layout.fillWidth: true
                Field { id: remoteHostField; text: remote.remoteHost; placeholderText: "127.0.0.1" }
                Field {
                    id: remotePortField
                    Layout.fillWidth: false
                    Layout.preferredWidth: 80
                    text: remote.remotePort
                    placeholderText: "8765"
                    inputMethodHints: Qt.ImhDigitsOnly
                }
            }

            FieldLabel { text: "Token file (first line is the token)" }
            Field { id: tokenField; text: remote.tokenPath }
            Label {
                text: (fileTick, remote.fileExists(tokenField.text)) ? "● token file found" : "● token file not found"
                color: (fileTick, remote.fileExists(tokenField.text)) ? ok : bad
                font.pixelSize: 12
            }

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: "App data folder (put the key and token here):\n" + remote.dataDir
                color: textDim
                font.pixelSize: 11
            }

            // ── Connect ──────────────────────────────────────────────────────
            Label {
                text: remote.connected ? "● Connected" : (remote.busy ? "● Connecting…" : "● Disconnected")
                color: remote.connected ? ok : textDim
                font.pixelSize: 13
            }

            ActionButton {
                text: remote.connected ? "Reconnect" : "Connect"
                enabled: !remote.busy
                onClicked: {
                    applyFields()
                    remote.connectToServer()
                    screen.done()
                }
            }

            ActionButton {
                visible: remote.connected
                text: "Disconnect"
                fill: "#4a4a5e"
                onClicked: remote.disconnectFromServer()
            }

            Rectangle { Layout.fillWidth: true; height: 1; color: surface }

            ActionButton {
                text: "Clear local history of this session"
                enabled: remote.currentSession !== ""
                fill: "#5a3a3a"
                onClicked: {
                    remote.clearHistory()
                    screen.done()
                }
            }
        }
    }
}
