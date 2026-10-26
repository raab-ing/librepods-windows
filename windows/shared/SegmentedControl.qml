pragma ComponentBehavior: Bound

import QtQuick 2.15
import QtQuick.Controls 2.15

Control {
    id: root

    property var model: ["Option 1", "Option 2"]
    property int currentIndex: 0
    // Clicks emit this instead of assigning currentIndex — assigning to a
    // bound property destroys the binding, so the indicator would freeze on
    // the last click and stop tracking the device state. Callers keep
    // currentIndex bound and update the source on activated().
    signal activated(int index)

    readonly property color backgroundColor: palette.base
    readonly property color selectedColor: palette.window
    readonly property color textColor: palette.buttonText
    readonly property color selectedTextColor: palette.text

    SystemPalette { id: palette }

    padding: 3
    implicitWidth: 300
    implicitHeight: 28

    focusPolicy: Qt.StrongFocus
    activeFocusOnTab: true

    background: Rectangle {
        radius: height / 2
        color: root.backgroundColor
        border.width: root.activeFocus ? 1 : 0
        border.color: palette.highlight
    }

    contentItem: Item {
        readonly property real segWidth: root.availableWidth / root.model.length

        // Soft elevation shadow under the selected chip — tracks the same
        // slide, offset down so the active segment reads raised off the track.
        Rectangle {
            width: parent.segWidth - 6
            height: root.availableHeight - 4
            x: root.currentIndex * parent.segWidth + 3
            y: 3
            radius: height / 2
            color: "#000000"
            opacity: 0.28
            Behavior on x { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }
        }

        // sliding selection indicator
        Rectangle {
            width: parent.segWidth
            height: root.availableHeight
            x: root.currentIndex * parent.segWidth
            radius: height / 2
            color: root.selectedColor
            border.color: Qt.rgba(0, 0, 0, 0.12)
            border.width: 1

            // lit top edge so the chip reads as a raised surface
            Rectangle {
                x: 2; y: 1
                width: parent.width - 4; height: 1
                color: "#ffffff"; opacity: 0.10
            }

            Behavior on x { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }
        }

        Row {
            anchors.fill: parent
            Repeater {
                model: root.model
                delegate: Item {
                    required property int index
                    required property string modelData
                    width: parent.parent.segWidth
                    height: parent.height

                    Text {
                        anchors.centerIn: parent
                        text: modelData
                        font.pixelSize: 11
                        font.weight: root.currentIndex === index ? Font.DemiBold : Font.Normal
                        color: root.currentIndex === index ? root.selectedTextColor
                               : segHover.containsMouse ? root.textColor
                               : Qt.rgba(root.textColor.r, root.textColor.g, root.textColor.b, 0.72)
                        elide: Text.ElideRight
                        Behavior on color { ColorAnimation { duration: 120 } }
                    }

                    MouseArea {
                        id: segHover
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: if (root.currentIndex !== index) root.activated(index)
                    }
                }
            }
        }
    }

    Keys.onPressed: event => {
        if (event.key === Qt.Key_Left) {
            if (root.currentIndex > 0) { root.activated(root.currentIndex - 1); event.accepted = true; }
        } else if (event.key === Qt.Key_Right) {
            if (root.currentIndex < root.model.length - 1) { root.activated(root.currentIndex + 1); event.accepted = true; }
        } else if (event.key === Qt.Key_Home) {
            root.activated(0); event.accepted = true;
        } else if (event.key === Qt.Key_End) {
            root.activated(root.model.length - 1); event.accepted = true;
        } else if (event.key >= Qt.Key_1 && event.key <= Qt.Key_9) {
            const index = event.key - Qt.Key_1;
            if (index <= root.model.length) { root.activated(index); event.accepted = true; }
        }
    }
}
