pragma ComponentBehavior: Bound

import QtQuick 2.15
import QtQuick.Controls 2.15

// iOS-style pill switch — accent fill when on, neutral track when off.
// Colors come from the inherited palette (ApplicationWindow-level).
Switch {
    id: control

    implicitWidth: 46
    implicitHeight: 28
    spacing: 6

    indicator: Rectangle {
        implicitWidth: 42
        implicitHeight: 24
        x: control.leftPadding + Math.round((control.availableWidth - width) / 2)
        y: control.topPadding + Math.round((control.availableHeight - height) / 2)
        radius: height / 2
        color: control.checked ? control.palette.highlight
                               : (control.enabled ? control.palette.mid : control.palette.dark)
        border.color: control.activeFocus ? control.palette.highlight : "transparent"
        border.width: control.activeFocus ? 2 : 0
        opacity: control.enabled ? 1.0 : 0.45
        scale: control.down ? 0.95 : 1.0
        Behavior on color { ColorAnimation { duration: 140 } }
        Behavior on scale { NumberAnimation { duration: 90; easing.type: Easing.OutCubic } }

        Rectangle {
            x: control.checked ? parent.width - width - 2 : 2
            anchors.verticalCenter: parent.verticalCenter
            width: 20
            height: 20
            radius: 10
            color: "#ffffff"
            border.color: "#26000000"
            border.width: 1
            Behavior on x { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
        }
    }

    contentItem: Text {
        leftPadding: control.indicator.width + control.spacing
        text: control.text
        font: control.font
        color: control.palette.windowText
        verticalAlignment: Text.AlignVCenter
    }
}
