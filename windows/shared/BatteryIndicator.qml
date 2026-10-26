import QtQuick 2.15
import QtQuick.Controls 2.15

// Compact battery chip: [L] [mini bar] [60%]
Row {
    id: root

    property int batteryLevel: 50 // 0-100
    property bool isCharging: false
    property string indicator: "" // "L" / "R" / "C" / "H"
    property bool inEar: true
    property color primaryColor: "#1a1a1a"
    property color secondaryColor: "#666666"
    property color trackColor: "#e4e4e4"

    readonly property color lowColor: "#FF453A"
    readonly property color mediumColor: "#FFD60A"
    readonly property color highColor: "#30D158"

    // Continuous level color — red blends to yellow across 0-50, yellow
    // to green across 50-100, so a draining pod visibly cools instead of
    // snapping between thresholds. Charging forces green.
    readonly property color levelColor: {
        if (isCharging) return highColor
        const t = Math.max(0, Math.min(100, batteryLevel))
        if (t <= 50) {
            const f = t / 50.0
            return Qt.rgba(lowColor.r + (mediumColor.r - lowColor.r) * f,
                           lowColor.g + (mediumColor.g - lowColor.g) * f,
                           lowColor.b + (mediumColor.b - lowColor.b) * f, 1)
        }
        const f = (t - 50) / 50.0
        return Qt.rgba(mediumColor.r + (highColor.r - mediumColor.r) * f,
                       mediumColor.g + (highColor.g - mediumColor.g) * f,
                       mediumColor.b + (highColor.b - mediumColor.b) * f, 1)
    }
    readonly property bool lowBattery: !isCharging && batteryLevel <= 20

    spacing: 5
    opacity: inEar ? 1.0 : 0.45
    Behavior on opacity { NumberAnimation { duration: 200 } }

    Text {
        visible: root.indicator !== ""
        text: root.indicator
        color: root.secondaryColor
        font.pixelSize: 11
        font.weight: Font.DemiBold
        anchors.verticalCenter: parent.verticalCenter
    }

    Text {
        visible: root.isCharging
        text: "⚡"
        color: root.highColor
        font.pixelSize: 10
        anchors.verticalCenter: parent.verticalCenter
    }

    // Track: hairline border gives the trough a clean edge on dark cards.
    Rectangle {
        id: track
        width: 32
        height: 5
        radius: 2.5
        color: root.trackColor
        border.color: Qt.rgba(1, 1, 1, 0.06)
        border.width: 1
        anchors.verticalCenter: parent.verticalCenter

        Rectangle {
            id: fill
            width: Math.max(3, track.width * (Math.max(0, Math.min(100, root.batteryLevel)) / 100))
            height: track.height - 2 * track.border.width
            anchors.verticalCenter: parent.verticalCenter
            x: track.border.width
            radius: height / 2

            // Vertical gradient: lighter crown → solid level color gives
            // the 5px bar a hint of a lit tube rather than a flat stripe.
            gradient: Gradient {
                GradientStop { position: 0.0; color: Qt.lighter(root.levelColor, 1.35) }
                GradientStop { position: 0.55; color: root.levelColor }
                GradientStop { position: 1.0; color: Qt.darker(root.levelColor, 1.12) }
            }
            Behavior on width { NumberAnimation { duration: 250; easing.type: Easing.OutCubic } }

            // Charging shimmer: a bright sweep looping across the fill.
            Rectangle {
                visible: root.isCharging
                anchors.fill: parent
                radius: parent.radius
                color: "transparent"
                clip: true
                Rectangle {
                    id: shimmer
                    width: 10
                    height: parent.height
                    radius: height / 2
                    opacity: 0.55
                    gradient: Gradient {
                        orientation: Gradient.Horizontal
                        GradientStop { position: 0.0; color: "transparent" }
                        GradientStop { position: 0.5; color: Qt.lighter(root.levelColor, 1.8) }
                        GradientStop { position: 1.0; color: "transparent" }
                    }
                    SequentialAnimation on x {
                        running: root.isCharging && root.visible
                        loops: Animation.Infinite
                        NumberAnimation { from: -10; to: track.width; duration: 1200; easing.type: Easing.InOutSine }
                    }
                }
            }

            // Low-battery pulse: the fill breathes so a dying pod is noticed.
            SequentialAnimation {
                running: root.lowBattery && root.visible
                loops: Animation.Infinite
                onRunningChanged: if (!running) fill.opacity = 1.0
                NumberAnimation { target: fill; property: "opacity"; to: 0.45; duration: 450; easing.type: Easing.InOutSine }
                NumberAnimation { target: fill; property: "opacity"; to: 1.0; duration: 450; easing.type: Easing.InOutSine }
            }
        }
    }

    Text {
        text: root.batteryLevel + "%"
        color: (root.isCharging || root.batteryLevel <= 20) ? root.levelColor : root.primaryColor
        font.pixelSize: 12
        font.weight: Font.Medium
        anchors.verticalCenter: parent.verticalCenter
        Behavior on color { ColorAnimation { duration: 200 } }
    }
}
