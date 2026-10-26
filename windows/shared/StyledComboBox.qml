pragma ComponentBehavior: Bound

import QtQuick 2.15
import QtQuick.Controls 2.15

// Dark themed combo box — flat button face, chevron indicator, themed popup.
// String-array models only (all call sites use plain string lists).
ComboBox {
    id: control

    implicitWidth: Math.max(contentItem.implicitWidth + 40, 118)
    implicitHeight: 28
    padding: 10
    rightPadding: 26

    delegate: ItemDelegate {
        id: itemDelegate
        required property int index
        required property var modelData
        width: control.width - 8
        padding: 8
        highlighted: control.highlightedIndex === index

        contentItem: Text {
            text: itemDelegate.modelData
            color: itemDelegate.highlighted ? control.palette.highlightedText
                                            : control.palette.text
            font.pixelSize: 12
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            radius: 4
            color: itemDelegate.highlighted ? control.palette.highlight : "transparent"
        }
    }

    indicator: Text {
        x: control.width - width - 9
        y: Math.round((control.height - height) / 2)
        text: "▾"
        color: control.palette.buttonText
        font.pixelSize: 11
    }

    contentItem: Text {
        leftPadding: 2
        rightPadding: control.indicator.width + control.spacing
        text: control.displayText
        font.pixelSize: 12
        color: control.enabled ? control.palette.buttonText : control.palette.mid
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    background: Rectangle {
        implicitWidth: 118
        implicitHeight: 28
        radius: 6
        color: control.pressed ? control.palette.dark
             : control.hovered ? Qt.lighter(control.palette.button, 1.12)
             : control.palette.button
        border.color: control.activeFocus ? control.palette.highlight : control.palette.mid
        border.width: control.activeFocus ? 1.5 : 1
        opacity: control.enabled ? 1.0 : 0.5
        Behavior on color { ColorAnimation { duration: 110 } }
        Behavior on border.color { ColorAnimation { duration: 110 } }
    }

    popup: Popup {
        y: control.height + 2
        width: control.width
        implicitHeight: contentItem.implicitHeight + 8
        padding: 4

        enter: Transition {
            NumberAnimation { property: "opacity"; from: 0.0; to: 1.0; duration: 110 }
            NumberAnimation { property: "scale"; from: 0.94; to: 1.0; duration: 110; easing.type: Easing.OutCubic }
        }
        exit: Transition {
            NumberAnimation { property: "opacity"; to: 0.0; duration: 70 }
        }

        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: control.popup.visible ? control.delegateModel : null
            currentIndex: control.highlightedIndex
            ScrollIndicator.vertical: ScrollIndicator { }
        }

        background: Rectangle {
            radius: 6
            color: control.palette.base
            border.color: control.palette.mid
            border.width: 1
        }
    }
}
