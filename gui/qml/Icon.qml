import QtQuick 2.15

// Icons drawn from plain shapes: Android's fonts lack most symbol glyphs
// (☰ ⚙ ■ 🗑 render as boxes there).
//   kind: "menu" (three bars), "more" (three dots), "stop" (square), "close" (×)
Item {
    id: icon
    property string kind: "menu"
    property color color: "#e0e0e0"
    implicitWidth: 22
    implicitHeight: 22

    Column {
        visible: icon.kind === "menu"
        anchors.centerIn: parent
        spacing: 4
        Repeater {
            model: 3
            Rectangle { width: 18; height: 2; radius: 1; color: icon.color }
        }
    }

    Column {
        visible: icon.kind === "more"
        anchors.centerIn: parent
        spacing: 3
        Repeater {
            model: 3
            Rectangle { width: 4; height: 4; radius: 2; color: icon.color }
        }
    }

    Rectangle {
        visible: icon.kind === "stop"
        anchors.centerIn: parent
        width: 12; height: 12; radius: 2
        color: icon.color
    }

    Item {
        visible: icon.kind === "close"
        anchors.centerIn: parent
        width: 14; height: 14
        Rectangle { anchors.centerIn: parent; width: 18; height: 2; radius: 1; rotation: 45;  color: icon.color }
        Rectangle { anchors.centerIn: parent; width: 18; height: 2; radius: 1; rotation: -45; color: icon.color }
    }
}
