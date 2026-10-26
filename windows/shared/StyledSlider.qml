pragma ComponentBehavior: Bound

import QtQuick 2.15
import QtQuick.Controls 2.15

// Thin accent-track slider with a floating white knob.
// `bipolar` fills the track from center (for -1..1 ranges like balance/tone).
Slider {
    id: control

    property bool bipolar: false

    implicitHeight: 24
    padding: 2

    background: Rectangle {
        x: control.leftPadding
        y: control.topPadding + Math.round((control.availableHeight - height) / 2)
        width: control.availableWidth
        height: 4
        radius: 2
        color: control.palette.mid

        Rectangle {
            x: control.bipolar
               ? Math.round(Math.min(parent.width / 2, control.visualPosition * parent.width))
               : 0
            width: control.bipolar
                   ? Math.round(Math.abs((control.visualPosition - 0.5) * parent.width))
                   : Math.round(control.visualPosition * parent.width)
            height: parent.height
            radius: 2
            color: control.enabled ? control.palette.highlight : control.palette.dark
        }
    }

    handle: Rectangle {
        x: control.leftPadding + Math.round(control.visualPosition * (control.availableWidth - width))
        y: control.topPadding + Math.round((control.availableHeight - height) / 2)
        width: 16
        height: 16
        radius: 8
        color: control.enabled ? "#ffffff" : control.palette.mid
        border.color: control.activeFocus ? control.palette.highlight : control.palette.dark
        border.width: control.activeFocus ? 1.5 : 1
        scale: control.pressed ? 1.2 : 1.0
        Behavior on border.color { ColorAnimation { duration: 110 } }
        Behavior on scale { NumberAnimation { duration: 100 } }
    }
}
