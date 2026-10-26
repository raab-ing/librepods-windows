pragma ComponentBehavior: Bound

import QtQuick 2.15
import QtQuick.Controls 2.15

// Flat themed push button — card-color face, hover lift, press darken.
Button {
    id: control

    implicitHeight: 28
    leftPadding: 14
    rightPadding: 14
    font.pixelSize: 12

    scale: control.down ? 0.97 : 1.0
    Behavior on scale { NumberAnimation { duration: 90; easing.type: Easing.OutCubic } }

    contentItem: Text {
        text: control.text
        font: control.font
        color: control.enabled ? control.palette.buttonText : control.palette.mid
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    background: Rectangle {
        implicitWidth: 80
        implicitHeight: 28
        radius: 6
        color: control.down ? control.palette.dark
             : control.hovered ? Qt.lighter(control.palette.button, 1.12)
             : control.palette.button
        border.color: control.activeFocus ? control.palette.highlight : control.palette.mid
        border.width: control.activeFocus ? 1.5 : 1
        opacity: control.enabled ? 1.0 : 0.5
        Behavior on color { ColorAnimation { duration: 110 } }
        Behavior on border.color { ColorAnimation { duration: 110 } }
    }
}
