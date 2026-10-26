pragma ComponentBehavior: Bound

import QtQuick 2.15
import QtQuick.Controls 2.15

// Dark themed spin box — inset field, edge-docked − / + indicators.
SpinBox {
    id: control

    implicitWidth: 116
    implicitHeight: 28
    font.pixelSize: 12
    // Resting tint for the ± cells — translucent white on dark themes,
    // translucent black on light so the affordance survives both palettes.
    readonly property color cellRestingTint: control.palette.window.hslLightness < 0.5
                                             ? Qt.rgba(1, 1, 1, 0.05) : Qt.rgba(0, 0, 0, 0.045)

    contentItem: TextInput {
        z: 2
        text: control.textFromValue(control.value, control.locale)
        font: control.font
        color: control.palette.text
        selectionColor: control.palette.highlight
        selectedTextColor: control.palette.highlightedText
        horizontalAlignment: Qt.AlignHCenter
        verticalAlignment: Qt.AlignVCenter
        readOnly: !control.editable
        validator: control.validator
        inputMethodHints: control.inputMethodHints
    }

    up.indicator: Rectangle {
        x: control.mirrored ? 1 : control.width - width - 1
        y: 1
        width: 26
        height: control.height - 2
        radius: 5
        color: control.up.pressed ? control.palette.dark
             : control.up.hovered ? control.palette.mid
             : control.cellRestingTint
        // hairline divider toward the field — the +/− cells read as buttons
        // even before hover
        Rectangle {
            anchors { left: control.mirrored ? undefined : parent.left; right: control.mirrored ? parent.right : undefined; verticalCenter: parent.verticalCenter }
            width: 1; height: parent.height - 8
            color: control.palette.mid; opacity: 0.6
        }
        Text {
            anchors.centerIn: parent
            text: "+"
            font.pixelSize: 13
            color: control.palette.buttonText
        }
    }

    down.indicator: Rectangle {
        x: control.mirrored ? control.width - width - 1 : 1
        y: 1
        width: 26
        height: control.height - 2
        radius: 5
        color: control.down.pressed ? control.palette.dark
             : control.down.hovered ? control.palette.mid
             : control.cellRestingTint
        Rectangle {
            anchors { right: control.mirrored ? undefined : parent.right; left: control.mirrored ? parent.left : undefined; verticalCenter: parent.verticalCenter }
            width: 1; height: parent.height - 8
            color: control.palette.mid; opacity: 0.6
        }
        Text {
            anchors.centerIn: parent
            text: "−"
            font.pixelSize: 13
            color: control.palette.buttonText
        }
    }

    background: Rectangle {
        implicitWidth: 116
        implicitHeight: 28
        radius: 6
        color: control.palette.base
        border.color: control.activeFocus ? control.palette.highlight : control.palette.mid
        border.width: control.activeFocus ? 1.5 : 1
        Behavior on border.color { ColorAnimation { duration: 110 } }
    }
}
