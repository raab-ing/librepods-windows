pragma ComponentBehavior: Bound

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtQuick.Window 2.15

ApplicationWindow {
    id: mainWindow
    visible: !airPodsTrayApp.hideOnStart
    // Settings page sizes to its content (never less than the connected
    // main page) so there's no empty tail; the anchors below keep whichever
    // corner the user parked it on fixed while it grows.
    readonly property bool settingsOpen: stackView.depth > 1
    // Battery chips can populate from BLE ads even while the link is down —
    // the disconnected layout reserves their row when they'll show.
    readonly property bool batteryChipsAvailable: airPodsTrayApp.deviceInfo.battery.leftPodAvailable
        || airPodsTrayApp.deviceInfo.battery.rightPodAvailable
        || airPodsTrayApp.deviceInfo.battery.caseAvailable
        || airPodsTrayApp.deviceInfo.battery.headsetAvailable
    width: settingsOpen ? 400 : 340
    // Height comes from the active page's desiredHeight — the main page
    // computes it from its real content bottom so there's no dead space
    // below the toggles.
    height: settingsOpen
            ? Math.max(452, Math.min(820, stackView.currentItem.desiredHeight || 600))
            : Math.max(184, Math.min(820, stackView.currentItem.desiredHeight || 452))
    // The box auto-expands/shrinks as options appear and disappear (adaptive
    // slider, per-model features) — animate it so the change reads smoothly.
    Behavior on height { NumberAnimation { duration: airPodsTrayApp.reduceMotion ? 0 : 160; easing.type: Easing.OutCubic } }
    // Settings widen 340→400: slide too, not snap. onWidthChanged re-anchors
    // x every frame, so the grow stays pinned to the bottom-right corner.
    Behavior on width { NumberAnimation { duration: airPodsTrayApp.reduceMotion ? 0 : 160; easing.type: Easing.OutCubic } }
    minimumWidth: 340
    maximumWidth: 400
    minimumHeight: 180
    maximumHeight: 1200
    title: "LibrePods"
    objectName: "mainWindowObject"
    // StaysOnTop is required for the ear-event popup: raise() alone cannot
    // reorder a non-activated window above the user's focused app, so the
    // flash would render behind whatever has focus. Safe because the flyout
    // auto-dismisses on focus loss — it can't linger on top.
    flags: Qt.Window | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
    color: "transparent"

    // Track the window's right/bottom edges so growing for settings expands
    // up-and-left instead of sliding down past the taskbar. Re-anchoring in
    // onWidth/onHeightChanged triggers onX/onYChanged, which recompute the
    // same anchor value — no drift.
    property real _anchorRight: 0
    property real _anchorBottom: 0
    // Seed wasActive with the launch state: a window created already-active
    // produces no onActiveChanged transition, so without this the first blur
    // would find wasActive still false and never dismiss.
    Component.onCompleted: { _anchorRight = x + width; _anchorBottom = y + height; wasActive = active }
    onXChanged: _anchorRight = x + width
    onYChanged: _anchorBottom = y + height
    onWidthChanged: x = Math.max(0, _anchorRight - width)
    onHeightChanged: y = Math.max(0, _anchorBottom - height)

    onClosing: mainWindow.visible = false

    // Tray-flyout convention: dismiss on focus loss — but only once it has
    // actually been focused, so the ear-event popup (shown without focus) is
    // not instantly dismissed.
    property bool wasActive: false
    onActiveChanged: {
        if (active)
            wasActive = true
        else if (wasActive)
            dismissFlyout()
    }

    // ---- theme: follow system light/dark ----
    SystemPalette { id: sysPalette }
    readonly property bool darkMode: sysPalette.window.hslLightness < 0.5
    readonly property color surfaceColor:  darkMode ? "#252525" : "#f5f5f5"
    readonly property color cardColor:     darkMode ? "#303030" : "#ffffff"
    readonly property color borderColor:   darkMode ? "#3d3d3d" : "#d8d8d8"
    readonly property color textPrimary:   darkMode ? "#f2f2f2" : "#1a1a1a"
    readonly property color textSecondary: darkMode ? "#a8a8a8" : "#666666"
    readonly property color accentColor:   "#30D158"
    readonly property color warnColor:     darkMode ? "#FF9F0A" : "#C67800"
    // Card face depth — a whisper of vertical falloff so cards read as
    // surfaces, not painted areas. Stays within a few points of cardColor;
    // anything stronger starts looking glossy.
    readonly property Gradient cardGradient: Gradient {
        GradientStop { position: 0.0; color: mainWindow.darkMode ? "#363636" : "#ffffff" }
        GradientStop { position: 1.0; color: mainWindow.darkMode ? "#2b2b2b" : "#f3f3f3" }
    }

    // Drive every Qt Quick Control from the theme palette — the Basic style
    // derives all chrome (fields, popups, spin boxes, scrollbars) from these.
    palette.window: surfaceColor
    palette.windowText: textPrimary
    palette.base: cardColor
    palette.alternateBase: surfaceColor
    palette.text: textPrimary
    palette.button: cardColor
    palette.buttonText: textPrimary
    palette.highlight: accentColor
    palette.highlightedText: darkMode ? "#0a1a10" : "#ffffff"
    palette.placeholderText: textSecondary
    palette.mid: darkMode ? "#4a4a4a" : "#cfcfcf"
    palette.dark: darkMode ? "#1c1c1c" : "#9e9e9e"
    palette.light: darkMode ? "#565656" : "#ffffff"
    palette.link: accentColor
    palette.toolTipBase: cardColor
    palette.toolTipText: textPrimary

    function reopen(pageToLoad) {
        cancelClose()
        if (pageToLoad == "settings")
        {
            if (stackView.depth == 1)
            {
                stackView.push(settingsPage)
            }
        }
        else
        {
            if (stackView.depth > 1)
            {
                stackView.pop()
            }
        }

        if (!mainWindow.visible) {
            mainWindow.visible = true
        }
        entranceAnim.restart()
        raise()
        requestActivate()
    }

    // Ear-event popup — same entrance as reopen() but never steals focus.
    function popupShow() {
        cancelClose()
        if (!mainWindow.visible)
            mainWindow.visible = true
        entranceAnim.restart()
        raise()
    }

    // Exit animation — the flyout settles back toward the tray corner it
    // came from. Callers use dismissFlyout(); visible=false lands in
    // onFinished so a mid-flight re-show can't strand the card faded.
    property bool closing: false
    function dismissFlyout() {
        if (!mainWindow.visible || mainWindow.closing)
            return
        mainWindow.closing = true
        exitAnim.restart()
    }
    function cancelClose() {
        if (mainWindow.closing) {
            exitAnim.stop()
            mainWindow.closing = false
            card.opacity = 1.0
            card.scale = 1.0
            cardSlide.y = 0
        }
    }

    // Ear event while the flyout is already open and in use — pulse the card
    // edge instead of moving the window or stealing focus.
    function earEventPulse() {
        earPulseAnim.restart()
    }

    // Dev/ctl hook: scroll the settings page to a fraction of its range.
    function scrollSettings(pct) {
        if (stackView.currentItem && stackView.currentItem.scrollToFraction)
            stackView.currentItem.scrollToFraction(pct)
    }

    FontLoader {
        id: iconFont
        source: "qrc:/icons/assets/fonts/SF-Symbols-6.ttf"
    }

    // 1px light edge at the top of a card — drop inside any card Rectangle to
    // give flat surfaces a lit-from-above read.
    component CardShine: Rectangle {
        x: 1; y: 1
        width: parent.width - 2
        height: 1
        color: "#ffffff"
        // Light-mode shine sits on white cards — half strength or it reads
        // as a visible white seam instead of a lit edge.
        opacity: mainWindow.darkMode ? 0.055 : 0.30
        z: 10
    }

    // Ambient elevation ring — frameless windows get no OS shadow, so a soft
    // radial falloff behind the card lifts it off the desktop.
    Canvas {
        id: shadowCanvas
        anchors.fill: parent
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            ctx.save()
            ctx.translate(width / 2, height / 2)
            ctx.scale(1.0, height / width)
            const g = ctx.createRadialGradient(0, 0, 0, 0, 0, width / 2)
            g.addColorStop(0.84, "rgba(0,0,0,0)")
            g.addColorStop(0.95, "rgba(0,0,0,0.30)")
            g.addColorStop(1.0, "rgba(0,0,0,0)")
            ctx.fillStyle = g
            ctx.fillRect(-width, -width, width * 2, width * 2)
            ctx.restore()
        }
    }

    // Flyout entrance — rises out of the tray corner (BottomRight origin
    // plus a short upward drift), brief and never in the way.
    ParallelAnimation {
        id: entranceAnim
        NumberAnimation { target: card; property: "scale"; from: 0.94; to: 1.0; duration: airPodsTrayApp.reduceMotion ? 0 : 200; easing.type: Easing.OutCubic }
        NumberAnimation { target: card; property: "opacity"; from: 0.0; to: 1.0; duration: airPodsTrayApp.reduceMotion ? 0 : 150 }
        NumberAnimation { target: cardSlide; property: "y"; from: 14; to: 0; duration: airPodsTrayApp.reduceMotion ? 0 : 200; easing.type: Easing.OutCubic }
    }

    // Exit — mirror of the entrance: drift back down toward the tray,
    // slight shrink, fade. onFinished hides the window and restores the
    // card for the next show.
    ParallelAnimation {
        id: exitAnim
        NumberAnimation { target: card; property: "scale"; to: 0.94; duration: airPodsTrayApp.reduceMotion ? 0 : 150; easing.type: Easing.InCubic }
        NumberAnimation { target: card; property: "opacity"; to: 0.0; duration: airPodsTrayApp.reduceMotion ? 0 : 130; easing.type: Easing.InQuad }
        NumberAnimation { target: cardSlide; property: "y"; to: 16; duration: airPodsTrayApp.reduceMotion ? 0 : 150; easing.type: Easing.InCubic }
        onFinished: {
            mainWindow.visible = false
            mainWindow.closing = false
            card.opacity = 1.0
            card.scale = 1.0
            cardSlide.y = 0
        }
    }

    // Accent edge pulse — the "popup" when the flyout is already open.
    SequentialAnimation {
        id: earPulseAnim
        NumberAnimation { target: earPulseOverlay; property: "opacity"; from: 0.0; to: 1.0; duration: airPodsTrayApp.reduceMotion ? 0 : 120 }
        NumberAnimation { target: earPulseOverlay; property: "opacity"; to: 0.0; duration: airPodsTrayApp.reduceMotion ? 0 : 500; easing.type: Easing.OutCubic }
    }

    Rectangle {
        id: card
        anchors.fill: parent
        anchors.margins: 6
        radius: 10
        color: mainWindow.surfaceColor
        border.color: mainWindow.borderColor
        border.width: 1
        clip: true
        // Scale toward the tray corner so open/close reads as surfacing
        // from and settling back into the tray.
        transformOrigin: Item.BottomRight
        transform: Translate { id: cardSlide }
        gradient: Gradient {
            GradientStop { position: 0.0; color: mainWindow.darkMode ? "#2b2b2b" : "#fafafa" }
            GradientStop { position: 1.0; color: mainWindow.darkMode ? "#212121" : "#ececec" }
        }

        Rectangle {
            id: earPulseOverlay
            anchors.fill: parent
            radius: card.radius
            color: "transparent"
            border.color: mainWindow.accentColor
            border.width: 2
            opacity: 0
            z: 20
        }

        // Drag anywhere on empty space to move the flyout (declared before
        // the StackView so buttons/inputs still get first pick of events).
        MouseArea {
            anchors.fill: parent
            onPressed: mainWindow.startSystemMove()
        }

        StackView {
            id: stackView
            anchors.fill: parent
            initialItem: mainPage

            pushEnter: Transition {
                PropertyAnimation { property: "x"; from: 22; to: 0; duration: airPodsTrayApp.reduceMotion ? 0 : 180; easing.type: Easing.OutCubic }
                PropertyAnimation { property: "opacity"; from: 0.0; to: 1.0; duration: airPodsTrayApp.reduceMotion ? 0 : 180 }
            }
            pushExit: Transition {
                // Dim hard — the settings page keeps a translucent scrim, and
                // at 0.65 the main card's text ghosted through at the margins.
                PropertyAnimation { property: "opacity"; to: 0.22; duration: airPodsTrayApp.reduceMotion ? 0 : 180 }
            }
            popEnter: Transition {
                PropertyAnimation { property: "x"; from: -22; to: 0; duration: airPodsTrayApp.reduceMotion ? 0 : 180; easing.type: Easing.OutCubic }
                PropertyAnimation { property: "opacity"; from: 0.22; to: 1.0; duration: airPodsTrayApp.reduceMotion ? 0 : 180 }
            }
            popExit: Transition {
                PropertyAnimation { property: "x"; to: 22; duration: airPodsTrayApp.reduceMotion ? 0 : 180; easing.type: Easing.OutCubic }
                PropertyAnimation { property: "opacity"; to: 0.0; duration: airPodsTrayApp.reduceMotion ? 0 : 180 }
            }
        }
    }

    // Mouse area for handling back/forward navigation
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.BackButton | Qt.ForwardButton
        onClicked: (mouse) => {
            if (mouse.button === Qt.BackButton && stackView.depth > 1) {
                stackView.pop()
            } else if (mouse.button === Qt.ForwardButton) {
                console.log("Forward button pressed")
            }
        }
    }

    Component {
        id: mainPage
        Item {
            // Height the window needs for this page: the bottom edge of the
            // last visible element + 12px page margin + the card's 12px
            // window inset (6px margin each side). Without the inset term the
            // bottom border lands flush on the window edge — no space below
            // it, unlike the 12px the sides get.
            readonly property int desiredHeight: {
                if (controlsCard.visible)
                    return controlsCard.y + controlsCard.height + 24;
                if (guidanceCard.visible)
                    return guidanceCard.y + guidanceCard.height + 24;
                if (caption.visible)
                    return caption.y + caption.height + 24;
                if (batteryCard.visible)
                    return batteryCard.y + batteryCard.height + 24;
                return heroArea.y + heroArea.height + 24;
            }
            // ---------- custom title bar ----------
            Rectangle {
                id: titleBar
                anchors { top: parent.top; left: parent.left; right: parent.right }
                height: 38
                color: "transparent"

                MouseArea {
                    anchors.fill: parent
                    onPressed: mainWindow.startSystemMove()
                }

                Row {
                    anchors { left: parent.left; leftMargin: 12; verticalCenter: parent.verticalCenter }
                    spacing: 8

                    Text {
                        text: airPodsTrayApp.deviceInfo.deviceName !== "" ? airPodsTrayApp.deviceInfo.deviceName : "LibrePods"
                        color: mainWindow.textPrimary
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                        anchors.verticalCenter: parent.verticalCenter
                    }

                    // Connection status capsule — always an explicit word matching
                    // what Windows reports; dot breathes while the control
                    // channel is still opening on an up link.
                    Rectangle {
                        id: statusPill
                        anchors.verticalCenter: parent.verticalCenter
                        height: 19
                        width: statusRow.implicitWidth + 14
                        radius: 9.5
                        color: airPodsTrayApp.airpodsConnected
                               ? Qt.rgba(mainWindow.accentColor.r, mainWindow.accentColor.g, mainWindow.accentColor.b, 0.16)
                               : (airPodsTrayApp.statusHint === qsTr("Connecting…")
                                  ? Qt.rgba(mainWindow.warnColor.r, mainWindow.warnColor.g, mainWindow.warnColor.b, 0.18)
                                  : (mainWindow.darkMode ? "#3a3a3a" : "#e2e2e2"))
                        Behavior on color { ColorAnimation { duration: 220 } }

                        Row {
                            id: statusRow
                            anchors.centerIn: parent
                            spacing: 5
                            Rectangle {
                                id: statusDot
                                width: 6; height: 6; radius: 3
                                anchors.verticalCenter: parent.verticalCenter
                                color: airPodsTrayApp.airpodsConnected ? mainWindow.accentColor
                                       : (airPodsTrayApp.statusHint === qsTr("Connecting…") ? mainWindow.warnColor : "#8e8e93")
                                opacity: 1.0
                                SequentialAnimation {
                                    loops: Animation.Infinite
                                    running: airPodsTrayApp.statusHint === qsTr("Connecting…")
                                    alwaysRunToEnd: false
                                    onRunningChanged: if (!running) statusDot.opacity = 1.0
                                    NumberAnimation { target: statusDot; property: "opacity"; to: 0.3; duration: 750 }
                                    NumberAnimation { target: statusDot; property: "opacity"; to: 1.0; duration: 750 }
                                }
                            }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: airPodsTrayApp.deviceLinkConnected
                                      ? (airPodsTrayApp.deviceInfo.connectionState !== ""
                                         ? airPodsTrayApp.deviceInfo.connectionState
                                         : qsTr("Connected"))
                                      : (airPodsTrayApp.statusHint !== ""
                                         ? airPodsTrayApp.statusHint
                                         : qsTr("Disconnected"))
                                color: airPodsTrayApp.airpodsConnected ? mainWindow.accentColor
                                       : (airPodsTrayApp.statusHint === qsTr("Connecting…") ? mainWindow.warnColor : mainWindow.textSecondary)
                                font.pixelSize: 11
                                font.weight: Font.Medium
                                Behavior on color { ColorAnimation { duration: 220 } }
                            }
                        }
                    }
                }

                // settings + close buttons (right)
                Row {
                    anchors { right: parent.right; rightMargin: 8; verticalCenter: parent.verticalCenter }
                    spacing: 2

                    Rectangle {
                        width: 26; height: 26; radius: 6
                        color: gearArea.containsMouse ? (mainWindow.darkMode ? "#3d3d3d" : "#e4e4e4") : "transparent"
                        Text {
                            anchors.centerIn: parent
                            text: "\uf958"
                            font.family: iconFont.name
                            font.pixelSize: 14
                            color: mainWindow.textSecondary
                            rotation: gearArea.containsMouse ? 30 : 0
                            Behavior on rotation { NumberAnimation { duration: 200 } }
                        }
                        MouseArea {
                            id: gearArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: stackView.push(settingsPage)
                        }
                    }

                    Rectangle {
                        width: 26; height: 26; radius: 6
                        color: closeArea.containsMouse ? "#FF453A" : "transparent"
                        Text {
                            anchors.centerIn: parent
                            text: "✕"
                            font.pixelSize: 13
                            color: closeArea.containsMouse ? "white" : mainWindow.textSecondary
                        }
                        MouseArea {
                            id: closeArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: mainWindow.close()
                        }
                    }
                }
            }

            // ---------- hero: case art when docked, pod pair when worn ----------
            Item {
                id: heroArea
                anchors { top: titleBar.bottom; topMargin: 2; left: parent.left; right: parent.right }
                height: 96

                // Ambient accent glow behind the hero — lit when a pod is
                // actually in-ear, so the state reads at a glance.
                Canvas {
                    id: heroGlow
                    anchors.centerIn: parent
                    width: parent.width * 0.66
                    height: parent.height * 1.6
                    opacity: airPodsTrayApp.deviceLinkConnected
                             && (airPodsTrayApp.deviceInfo.leftPodInEar
                                 || airPodsTrayApp.deviceInfo.rightPodInEar
                                 || airPodsTrayApp.deviceInfo.isHeadset)
                             ? 1.0 : 0.0
                    Behavior on opacity { NumberAnimation { duration: 500 } }
                    onWidthChanged: requestPaint()
                    onHeightChanged: requestPaint()
                    onPaint: {
                        const ctx = getContext("2d")
                        ctx.reset()
                        ctx.save()
                        ctx.translate(width / 2, height / 2)
                        ctx.scale(1.0, height / width)
                        const acc = mainWindow.accentColor
                        const g = ctx.createRadialGradient(0, 0, 0, 0, 0, width / 2)
                        g.addColorStop(0.0,  "rgba(" + Math.round(acc.r * 255) + "," + Math.round(acc.g * 255) + "," + Math.round(acc.b * 255) + ",0.20)")
                        g.addColorStop(0.55, "rgba(" + Math.round(acc.r * 255) + "," + Math.round(acc.g * 255) + "," + Math.round(acc.b * 255) + ",0.07)")
                        g.addColorStop(1.0,  "rgba(0,0,0,0)")
                        ctx.fillStyle = g
                        ctx.fillRect(-width, -width, width * 2, width * 2)
                        ctx.restore()
                    }
                }

                // Pods docked — case artwork
                // Composite hero — pods draw whenever they're not docked in the
                // case, and the case art draws alongside them whenever it holds
                // a pod. Covers every real state: both worn (pods only), one
                // worn + one docked (pod + case), both docked (case only).
                Row {
                    anchors.centerIn: parent
                    spacing: 12
                    visible: !airPodsTrayApp.deviceInfo.isHeadset

                    Image {
                        id: leftPodImg
                        height: 88
                        fillMode: Image.PreserveAspectFit
                        mipmap: true
                        visible: !airPodsTrayApp.deviceInfo.leftPodInCase
                        source: "qrc:/icons/assets/" + airPodsTrayApp.deviceInfo.podIcon
                        opacity: !airPodsTrayApp.deviceLinkConnected ? 0.4
                                : (airPodsTrayApp.deviceInfo.leftPodInEar ? 1.0 : 0.45)
                        Behavior on opacity { NumberAnimation { duration: 200 } }
                        onVisibleChanged: if (visible) popLeft.restart()
                    }

                    Image {
                        id: rightPodImg
                        height: 88
                        fillMode: Image.PreserveAspectFit
                        mipmap: true
                        mirror: true
                        visible: !airPodsTrayApp.deviceInfo.rightPodInCase
                        source: "qrc:/icons/assets/" + airPodsTrayApp.deviceInfo.podIcon
                        opacity: !airPodsTrayApp.deviceLinkConnected ? 0.4
                                : (airPodsTrayApp.deviceInfo.rightPodInEar ? 1.0 : 0.45)
                        Behavior on opacity { NumberAnimation { duration: 200 } }
                        onVisibleChanged: if (visible) popRight.restart()
                    }

                    Image {
                        id: caseImg
                        height: 96
                        fillMode: Image.PreserveAspectFit
                        mipmap: true
                        visible: airPodsTrayApp.deviceInfo.supportsCaseBattery
                                 && (airPodsTrayApp.deviceInfo.podsInCase > 0
                                     || airPodsTrayApp.deviceInfo.leftPodInCase
                                     || airPodsTrayApp.deviceInfo.rightPodInCase)
                        source: visible ? "qrc:/icons/assets/" + airPodsTrayApp.deviceInfo.caseIcon : ""
                        opacity: airPodsTrayApp.deviceLinkConnected ? 1.0 : 0.4
                        Behavior on opacity { NumberAnimation { duration: 200 } }
                        onVisibleChanged: if (visible) popCase.restart()
                    }

                    // Gentle pop when artwork enters (pod pulled from case).
                    component PodPop: SequentialAnimation {
                        property alias popTarget: innerAnim.target
                        NumberAnimation { id: innerAnim; property: "scale"; from: 0.9; to: 1.0; duration: airPodsTrayApp.reduceMotion ? 0 : 220; easing.type: Easing.OutCubic }
                    }
                    PodPop { id: popLeft;  popTarget: leftPodImg }
                    PodPop { id: popRight; popTarget: rightPodImg }
                    PodPop { id: popCase;  popTarget: caseImg }
                }

                // AirPods Max — single headset artwork
                Image {
                    anchors.centerIn: parent
                    height: 96
                    fillMode: Image.PreserveAspectFit
                    mipmap: true
                    visible: airPodsTrayApp.deviceInfo.podsInCase === 0
                             && airPodsTrayApp.deviceInfo.isHeadset
                    source: "qrc:/icons/assets/" + airPodsTrayApp.deviceInfo.podIcon
                    opacity: airPodsTrayApp.deviceLinkConnected ? 1.0 : 0.4
                    Behavior on opacity { NumberAnimation { duration: 200 } }
                }
            }

            // ---------- battery chips card ----------
            Rectangle {
                id: batteryCard
                anchors { top: heroArea.bottom; topMargin: 6; left: parent.left; right: parent.right; margins: 12 }
                height: 46
                radius: 8
                color: mainWindow.cardColor
                gradient: mainWindow.cardGradient
                border.color: mainWindow.borderColor
                border.width: 1
                // Bind to the battery properties directly — chaining through
                // the chips' own bound `visible` doesn't re-evaluate reliably.
                visible: airPodsTrayApp.deviceInfo.battery.leftPodAvailable
                      || airPodsTrayApp.deviceInfo.battery.rightPodAvailable
                      || airPodsTrayApp.deviceInfo.battery.caseAvailable
                      || airPodsTrayApp.deviceInfo.battery.headsetAvailable

                CardShine {}
                opacity: airPodsTrayApp.deviceLinkConnected ? 1.0 : 0.55
                Behavior on opacity { NumberAnimation { duration: 200 } }

                Row {
                    anchors.centerIn: parent
                    spacing: 16

                    BatteryIndicator {
                        id: leftChip
                        visible: airPodsTrayApp.deviceInfo.battery.leftPodAvailable
                        inEar: airPodsTrayApp.deviceInfo.leftPodInEar
                        batteryLevel: airPodsTrayApp.deviceInfo.battery.leftPodLevel
                        isCharging: airPodsTrayApp.deviceInfo.battery.leftPodCharging
                        indicator: "L"
                        secondaryColor: mainWindow.textSecondary
                        primaryColor: mainWindow.textPrimary
                        trackColor: mainWindow.darkMode ? "#3f3f3f" : "#e4e4e4"
                    }

                    BatteryIndicator {
                        id: rightChip
                        visible: airPodsTrayApp.deviceInfo.battery.rightPodAvailable
                        inEar: airPodsTrayApp.deviceInfo.rightPodInEar
                        batteryLevel: airPodsTrayApp.deviceInfo.battery.rightPodLevel
                        isCharging: airPodsTrayApp.deviceInfo.battery.rightPodCharging
                        indicator: "R"
                        secondaryColor: mainWindow.textSecondary
                        primaryColor: mainWindow.textPrimary
                        trackColor: mainWindow.darkMode ? "#3f3f3f" : "#e4e4e4"
                    }

                    BatteryIndicator {
                        id: caseChip
                        visible: airPodsTrayApp.deviceInfo.battery.caseAvailable
                        batteryLevel: airPodsTrayApp.deviceInfo.battery.caseLevel
                        isCharging: airPodsTrayApp.deviceInfo.battery.caseCharging
                        indicator: "C"
                        secondaryColor: mainWindow.textSecondary
                        primaryColor: mainWindow.textPrimary
                        trackColor: mainWindow.darkMode ? "#3f3f3f" : "#e4e4e4"
                    }

                    BatteryIndicator {
                        id: headsetChip
                        visible: airPodsTrayApp.deviceInfo.battery.headsetAvailable
                        batteryLevel: airPodsTrayApp.deviceInfo.battery.headsetLevel
                        isCharging: airPodsTrayApp.deviceInfo.battery.headsetCharging
                        indicator: "H"
                        secondaryColor: mainWindow.textSecondary
                        primaryColor: mainWindow.textPrimary
                        trackColor: mainWindow.darkMode ? "#3f3f3f" : "#e4e4e4"
                    }
                }
            }

            // ---------- merged status caption ----------
            Text {
                id: caption
                anchors { top: batteryCard.visible ? batteryCard.bottom : heroArea.bottom; topMargin: 8; horizontalCenter: parent.horizontalCenter }
                width: parent.width - 24
                horizontalAlignment: Text.AlignHCenter
                color: mainWindow.textSecondary
                font.pixelSize: 12
                elide: Text.ElideRight
                maximumLineCount: 1
                visible: text !== ""
                text: {
                    var parts = []
                    if (airPodsTrayApp.deviceInfo.podsInCase > 0)
                        parts.push(airPodsTrayApp.deviceInfo.podsInCase >= 2
                                   ? qsTr("Both pods in case") : qsTr("Pod in case"))
                    // Guidance card already carries the state + action when
                    // disconnected — don't repeat the bare word in the caption.
                    if (!airPodsTrayApp.airpodsConnected && airPodsTrayApp.statusHint !== ""
                            && !guidanceCard.visible)
                        parts.push(airPodsTrayApp.statusHint)
                    if (airPodsTrayApp.deviceInfo.podsInCase > 0 && !airPodsTrayApp.deviceInfo.caseLidOpen)
                        parts.push(qsTr("lid closed"))
                    return parts.join(" · ")
                }
            }

            // ---------- offline / first-run guidance ----------
            // Bare status words left users guessing what to do next; this
            // card names the next step and gives a real action for the two
            // states that have one (pairing, manual connect).
            Rectangle {
                id: guidanceCard
                anchors { top: caption.bottom; topMargin: 8; left: parent.left; right: parent.right; margins: 12 }
                height: guideCol.height + 20
                radius: 8
                color: mainWindow.cardColor
                gradient: mainWindow.cardGradient
                border.color: mainWindow.borderColor
                border.width: 1
                visible: !airPodsTrayApp.airpodsConnected

                CardShine {}
                Column {
                    id: guideCol
                    anchors { left: parent.left; right: parent.right; top: parent.top; margins: 10 }
                    spacing: 8

                    Text {
                        width: parent.width
                        wrapMode: Text.WordWrap
                        color: mainWindow.textSecondary
                        font.pixelSize: 11
                        text: !airPodsTrayApp.driverAvailable
                              ? qsTr("Limited mode — the kernel driver isn't loaded, so controls are unavailable.")
                              : airPodsTrayApp.deviceInfo.bluetoothAddress === ""
                                ? qsTr("Put your AirPods in the case, hold the setup button until the light flashes, then pair them in Windows Settings → Bluetooth.")
                                : airPodsTrayApp.statusHint === qsTr("Connecting…")
                                  ? qsTr("Opening the control channel — this usually takes a few seconds.")
                                  : qsTr("Your AirPods are paired but not connected. Take them out of the case, or connect manually below.")
                    }
                    StyledButton {
                        anchors.horizontalCenter: parent.horizontalCenter
                        visible: airPodsTrayApp.driverAvailable
                                 && airPodsTrayApp.deviceInfo.bluetoothAddress === ""
                        text: qsTr("Open Bluetooth settings")
                        onClicked: Qt.openUrlExternally("ms-settings:bluetooth")
                    }
                    StyledButton {
                        anchors.horizontalCenter: parent.horizontalCenter
                        visible: airPodsTrayApp.driverAvailable
                                 && airPodsTrayApp.deviceInfo.bluetoothAddress !== ""
                                 && airPodsTrayApp.statusHint === qsTr("Not connected")
                        text: qsTr("Connect now")
                        onClicked: airPodsTrayApp.connectToDevice(airPodsTrayApp.deviceInfo.bluetoothAddress)
                    }
                }
            }

            // ---------- controls (connected mode) ----------
            Rectangle {
                id: controlsCard
                anchors { top: caption.bottom; topMargin: 8; left: parent.left; right: parent.right; margins: 12 }
                // +12 top margin / +18 bottom — the last switch row reads
                // cramped against the border with symmetric padding.
                height: controlsCol.height + 30
                radius: 8
                color: mainWindow.cardColor
                gradient: mainWindow.cardGradient
                border.color: mainWindow.borderColor
                border.width: 1
                visible: airPodsTrayApp.airpodsConnected

                CardShine {}
                Column {
                    id: controlsCol
                    anchors { left: parent.left; right: parent.right; top: parent.top; margins: 12 }
                    spacing: 10

                    SegmentedControl {
                        anchors.horizontalCenter: parent.horizontalCenter
                        visible: airPodsTrayApp.deviceInfo.supportsANC
                        model: airPodsTrayApp.deviceInfo.supportsAdaptive
                               ? [qsTr("Off"), qsTr("ANC"), qsTr("Transparency"), qsTr("Adaptive")]
                               : [qsTr("Off"), qsTr("ANC"), qsTr("Transparency")]
                        currentIndex: airPodsTrayApp.deviceInfo.noiseControlMode
                        onActivated: (idx) => airPodsTrayApp.setNoiseControlModeInt(idx)
                    }

                    StyledSlider {
                        visible: airPodsTrayApp.deviceInfo.adaptiveModeActive
                        width: parent.width
                        from: 0; to: 100; stepSize: 1
                        value: airPodsTrayApp.deviceInfo.adaptiveNoiseLevel

                        Timer {
                            id: debounceTimer
                            interval: 500
                            onTriggered: if (!parent.pressed) airPodsTrayApp.setAdaptiveNoiseLevel(parent.value)
                        }
                        onPressedChanged: if (!pressed) airPodsTrayApp.setAdaptiveNoiseLevel(value)
                        onValueChanged: if (pressed) debounceTimer.restart()
                    }

                    RowLayout {
                        width: parent.width
                        // Row height pinned to the switch's *rendered* height —
                        // scale shrinks the visual but not implicitHeight, so an
                        // unpinned row allocates phantom space below the last one.
                        implicitHeight: 26
                        visible: airPodsTrayApp.deviceInfo.supportsConversationalAwareness
                        Text {
                            text: qsTr("Conversational Awareness")
                            color: mainWindow.textPrimary; font.pixelSize: 12
                            Layout.fillWidth: true
                        }
                        StyledSwitch {
                            checked: airPodsTrayApp.deviceInfo.conversationalAwareness
                            onToggled: airPodsTrayApp.setConversationalAwareness(checked)
                            scale: 0.85
                        }
                    }

                    RowLayout {
                        width: parent.width
                        implicitHeight: 26
                        visible: airPodsTrayApp.deviceInfo.supportsHearingAid
                        Text {
                            text: qsTr("Hearing Aid")
                            color: mainWindow.textPrimary; font.pixelSize: 12
                            Layout.fillWidth: true
                        }
                        StyledSwitch {
                            checked: airPodsTrayApp.deviceInfo.hearingAidEnabled
                            onToggled: airPodsTrayApp.setHearingAidEnabled(checked)
                            scale: 0.85
                        }
                    }
                }
            }
        }
    }

    Component {
        id: settingsPage
        Page {
            id: settingsPageItem
            // Mostly-opaque scrim over the dimmed main page — enough surface
            // color that the underlying card can't ghost through at the
            // margins, translucent enough to keep the stacked-depth read.
            background: Rectangle {
                color: mainWindow.darkMode ? Qt.rgba(0.145, 0.145, 0.145, 0.95)
                                           : Qt.rgba(0.96, 0.96, 0.96, 0.95)
            }
            // Natural height of the settings column + header so the window
            // can size itself to the content with no empty tail.
            readonly property int desiredHeight: settingsColumn.implicitHeight + 38 + settingsColumn.padding * 2

            // Dev/ctl hook: jump to a fraction of the scroll range.
            function scrollToFraction(f) {
                settingsScroll.contentItem.contentY = Math.max(0, f * (settingsScroll.contentItem.contentHeight - settingsScroll.contentItem.height))
            }

            // Loopback spectrum capture runs only while this page is open.
            Component.onCompleted: airPodsTrayApp.spectrum.enabled = true
            onVisibleChanged: airPodsTrayApp.spectrum.enabled = visible
            Component.onDestruction: airPodsTrayApp.spectrum.enabled = false

            // Drag to move the flyout — declared first so the ScrollView,
            // header buttons and all controls get their events first; only
            // dead space, card faces and labels reach this.
            MouseArea {
                anchors.fill: parent
                onPressed: mainWindow.startSystemMove()
            }

            // custom header: back + title
            Rectangle {
                anchors { top: parent.top; left: parent.left; right: parent.right }
                height: 38
                color: "transparent"
                z: 10

                Rectangle {
                    width: 26; height: 26; radius: 6
                    anchors { left: parent.left; leftMargin: 8; verticalCenter: parent.verticalCenter }
                    color: backArea.containsMouse ? (mainWindow.darkMode ? "#3d3d3d" : "#e4e4e4") : "transparent"
                    Text {
                        anchors.centerIn: parent
                        text: "\uecb1"
                        font.family: iconFont.name
                        font.pixelSize: 15
                        color: mainWindow.textSecondary
                    }
                    MouseArea {
                        id: backArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: stackView.pop()
                    }
                }

                Text {
                    anchors.centerIn: parent
                    text: qsTr("Settings")
                    color: mainWindow.textPrimary
                    font.pixelSize: 14
                    font.weight: Font.DemiBold
                }
            }

            ScrollView {
                id: settingsScroll
                anchors.fill: parent
                anchors.topMargin: 38
                clip: true

                    Column {
                    id: settingsColumn
                    width: settingsPageItem.width
                    spacing: 14
                    padding: 12

                    // Shared option list for the gesture/stem action pickers —
                    // indices mirror the C++ action ids (0 none, 1 play/pause,
                    // 2 next, 3 previous, 4 vol up, 5 vol down, 6 cycle ANC).
                    property var actionNames: [
                        qsTr("None"), qsTr("Play / pause"), qsTr("Next track"),
                        qsTr("Previous track"), qsTr("Volume up"), qsTr("Volume down"),
                        qsTr("Cycle noise mode")
                    ]
                    // Stem presses: index 0 doesn't mean "do nothing" — it
                    // means we don't intercept and the pods' own configured
                    // press action (play/pause, Siri, ANC…) keeps running.
                    property var stemActionNames: [
                        qsTr("Built-in"), qsTr("Play / pause"), qsTr("Next track"),
                        qsTr("Previous track"), qsTr("Volume up"), qsTr("Volume down"),
                        qsTr("Cycle noise mode")
                    ]

                    // Section header: accent tick + tracked caps label.
                    component SectionHeader: Row {
                        property alias label: hdrText.text
                        spacing: 7
                        Rectangle {
                            width: 3; height: 11; radius: 1.5
                            anchors.verticalCenter: parent.verticalCenter
                            color: mainWindow.accentColor
                        }
                        Text {
                            id: hdrText
                            color: mainWindow.textSecondary
                            font.pixelSize: 11
                            font.weight: Font.DemiBold
                            font.letterSpacing: 1.0
                        }
                    }

                    // In-card cluster separator — the 10px spacing around it
                    // widens the gap between option groups; the hairline just
                    // makes the grouping legible.
                    component RowDivider: Rectangle {
                        width: parent.width
                        height: 1
                        color: mainWindow.borderColor
                        opacity: 0.55
                    }

                    // section helper texts
                    SectionHeader { label: qsTr("PLAYBACK") }

                    Rectangle {
                        width: parent.width - parent.padding * 2
                        height: playCol.height + 16
                        radius: 8; color: mainWindow.cardColor; gradient: mainWindow.cardGradient; border.color: mainWindow.borderColor; border.width: 1
                        CardShine {}
                        Column {
                            id: playCol
                            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 8 }
                            spacing: 10

                            RowLayout {
                                width: parent.width
                                Text { text: qsTr("Pause when removed"); color: mainWindow.textPrimary; font.pixelSize: 12; Layout.fillWidth: true }
                                StyledComboBox {
                                    model: [qsTr("One removed"), qsTr("Both removed"), qsTr("Never")]
                                    currentIndex: airPodsTrayApp.earDetectionBehavior
                                    onActivated: airPodsTrayApp.earDetectionBehavior = currentIndex
                                }
                            }

                            RowLayout {
                                width: parent.width
                                visible: airPodsTrayApp.airpodsConnected
                                Column {
                                    Layout.fillWidth: true
                                    Text { text: qsTr("Automatic ear detection"); color: mainWindow.textPrimary; font.pixelSize: 12 }
                                    Text { text: qsTr("Pod-side in-ear sensing"); color: mainWindow.textSecondary; font.pixelSize: 10 }
                                }
                                StyledSwitch {
                                    checked: airPodsTrayApp.deviceInfo.podEarDetection
                                    onToggled: airPodsTrayApp.setPodEarDetection(checked)
                                }
                            }

                            RowLayout {
                                width: parent.width
                                visible: airPodsTrayApp.airpodsConnected && airPodsTrayApp.deviceInfo.supportsSleepDetection
                                Column {
                                    Layout.fillWidth: true
                                    Text { text: qsTr("Sleep detection"); color: mainWindow.textPrimary; font.pixelSize: 12 }
                                    Text { text: qsTr("Pause media when you fall asleep"); color: mainWindow.textSecondary; font.pixelSize: 10 }
                                }
                                StyledSwitch {
                                    checked: airPodsTrayApp.deviceInfo.sleepDetection
                                    onToggled: airPodsTrayApp.setSleepDetection(checked)
                                }
                            }

                            RowLayout {
                                width: parent.width
                                visible: airPodsTrayApp.airpodsConnected && airPodsTrayApp.deviceInfo.supportsConversationalAwareness
                                Text { text: qsTr("Conversational Awareness"); color: mainWindow.textPrimary; font.pixelSize: 12; Layout.fillWidth: true }
                                StyledSwitch {
                                    checked: airPodsTrayApp.deviceInfo.conversationalAwareness
                                    onToggled: airPodsTrayApp.setConversationalAwareness(checked)
                                }
                            }

                            RowLayout {
                                width: parent.width
                                visible: airPodsTrayApp.airpodsConnected && airPodsTrayApp.deviceInfo.supportsConversationalAwareness
                                Column {
                                    Layout.fillWidth: true
                                    Text { text: qsTr("Adaptive volume"); color: mainWindow.textPrimary; font.pixelSize: 12 }
                                    Text { text: qsTr("Pods adjust volume to the environment"); color: mainWindow.textSecondary; font.pixelSize: 10 }
                                }
                                StyledSwitch {
                                    checked: airPodsTrayApp.deviceInfo.adaptiveVolume
                                    onToggled: airPodsTrayApp.setAdaptiveVolume(checked)
                                }
                            }

                            RowDivider { visible: airPodsTrayApp.airpodsConnected && airPodsTrayApp.deviceInfo.supportsHeadTracking }

                            RowLayout {
                                width: parent.width
                                visible: airPodsTrayApp.airpodsConnected && airPodsTrayApp.deviceInfo.supportsHeadTracking
                                Text { text: qsTr("Head tracking"); color: mainWindow.textPrimary; font.pixelSize: 12; Layout.fillWidth: true }
                                StyledSwitch {
                                    checked: airPodsTrayApp.deviceInfo.headTrackingEnabled
                                    onToggled: airPodsTrayApp.setHeadTrackingEnabled(checked)
                                }
                            }

                            // Live head-orientation readout — the glyph turns
                            // with yaw and tilts with pitch. The pods stream
                            // fused orientation whenever at least one is worn
                            // (iOS tracks single-bud too), so the model shows
                            // whenever tracking is on; only a fully-cased or
                            // pocketed pair gets the explainer.
                            ColumnLayout {
                                width: parent.width
                                spacing: 4
                                visible: airPodsTrayApp.airpodsConnected && airPodsTrayApp.deviceInfo.headTrackingEnabled && airPodsTrayApp.deviceInfo.supportsHeadTracking

                                readonly property bool anyPodInEar: airPodsTrayApp.deviceInfo.leftPodInEar
                                                                     || airPodsTrayApp.deviceInfo.rightPodInEar
                                HeadModel {
                                    id: headModel
                                    visible: parent.anyPodInEar
                                    Layout.alignment: Qt.AlignHCenter
                                    Layout.preferredWidth: 130
                                    Layout.preferredHeight: 130
                                    yaw: airPodsTrayApp.deviceInfo.headYaw
                                    pitch: airPodsTrayApp.deviceInfo.headPitch
                                    gain: 1.4
                                    lineColor: mainWindow.accentColor
                                    dimColor: mainWindow.darkMode ? "#636366" : "#c4c4cc"
                                    fillTop: mainWindow.darkMode ? "#4a4a54" : "#dcdce4"
                                    fillBottom: mainWindow.darkMode ? "#2c2c33" : "#b8b8c4"
                                    earColor: mainWindow.darkMode ? "#4a4a54" : "#dcdce4"
                                }
                                Text {
                                    visible: parent.anyPodInEar
                                    Layout.alignment: Qt.AlignHCenter
                                    text: "yaw " + Math.round(airPodsTrayApp.deviceInfo.headYaw) + "° · pitch " + Math.round(airPodsTrayApp.deviceInfo.headPitch) + "°"
                                    color: mainWindow.textSecondary
                                    font.pixelSize: 11
                                }
                                Text {
                                    visible: !parent.anyPodInEar
                                    Layout.fillWidth: true
                                    Layout.topMargin: 2
                                    Layout.bottomMargin: 4
                                    text: qsTr("Wear an AirPod to see live head tracking")
                                    color: mainWindow.textSecondary
                                    font.pixelSize: 11
                                    horizontalAlignment: Text.AlignHCenter
                                    wrapMode: Text.Wrap
                                }
                            }

                            // Head-gesture action maps — always visible so the
                            // feature is discoverable, but disabled+dimmed until
                            // the tracking stream is actually running.
                            Repeater {
                                model: [
                                    { label: qsTr("Nod (accept)"), prop: "gestureNodAction" },
                                    { label: qsTr("Shake (reject)"), prop: "gestureShakeAction" }
                                ]
                                delegate: RowLayout {
                                    required property var modelData
                                    width: playCol.width
                                    visible: airPodsTrayApp.airpodsConnected && airPodsTrayApp.deviceInfo.supportsHeadTracking
                                    enabled: airPodsTrayApp.deviceInfo.headTrackingEnabled
                                             && (airPodsTrayApp.deviceInfo.leftPodInEar
                                                 || airPodsTrayApp.deviceInfo.rightPodInEar)
                                    opacity: enabled ? 1.0 : 0.45
                                    Text { text: modelData.label; color: mainWindow.textPrimary; font.pixelSize: 12; Layout.fillWidth: true }
                                    StyledComboBox {
                                        model: settingsColumn.actionNames
                                        currentIndex: airPodsTrayApp[modelData.prop]
                                        onActivated: airPodsTrayApp[modelData.prop] = currentIndex
                                    }
                                }
                            }

                            Text {
                                id: lastGestureText
                                visible: airPodsTrayApp.lastGesture !== ""
                                text: qsTr("Last gesture: %1").arg(airPodsTrayApp.lastGesture)
                                color: mainWindow.textSecondary; font.pixelSize: 11
                            }

                            // A fired gesture visibly lands: the dial flares
                            // and this label flashes accent, then settles.
                            Connections {
                                target: airPodsTrayApp
                                function onLastGestureChanged() {
                                    headModel.gestureFlash()
                                    if (!airPodsTrayApp.reduceMotion) gestureTint.restart()
                                }
                            }
                            SequentialAnimation {
                                id: gestureTint
                                ColorAnimation { target: lastGestureText; property: "color"; to: mainWindow.accentColor; duration: 120 }
                                ColorAnimation { target: lastGestureText; property: "color"; to: mainWindow.textSecondary; duration: 700; easing.type: Easing.OutCubic }
                            }

                            RowDivider { visible: airPodsTrayApp.airpodsConnected }

                            RowLayout {
                                width: parent.width
                                visible: airPodsTrayApp.airpodsConnected && airPodsTrayApp.deviceInfo.supportsANC && !airPodsTrayApp.deviceInfo.isHeadset
                                Text { text: qsTr("One Bud ANC Mode"); color: mainWindow.textPrimary; font.pixelSize: 12; Layout.fillWidth: true }
                                StyledSwitch {
                                    checked: airPodsTrayApp.deviceInfo.oneBudANCMode
                                    onToggled: airPodsTrayApp.deviceInfo.oneBudANCMode = checked
                                }
                            }

                            RowLayout {
                                width: parent.width
                                visible: airPodsTrayApp.airpodsConnected
                                Text { text: qsTr("Microphone"); color: mainWindow.textPrimary; font.pixelSize: 12; Layout.fillWidth: true }
                                StyledComboBox {
                                    model: [qsTr("Automatic"), qsTr("Always right"), qsTr("Always left")]
                                    currentIndex: airPodsTrayApp.deviceInfo.micMode
                                    onActivated: airPodsTrayApp.setMicMode(currentIndex)
                                }
                            }

                            RowLayout {
                                width: parent.width
                                visible: airPodsTrayApp.airpodsConnected && airPodsTrayApp.deviceInfo.isHeadset
                                Text { text: qsTr("Crown direction"); color: mainWindow.textPrimary; font.pixelSize: 12; Layout.fillWidth: true }
                                StyledComboBox {
                                    model: [qsTr("Back to front"), qsTr("Front to back")]
                                    currentIndex: airPodsTrayApp.deviceInfo.crownDirection
                                    onActivated: airPodsTrayApp.setCrownDirection(currentIndex)
                                }
                            }

                            RowLayout {
                                width: parent.width
                                visible: airPodsTrayApp.airpodsConnected && airPodsTrayApp.deviceInfo.supportsVolumeSwipe
                                Text { text: qsTr("Volume swipe"); color: mainWindow.textPrimary; font.pixelSize: 12; Layout.fillWidth: true }
                                StyledSwitch {
                                    checked: airPodsTrayApp.deviceInfo.volumeSwipeEnabled
                                    onToggled: airPodsTrayApp.setVolumeSwipe(checked)
                                }
                            }

                            RowLayout {
                                width: parent.width
                                visible: airPodsTrayApp.airpodsConnected && airPodsTrayApp.deviceInfo.supportsVolumeSwipe
                                enabled: airPodsTrayApp.deviceInfo.volumeSwipeEnabled
                                opacity: enabled ? 1.0 : 0.45
                                Text { text: qsTr("Swipe speed"); color: mainWindow.textPrimary; font.pixelSize: 12; Layout.fillWidth: true }
                                StyledComboBox {
                                    // Pods report 0..3; 0 and 1 both mean default.
                                    model: [qsTr("Default"), qsTr("Longer"), qsTr("Longest")]
                                    currentIndex: Math.max(0, Math.min(2, airPodsTrayApp.deviceInfo.volumeSwipeInterval - 1))
                                    onActivated: airPodsTrayApp.setVolumeSwipeInterval(currentIndex + 1)
                                }
                            }

                            RowDivider {
                                visible: airPodsTrayApp.airpodsConnected
                                         && (airPodsTrayApp.deviceInfo.supportsANC
                                             || airPodsTrayApp.deviceInfo.supportsLoudSoundReduction)
                            }

                            // Which modes stem long-press cycles through —
                            // the 0x1A bitmask, one chip per mode.
                            Column {
                                width: parent.width
                                spacing: 6
                                visible: airPodsTrayApp.airpodsConnected && airPodsTrayApp.deviceInfo.supportsANC
                                Text { text: qsTr("Long-press cycles through"); color: mainWindow.textPrimary; font.pixelSize: 12 }
                                Flow {
                                    width: parent.width
                                    spacing: 6
                                    Repeater {
                                        model: [
                                            { label: qsTr("Off"), bit: 0x01 },
                                            { label: qsTr("ANC"), bit: 0x02 },
                                            { label: qsTr("Transparency"), bit: 0x04 },
                                            { label: qsTr("Adaptive"), bit: 0x08, cap: true }
                                        ]
                                        delegate: Rectangle {
                                            required property var modelData
                                            visible: !(modelData.cap === true && !airPodsTrayApp.deviceInfo.supportsAdaptive)
                                                     && !(modelData.bit === 0x01 && !airPodsTrayApp.deviceInfo.allowOffOption)
                                            readonly property bool active: (airPodsTrayApp.deviceInfo.listeningModeCycle & modelData.bit) !== 0
                                            implicitHeight: 26
                                            implicitWidth: chipText.implicitWidth + 20
                                            radius: 13
                                            color: active ? mainWindow.accentColor
                                                   : (chipMouse.containsMouse
                                                      ? Qt.lighter(mainWindow.cardColor, 1.08)
                                                      : mainWindow.surfaceColor)
                                            border.color: active ? mainWindow.accentColor
                                                          : (chipMouse.containsMouse
                                                             ? Qt.lighter(mainWindow.borderColor, 1.25)
                                                             : mainWindow.borderColor)
                                            Behavior on color { ColorAnimation { duration: 110 } }
                                            Behavior on border.color { ColorAnimation { duration: 110 } }
                                            Text {
                                                id: chipText
                                                anchors.centerIn: parent
                                                text: modelData.label
                                                color: active ? "#0a1a10" : mainWindow.textPrimary
                                                font.pixelSize: 11
                                            }
                                            MouseArea {
                                                id: chipMouse
                                                anchors.fill: parent
                                                hoverEnabled: true
                                                cursorShape: Qt.PointingHandCursor
                                                onClicked: airPodsTrayApp.setListeningModeCycle(
                                                               airPodsTrayApp.deviceInfo.listeningModeCycle ^ modelData.bit)
                                            }
                                        }
                                    }
                                }
                            }

                            // 0x34 — whether "Off" is offered in the
                            // noise-control cycle at all (Pro 2).
                            RowLayout {
                                width: parent.width
                                visible: airPodsTrayApp.airpodsConnected && airPodsTrayApp.deviceInfo.supportsLoudSoundReduction
                                Column {
                                    Layout.fillWidth: true
                                    Text { text: qsTr("Allow Off mode"); color: mainWindow.textPrimary; font.pixelSize: 12 }
                                    Text { text: qsTr("Off can be selected in the noise-mode cycle"); color: mainWindow.textSecondary; font.pixelSize: 10 }
                                }
                                StyledSwitch {
                                    checked: airPodsTrayApp.deviceInfo.allowOffOption
                                    onToggled: airPodsTrayApp.setAllowOffOption(checked)
                                }
                            }
                        }
                    }

                    SectionHeader { label: qsTr("STEM PRESS"); visible: airPodsTrayApp.airpodsConnected && airPodsTrayApp.deviceInfo.supportsStemPress }

                    Rectangle {
                        width: parent.width - parent.padding * 2
                        height: stemCol.height + 16
                        visible: airPodsTrayApp.airpodsConnected && airPodsTrayApp.deviceInfo.supportsStemPress
                        radius: 8; color: mainWindow.cardColor; gradient: mainWindow.cardGradient; border.color: mainWindow.borderColor; border.width: 1
                        CardShine {}
                        Column {
                            id: stemCol
                            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 8 }
                            spacing: 10

                            Repeater {
                                model: [
                                    { label: qsTr("Single press"), prop: "stemSingleAction" },
                                    { label: qsTr("Double press"), prop: "stemDoubleAction" },
                                    { label: qsTr("Triple press"), prop: "stemTripleAction" },
                                    { label: qsTr("Long press"), prop: "stemLongAction" }
                                ]
                                delegate: RowLayout {
                                    required property var modelData
                                    width: stemCol.width
                                    Text { text: modelData.label; color: mainWindow.textPrimary; font.pixelSize: 12; Layout.fillWidth: true }
                                    StyledComboBox {
                                        model: settingsColumn.stemActionNames
                                        currentIndex: airPodsTrayApp[modelData.prop]
                                        onActivated: airPodsTrayApp[modelData.prop] = currentIndex
                                    }
                                }
                            }

                            Text {
                                text: qsTr("\"Built-in\" = the press keeps doing whatever the pods were configured for (e.g. play/pause or Siri). Pick an action to override it from this app.")
                                color: mainWindow.textSecondary; font.pixelSize: 10
                                wrapMode: Text.WordWrap; width: parent.width
                            }

                            RowLayout {
                                width: parent.width
                                Text { text: qsTr("Press speed"); color: mainWindow.textPrimary; font.pixelSize: 12; Layout.fillWidth: true }
                                StyledComboBox {
                                    model: [qsTr("Default"), qsTr("Slower"), qsTr("Slowest")]
                                    currentIndex: airPodsTrayApp.deviceInfo.pressSpeed
                                    onActivated: airPodsTrayApp.setPressSpeed(currentIndex)
                                }
                            }

                            RowLayout {
                                width: parent.width
                                Text { text: qsTr("Hold duration"); color: mainWindow.textPrimary; font.pixelSize: 12; Layout.fillWidth: true }
                                StyledComboBox {
                                    model: [qsTr("Default"), qsTr("Longer"), qsTr("Longest")]
                                    currentIndex: airPodsTrayApp.deviceInfo.holdDuration
                                    onActivated: airPodsTrayApp.setHoldDuration(currentIndex)
                                }
                            }

                            RowLayout {
                                width: parent.width
                                Text { text: qsTr("Mute on call"); color: mainWindow.textPrimary; font.pixelSize: 12; Layout.fillWidth: true }
                                StyledComboBox {
                                    model: [qsTr("Double press"), qsTr("Single press")]
                                    currentIndex: airPodsTrayApp.deviceInfo.muteOnSinglePress ? 1 : 0
                                    onActivated: airPodsTrayApp.setMuteOnSinglePress(currentIndex === 1)
                                }
                            }
                        }
                    }

                    SectionHeader { label: qsTr("CUSTOM EQ"); visible: airPodsTrayApp.airpodsConnected }

                    Rectangle {
                        width: parent.width - parent.padding * 2
                        height: eqCol.height + 16
                        visible: airPodsTrayApp.airpodsConnected
                        radius: 8; color: mainWindow.cardColor; gradient: mainWindow.cardGradient
                        border.color: airPodsTrayApp.deviceInfo.customEqEnabled
                                      ? Qt.rgba(mainWindow.accentColor.r, mainWindow.accentColor.g, mainWindow.accentColor.b, 0.5)
                                      : mainWindow.borderColor
                        border.width: 1
                        Behavior on border.color { ColorAnimation { duration: 250 } }
                        CardShine {}
                        Column {
                            id: eqCol
                            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 8 }
                            spacing: 10

                            RowLayout {
                                width: parent.width
                                Text { text: qsTr("Enable custom EQ"); color: mainWindow.textPrimary; font.pixelSize: 12; Layout.fillWidth: true }
                                StyledSwitch {
                                    checked: airPodsTrayApp.deviceInfo.customEqEnabled
                                    onToggled: airPodsTrayApp.setCustomEqEnabled(checked)
                                }
                            }

                            // Live output spectrum — capture runs only while
                            // the settings page is open (card on screen), so
                            // the loopback+FFT cost is never paid otherwise.
                            Item {
                                id: spectrumStrip
                                width: eqCol.width
                                height: 56
                                Row {
                                    id: specBars
                                    anchors.fill: parent
                                    spacing: 2
                                    // Idle fade — bars dim when nothing plays,
                                    // breathing slowly so the strip reads as
                                    // "listening" rather than dead.
                                    readonly property bool hasAudio: airPodsTrayApp.spectrum.levels.some(v => v > 0.02)
                                    property real idleBreath: 0.45
                                    opacity: hasAudio ? 1.0 : idleBreath
                                    SequentialAnimation {
                                        running: !specBars.hasAudio && specBars.visible && !airPodsTrayApp.reduceMotion
                                        loops: Animation.Infinite
                                        alwaysRunToEnd: false
                                        onRunningChanged: if (!running) specBars.idleBreath = 0.45
                                        NumberAnimation { target: specBars; property: "idleBreath"; to: 0.62; duration: 1500; easing.type: Easing.InOutSine }
                                        NumberAnimation { target: specBars; property: "idleBreath"; to: 0.36; duration: 1500; easing.type: Easing.InOutSine }
                                    }
                                    Repeater {
                                        // Fixed 24-slot model — binding levels[i] directly
                                        // keeps delegates stable across publishes (a list
                                        // model would rebuild every delegate each frame
                                        // and reset per-bar peak state).
                                        model: 24
                                        delegate: Item {
                                            required property int index
                                            readonly property real level: index < airPodsTrayApp.spectrum.levels.length
                                                                            ? Math.max(0, Math.min(1, airPodsTrayApp.spectrum.levels[index])) : 0
                                            width: (spectrumStrip.width - (23 * 2)) / 24
                                            height: spectrumStrip.height
                                            anchors.bottom: parent.bottom

                                            // Peak-hold level: chases up instantly, decays a
                                            // step per update — classic analyzer cap dot.
                                            property real peak: 0
                                            onLevelChanged: peak = Math.max(level, peak * 0.982)

                                            Rectangle {
                                                anchors.bottom: parent.bottom
                                                width: parent.width
                                                height: Math.max(2, parent.height * parent.level)
                                                radius: width / 2
                                                gradient: Gradient {
                                                    GradientStop { position: 0.0; color: mainWindow.accentColor }
                                                    GradientStop { position: 1.0; color: Qt.rgba(mainWindow.accentColor.r, mainWindow.accentColor.g, mainWindow.accentColor.b, 0.35) }
                                                }
                                            }
                                            // peak dot
                                            Rectangle {
                                                width: parent.width
                                                height: 3
                                                radius: 1.5
                                                y: Math.max(0, parent.height * (1 - parent.peak) - height)
                                                visible: parent.peak > 0.03
                                                color: Qt.lighter(mainWindow.accentColor, 1.3)
                                                opacity: 0.95
                                            }
                                        }
                                    }
                                }
                                Text {
                                    anchors.centerIn: parent
                                    text: qsTr("No audio playing")
                                    color: mainWindow.textSecondary
                                    font.pixelSize: 10
                                    visible: !airPodsTrayApp.spectrum.levels.some(v => v > 0.02)
                                }

                                // EQ response overlay: the 3-band gain curve
                                // drawn over the bars so a boost/cut is visible
                                // where it lands in the spectrum. 50 = flat.
                                Canvas {
                                    id: eqCurve
                                    anchors.fill: parent
                                    opacity: airPodsTrayApp.deviceInfo.customEqEnabled ? 1 : 0.3
                                    // live slider values so the curve tracks
                                    // during drags, not just on release
                                    property real low: eqLow.value
                                    property real mid: eqMid.value
                                    property real high: eqHigh.value
                                    onLowChanged: requestPaint()
                                    onMidChanged: requestPaint()
                                    onHighChanged: requestPaint()
                                    onWidthChanged: requestPaint()
                                    onPaint: {
                                        const ctx = getContext("2d")
                                        ctx.reset()
                                        // Gaussian peaks: low centered ~bar 4,
                                        // mid ~bar 12, high ~bar 19. Gain delta
                                        // (band-50)/50 → -1..+1, inverted so
                                        // up = boost (matches slider up).
                                        const bands = [
                                            { c: 4,  s: 4.2, g: (low - 50) / 50 },
                                            { c: 12, s: 4.6, g: (mid - 50) / 50 },
                                            { c: 19, s: 4.2, g: (high - 50) / 50 },
                                        ]
                                        const N = 24, midY = height * 0.62, amp = height * 0.55
                                        // neutral reference line
                                        ctx.strokeStyle = mainWindow.textSecondary
                                        ctx.globalAlpha = 0.35
                                        ctx.lineWidth = 1
                                        ctx.setLineDash([2, 3])
                                        ctx.beginPath()
                                        ctx.moveTo(0, midY); ctx.lineTo(width, midY)
                                        ctx.stroke()
                                        ctx.setLineDash([])
                                        ctx.globalAlpha = 1

                                        // response curve
                                        ctx.strokeStyle = mainWindow.accentColor
                                        ctx.lineWidth = 1.6
                                        ctx.beginPath()
                                        for (let i = 0; i < N; ++i) {
                                            let r = 0
                                            for (const b of bands) {
                                                const d = (i - b.c) / b.s
                                                r += b.g * Math.exp(-d * d)
                                            }
                                            const x = (i + 0.5) / N * width
                                            const y = midY - r * amp
                                            if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y)
                                        }
                                        ctx.stroke()
                                    }
                                }
                            }

                            // Quick presets — write all three bands through
                            // the same packet path the sliders use, and the
                            // bound slider values follow the echo back.
                            Flow {
                                width: eqCol.width
                                spacing: 6
                                Repeater {
                                    model: [
                                        { label: qsTr("Flat"),   low: 50, mid: 50, high: 50 },
                                        { label: qsTr("Bass"),   low: 80, mid: 48, high: 40 },
                                        { label: qsTr("Vocal"),  low: 44, mid: 74, high: 56 },
                                        { label: qsTr("Treble"), low: 38, mid: 50, high: 80 }
                                    ]
                                    delegate: Rectangle {
                                        required property var modelData
                                        implicitHeight: 22
                                        implicitWidth: presetText.implicitWidth + 18
                                        radius: 11
                                        color: presetMouse.containsMouse
                                               ? Qt.lighter(mainWindow.cardColor, 1.08)
                                               : mainWindow.surfaceColor
                                        border.color: presetMouse.containsMouse
                                                      ? Qt.lighter(mainWindow.borderColor, 1.25)
                                                      : mainWindow.borderColor
                                        Behavior on color { ColorAnimation { duration: 110 } }
                                        Behavior on border.color { ColorAnimation { duration: 110 } }
                                        Text {
                                            id: presetText
                                            anchors.centerIn: parent
                                            text: modelData.label
                                            color: mainWindow.textPrimary
                                            font.pixelSize: 11
                                        }
                                        MouseArea {
                                            id: presetMouse
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: airPodsTrayApp.setCustomEqBand(
                                                           modelData.low, modelData.mid, modelData.high)
                                        }
                                    }
                                }
                            }

                            // Mixer-style band rows: label — accent slider — value
                            RowLayout {
                                width: eqCol.width
                                spacing: 10
                                Text { text: qsTr("Low"); color: mainWindow.textPrimary; font.pixelSize: 12; Layout.preferredWidth: 36 }
                                StyledSlider {
                                    id: eqLow
                                    Layout.fillWidth: true
                                    from: 0; to: 100; stepSize: 1
                                    enabled: airPodsTrayApp.deviceInfo.customEqEnabled
                                    value: airPodsTrayApp.deviceInfo.customEqLow
                                    onPressedChanged: if (!pressed)
                                        airPodsTrayApp.setCustomEqBand(value, eqMid.value, eqHigh.value)
                                }
                                Text {
                                    text: Math.round(eqLow.value)
                                    color: mainWindow.textSecondary; font.pixelSize: 11
                                    horizontalAlignment: Text.AlignRight
                                    Layout.preferredWidth: 26
                                }
                            }
                            RowLayout {
                                width: eqCol.width
                                spacing: 10
                                Text { text: qsTr("Mid"); color: mainWindow.textPrimary; font.pixelSize: 12; Layout.preferredWidth: 36 }
                                StyledSlider {
                                    id: eqMid
                                    Layout.fillWidth: true
                                    from: 0; to: 100; stepSize: 1
                                    enabled: airPodsTrayApp.deviceInfo.customEqEnabled
                                    value: airPodsTrayApp.deviceInfo.customEqMid
                                    onPressedChanged: if (!pressed)
                                        airPodsTrayApp.setCustomEqBand(eqLow.value, value, eqHigh.value)
                                }
                                Text {
                                    text: Math.round(eqMid.value)
                                    color: mainWindow.textSecondary; font.pixelSize: 11
                                    horizontalAlignment: Text.AlignRight
                                    Layout.preferredWidth: 26
                                }
                            }
                            RowLayout {
                                width: eqCol.width
                                spacing: 10
                                Text { text: qsTr("High"); color: mainWindow.textPrimary; font.pixelSize: 12; Layout.preferredWidth: 36 }
                                StyledSlider {
                                    id: eqHigh
                                    Layout.fillWidth: true
                                    from: 0; to: 100; stepSize: 1
                                    enabled: airPodsTrayApp.deviceInfo.customEqEnabled
                                    value: airPodsTrayApp.deviceInfo.customEqHigh
                                    onPressedChanged: if (!pressed)
                                        airPodsTrayApp.setCustomEqBand(eqLow.value, eqMid.value, value)
                                }
                                Text {
                                    text: Math.round(eqHigh.value)
                                    color: mainWindow.textSecondary; font.pixelSize: 11
                                    horizontalAlignment: Text.AlignRight
                                    Layout.preferredWidth: 26
                                }
                            }
                        }
                    }

                    SectionHeader { label: qsTr("HEARING AID"); visible: airPodsTrayApp.airpodsConnected && airPodsTrayApp.deviceInfo.supportsHearingAid }

                    Rectangle {
                        width: parent.width - parent.padding * 2
                        height: hearingCol.height + 16
                        visible: airPodsTrayApp.airpodsConnected && airPodsTrayApp.deviceInfo.supportsHearingAid
                        radius: 8; color: mainWindow.cardColor; gradient: mainWindow.cardGradient; border.color: mainWindow.borderColor; border.width: 1
                        CardShine {}
                        Column {
                            id: hearingCol
                            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 8 }
                            spacing: 10

                            // Status row — shown when the ATT channel hasn't
                            // delivered the hearing-aid characteristic, so the
                            // feature's absence is explainable instead of silent.
                            Text {
                                width: parent.width
                                wrapMode: Text.WordWrap
                                visible: !airPodsTrayApp.hearingAidModel.available
                                color: mainWindow.textSecondary; font.pixelSize: 11
                                text: {
                                    if (airPodsTrayApp.attState === "opening")
                                        return qsTr("Hearing aid service: connecting…");
                                    if (airPodsTrayApp.attState === "failed")
                                        return airPodsTrayApp.attError !== ""
                                            ? qsTr("Hearing aid unavailable — %1").arg(airPodsTrayApp.attError)
                                            : qsTr("Hearing aid unavailable — could not reach the pods' ATT service");
                                    return qsTr("Hearing aid service unavailable on this device");
                                }
                            }

                            Repeater {
                                model: [
                                    { label: qsTr("Amplification"), prop: "amplification", lo: -1.0, hi: 1.0 },
                                    { label: qsTr("Balance"), prop: "balance", lo: -1.0, hi: 1.0 },
                                    { label: qsTr("Tone"), prop: "tone", lo: -1.0, hi: 1.0 },
                                    { label: qsTr("Ambient noise reduction"), prop: "ambientNoiseReduction", lo: 0.0, hi: 1.0 },
                                    { label: qsTr("Own voice amplification"), prop: "ownVoiceAmplification", lo: -1.0, hi: 1.0 }
                                ]
                                delegate: Column {
                                    required property var modelData
                                    width: hearingCol.width
                                    spacing: 2
                                    visible: airPodsTrayApp.hearingAidModel.available
                                    RowLayout {
                                        width: parent.width
                                        Text { text: modelData.label; color: mainWindow.textPrimary; font.pixelSize: 12; Layout.fillWidth: true }
                                        Text {
                                            text: (bandSlider.value >= 0 && modelData.lo < 0 ? "+" : "") + bandSlider.value.toFixed(2)
                                            color: mainWindow.textSecondary; font.pixelSize: 11
                                        }
                                    }
                                    StyledSlider {
                                        id: bandSlider
                                        width: parent.width
                                        bipolar: modelData.lo < 0
                                        from: modelData.lo; to: modelData.hi; stepSize: 0.01
                                        value: airPodsTrayApp.hearingAidModel[modelData.prop]
                                        onMoved: airPodsTrayApp.hearingAidModel[modelData.prop] = value
                                    }
                                }
                            }

                            RowLayout {
                                width: parent.width
                                visible: airPodsTrayApp.hearingAidModel.available
                                Text { text: qsTr("Conversation boost"); color: mainWindow.textPrimary; font.pixelSize: 12; Layout.fillWidth: true }
                                StyledSwitch {
                                    checked: airPodsTrayApp.hearingAidModel.conversationBoost
                                    onToggled: airPodsTrayApp.hearingAidModel.conversationBoost = checked
                                }
                            }

                            RowLayout {
                                width: parent.width
                                Column {
                                    Layout.fillWidth: true
                                    Text { text: qsTr("Loud sound reduction"); color: mainWindow.textPrimary; font.pixelSize: 12 }
                                    Text { text: qsTr("Pods clamp sudden loud transients"); color: mainWindow.textSecondary; font.pixelSize: 10 }
                                }
                                StyledSwitch {
                                    checked: airPodsTrayApp.deviceInfo.loudSoundReduction
                                    onToggled: airPodsTrayApp.setLoudSoundReduction(checked)
                                }
                            }

                            RowLayout {
                                width: parent.width
                                spacing: 10
                                enabled: airPodsTrayApp.deviceInfo.loudSoundReduction
                                opacity: enabled ? 1.0 : 0.45
                                Text { text: qsTr("Cap level"); color: mainWindow.textPrimary; font.pixelSize: 12; Layout.preferredWidth: 62 }
                                StyledSlider {
                                    Layout.fillWidth: true
                                    from: 0; to: 100; stepSize: 1
                                    value: airPodsTrayApp.deviceInfo.ppeCapLevel
                                    onPressedChanged: if (!pressed) airPodsTrayApp.setPpeCapLevel(value)
                                }
                                Text {
                                    text: Math.round(airPodsTrayApp.deviceInfo.ppeCapLevel)
                                    color: mainWindow.textSecondary; font.pixelSize: 11
                                    horizontalAlignment: Text.AlignRight
                                    Layout.preferredWidth: 26
                                }
                            }

                            RowLayout {
                                width: parent.width
                                Column {
                                    Layout.fillWidth: true
                                    Text { text: qsTr("Gain swipe"); color: mainWindow.textPrimary; font.pixelSize: 12 }
                                    Text { text: qsTr("Stem swipe adjusts hearing-aid gain"); color: mainWindow.textSecondary; font.pixelSize: 10 }
                                }
                                StyledSwitch {
                                    checked: airPodsTrayApp.deviceInfo.hearingGainSwipe
                                    onToggled: airPodsTrayApp.setHearingGainSwipe(checked)
                                }
                            }
                        }
                    }

                    SectionHeader { label: qsTr("SOUNDS"); visible: airPodsTrayApp.airpodsConnected && airPodsTrayApp.deviceInfo.supportsCaseSpeaker }

                    Rectangle {
                        width: parent.width - parent.padding * 2
                        height: soundsCol.height + 16
                        visible: airPodsTrayApp.airpodsConnected && airPodsTrayApp.deviceInfo.supportsCaseSpeaker
                        radius: 8; color: mainWindow.cardColor; gradient: mainWindow.cardGradient; border.color: mainWindow.borderColor; border.width: 1
                        CardShine {}
                        Column {
                            id: soundsCol
                            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 8 }
                            spacing: 10

                            RowLayout {
                                width: parent.width
                                Column {
                                    Layout.fillWidth: true
                                    Text { text: qsTr("In-case tone"); color: mainWindow.textPrimary; font.pixelSize: 12 }
                                    Text { text: qsTr("Case speaker plays sounds (Find My)"); color: mainWindow.textSecondary; font.pixelSize: 10 }
                                }
                                StyledSwitch {
                                    checked: airPodsTrayApp.deviceInfo.inCaseTone
                                    onToggled: airPodsTrayApp.setInCaseTone(checked)
                                }
                            }

                            RowLayout {
                                width: parent.width
                                Column {
                                    Layout.fillWidth: true
                                    Text { text: qsTr("Siri chime"); color: mainWindow.textPrimary; font.pixelSize: 12 }
                                    Text { text: qsTr("Pods play a tone for Siri interactions"); color: mainWindow.textSecondary; font.pixelSize: 10 }
                                }
                                StyledSwitch {
                                    checked: airPodsTrayApp.deviceInfo.siriMultitone
                                    onToggled: airPodsTrayApp.setSiriMultitone(checked)
                                }
                            }

                            RowLayout {
                                width: parent.width
                                spacing: 10
                                Text { text: qsTr("Chime volume"); color: mainWindow.textPrimary; font.pixelSize: 12; Layout.preferredWidth: 78 }
                                StyledSlider {
                                    Layout.fillWidth: true
                                    from: 0; to: 100; stepSize: 1
                                    value: airPodsTrayApp.deviceInfo.chimeVolume
                                    onPressedChanged: if (!pressed) airPodsTrayApp.setChimeVolume(Math.round(value))
                                }
                                Text {
                                    text: airPodsTrayApp.deviceInfo.chimeVolume
                                    color: mainWindow.textSecondary; font.pixelSize: 11
                                    horizontalAlignment: Text.AlignRight
                                    Layout.preferredWidth: 26
                                }
                            }
                        }
                    }

                    SectionHeader { label: qsTr("CONNECTIVITY") }

                    Rectangle {
                        width: parent.width - parent.padding * 2
                        height: connCol.height + 16
                        radius: 8; color: mainWindow.cardColor; gradient: mainWindow.cardGradient; border.color: mainWindow.borderColor; border.width: 1
                        CardShine {}
                        Column {
                            id: connCol
                            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 8 }
                            spacing: 10

                            RowLayout {
                                width: parent.width
                                Text { text: qsTr("Android cross-device"); color: mainWindow.textPrimary; font.pixelSize: 12; Layout.fillWidth: true }
                                StyledSwitch {
                                    checked: airPodsTrayApp.crossDeviceEnabled
                                    onToggled: airPodsTrayApp.setCrossDeviceEnabled(checked)
                                }
                            }

                            RowLayout {
                                width: parent.width
                                Text { text: qsTr("Bluetooth retries"); color: mainWindow.textPrimary; font.pixelSize: 12; Layout.fillWidth: true }
                                StyledSpinBox {
                                    from: 1; to: 10
                                    value: airPodsTrayApp.retryAttempts
                                    onValueChanged: airPodsTrayApp.retryAttempts = value
                                }
                            }

                            RowLayout {
                                width: parent.width
                                visible: airPodsTrayApp.airpodsConnected
                                Column {
                                    Layout.fillWidth: true
                                    Text { text: qsTr("Automatic connection"); color: mainWindow.textPrimary; font.pixelSize: 12 }
                                    Text { text: qsTr("Pods connect to this host when in range"); color: mainWindow.textSecondary; font.pixelSize: 10 }
                                }
                                StyledSwitch {
                                    checked: airPodsTrayApp.deviceInfo.automaticConnection
                                    onToggled: airPodsTrayApp.setAutomaticConnection(checked)
                                }
                            }

                            RowLayout {
                                width: parent.width
                                StyledTextField {
                                    id: newPhoneMacField
                                    Layout.fillWidth: true
                                    placeholderText: (PHONE_MAC_ADDRESS !== "" ? PHONE_MAC_ADDRESS : "00:00:00:00:00:00")
                                    maximumLength: 32
                                    font.pixelSize: 11
                                }
                                StyledButton {
                                    text: qsTr("Phone MAC")
                                    onClicked: airPodsTrayApp.setPhoneMac(newPhoneMacField.text)
                                }
                            }

                            // TiPi multidevice session — hosts currently
                            // attached to the pods, plus the takeover nudge.
                            Column {
                                width: parent.width
                                spacing: 4
                                visible: airPodsTrayApp.airpodsConnected
                                Text { text: qsTr("Session devices"); color: mainWindow.textPrimary; font.pixelSize: 12 }
                                Text {
                                    width: parent.width
                                    wrapMode: Text.WordWrap
                                    color: mainWindow.textSecondary; font.pixelSize: 11
                                    text: airPodsTrayApp.deviceInfo.connectedDevices.length > 0
                                        ? airPodsTrayApp.deviceInfo.connectedDevices.join(", ")
                                        : qsTr("No other devices in the session")
                                }
                                StyledButton {
                                    visible: airPodsTrayApp.deviceInfo.connectedDevices.length > 1
                                    text: qsTr("Take over audio")
                                    onClicked: airPodsTrayApp.takeOverAudio()
                                }
                            }
                        }
                    }

                    SectionHeader { label: qsTr("SYSTEM") }

                    Rectangle {
                        width: parent.width - parent.padding * 2
                        height: sysCol.height + 16
                        radius: 8; color: mainWindow.cardColor; gradient: mainWindow.cardGradient; border.color: mainWindow.borderColor; border.width: 1
                        CardShine {}
                        Column {
                            id: sysCol
                            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 8 }
                            spacing: 10

                            RowLayout {
                                width: parent.width
                                Text { text: qsTr("Start on login"); color: mainWindow.textPrimary; font.pixelSize: 12; Layout.fillWidth: true }
                                StyledSwitch {
                                    checked: airPodsTrayApp.autoStartManager.autoStartEnabled
                                    onToggled: airPodsTrayApp.autoStartManager.autoStartEnabled = checked
                                }
                            }

                            RowLayout {
                                width: parent.width
                                Column {
                                    Layout.fillWidth: true
                                    Text { text: qsTr("Popup on ear events"); color: mainWindow.textPrimary; font.pixelSize: 12 }
                                    Text { text: qsTr("Flash the window when a pod leaves your ear or the case"); color: mainWindow.textSecondary; font.pixelSize: 10 }
                                }
                                StyledSwitch {
                                    checked: airPodsTrayApp.earPopupEnabled
                                    onToggled: airPodsTrayApp.earPopupEnabled = checked
                                }
                            }

                            RowLayout {
                                width: parent.width
                                visible: airPodsTrayApp.earPopupEnabled
                                Text { text: qsTr("Popup seconds"); color: mainWindow.textPrimary; font.pixelSize: 12; Layout.fillWidth: true }
                                StyledSpinBox {
                                    from: 2; to: 30
                                    value: airPodsTrayApp.earPopupSecs
                                    onValueChanged: airPodsTrayApp.earPopupSecs = value
                                }
                            }

                            RowLayout {
                                width: parent.width
                                visible: airPodsTrayApp.airpodsConnected && airPodsTrayApp.deviceInfo.supportsCaseBattery
                                Column {
                                    Layout.fillWidth: true
                                    Text { text: qsTr("Optimized charging"); color: mainWindow.textPrimary; font.pixelSize: 12 }
                                    Text { text: qsTr("Pods learn your routine and delay charging past 80%"); color: mainWindow.textSecondary; font.pixelSize: 10 }
                                }
                                StyledSwitch {
                                    checked: airPodsTrayApp.deviceInfo.optimizedCharging
                                    onToggled: airPodsTrayApp.setOptimizedCharging(checked)
                                }
                            }

                            RowLayout {
                                width: parent.width
                                visible: airPodsTrayApp.airpodsConnected
                                StyledTextField {
                                    id: newNameField
                                    Layout.fillWidth: true
                                    placeholderText: airPodsTrayApp.deviceInfo.deviceName
                                    maximumLength: 32
                                    font.pixelSize: 11
                                }
                                StyledButton {
                                    text: qsTr("Rename")
                                    onClicked: airPodsTrayApp.renameAirPods(newNameField.text)
                                }
                            }

                            StyledButton {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: qsTr("Show Proximity Keys QR")
                                onClicked: keysQrDialog.show()
                            }
                        }
                    }

                    SectionHeader { label: qsTr("ABOUT") }

                    Rectangle {
                        width: parent.width - parent.padding * 2
                        height: aboutCol.height + 16
                        radius: 8; color: mainWindow.cardColor; gradient: mainWindow.cardGradient; border.color: mainWindow.borderColor; border.width: 1
                        CardShine {}
                        Column {
                            id: aboutCol
                            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 8 }
                            spacing: 6

                            Repeater {
                                model: [
                                    [qsTr("Model number"), airPodsTrayApp.deviceInfo.modelNumber],
                                    [qsTr("Serial number"), airPodsTrayApp.deviceInfo.serialNumber],
                                    [qsTr("Left serial"), airPodsTrayApp.deviceInfo.leftSerialNumber],
                                    [qsTr("Right serial"), airPodsTrayApp.deviceInfo.rightSerialNumber],
                                    [qsTr("Firmware"), airPodsTrayApp.deviceInfo.firmwareVersion],
                                    [qsTr("Hardware revision"), airPodsTrayApp.deviceInfo.hardwareRevision],
                                    [qsTr("Bluetooth address"), airPodsTrayApp.deviceInfo.bluetoothAddress]
                                ]
                                delegate: RowLayout {
                                    required property var modelData
                                    width: aboutCol.width
                                    visible: modelData[1] !== ""
                                    Text { text: modelData[0]; color: mainWindow.textSecondary; font.pixelSize: 11; Layout.fillWidth: true }
                                    TextInput {
                                        text: modelData[1]
                                        readOnly: true
                                        selectByMouse: true
                                        color: mainWindow.textPrimary
                                        font.pixelSize: 11
                                        selectionColor: mainWindow.accentColor
                                    }
                                }
                            }

                            RowLayout {
                                width: parent.width
                                visible: airPodsTrayApp.deviceInfo.headTrackingEnabled && airPodsTrayApp.airpodsConnected && airPodsTrayApp.deviceInfo.supportsHeadTracking
                                Text { text: qsTr("Head tracking"); color: mainWindow.textSecondary; font.pixelSize: 11; Layout.fillWidth: true }
                                Text {
                                    text: "yaw " + airPodsTrayApp.deviceInfo.headYaw + " · pitch " + airPodsTrayApp.deviceInfo.headPitch
                                    color: mainWindow.textPrimary
                                    font.pixelSize: 11
                                }
                            }

                            Item {
                                width: aboutCol.width
                                height: coverageBtn.height
                                StyledButton {
                                    id: coverageBtn
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    text: qsTr("Check coverage")
                                    onClicked: Qt.openUrlExternally("https://checkcoverage.apple.com")
                                }
                            }
                        }
                    }

                    KeysQRDialog {
                        id: keysQrDialog
                        encKey: airPodsTrayApp.deviceInfo.magicAccEncKey
                        irk: airPodsTrayApp.deviceInfo.magicAccIRK
                    }
                }
            }
        }
    }
}
