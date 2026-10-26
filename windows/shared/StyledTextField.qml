pragma ComponentBehavior: Bound

import QtQuick 2.15
import QtQuick.Controls 2.15

// Dark themed single-line field — inset bg, accent focus ring.
TextField {
    id: control

    implicitHeight: 28
    color: control.palette.text
    placeholderTextColor: control.palette.placeholderText
    selectionColor: control.palette.highlight
    selectedTextColor: control.palette.highlightedText
    font.pixelSize: 12
    leftPadding: 8
    rightPadding: 8

    background: Rectangle {
        implicitWidth: 140
        implicitHeight: 28
        radius: 6
        color: control.palette.base
        border.width: 1
        border.color: control.activeFocus ? control.palette.highlight : control.palette.mid
    }
}
